// SPDX-License-Identifier: GPL-2.0
/*
 * ssdpager: SSD-backed paging of private anonymous memory without swap.
 *
 * The SSD is registered as MEMORY_DEVICE_PRIVATE "device memory": slot i on
 * the block device is device-private pfn base+i.  Eviction and promotion use
 * migrate_vma_*(); demand paging arrives through dev_pagemap_ops.migrate_to_ram.
 * Only exported symbols are used.  Policy lives in user space (ioctl).
 *
 * Slots are handed out as contiguous runs so that pages evicted together sit
 * together on disk.  Every demand fault, and every access that had to wait
 * for an in-flight batch, is logged to a ring that user space drains.
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/memremap.h>
#include <linux/migrate.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/crc32c.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/uaccess.h>
#include <linux/bitmap.h>
#include <linux/kprobes.h>
#include <linux/pagewalk.h>
#include <linux/swapops.h>
#include <linux/vmalloc.h>
#include "ssdpager.h"

#define SP_CHUNK_SIZE	(1UL << 27)		/* one arm64 4K section */
#define SP_CHUNK_PAGES	(SP_CHUNK_SIZE >> PAGE_SHIFT)
#define SP_BATCH	256
#define SP_PA_FLOOR	(1ULL << 44)
#define SP_CRC_VALID	(1UL << 32)
#define SP_FAULT_BUSY	33			/* bit in zone_device_data */
#define SP_FAULT_TRIES	64
#define SP_HIST		24
#define SP_VEC_MAX	(1U << 16)
#define SP_TRACE_SIZE	(1U << 16)		/* events, power of two */
#define SP_TRACE_READ	4096U

static char *dev;
module_param(dev, charp, 0444);
MODULE_PARM_DESC(dev, "backing block device (used exclusively, contents destroyed)");
static int inject_eio, inject_crc;
module_param(inject_eio, int, 0644);
MODULE_PARM_DESC(inject_eio, "fail the next N page reads with EIO");
module_param(inject_crc, int, 0644);
MODULE_PARM_DESC(inject_crc, "treat the next N page reads as checksum mismatch");

struct sp_chunk {
	struct dev_pagemap pgmap;
	unsigned long base_pfn;
	unsigned long slot_base;
	unsigned long *map;		/* one bit per slot, set = in use */
	unsigned int nfree;
};

struct sp_batch;
struct sp_ent {
	struct sp_batch *b;
	blk_status_t st;
};

/*
 * Up to SP_BATCH pages in up to SP_BATCH virtually contiguous runs.  All runs
 * are unmapped first, their IO is submitted together, then all are remapped.
 */
struct sp_batch {
	struct list_head link;		/* sp_inflight */
	struct mm_struct *mm;
	bool evict;
	unsigned int nr_runs, nr_pages;
	atomic_t pending;
	struct completion done;
	struct sp_ent e[SP_BATCH];
	unsigned long src[SP_BATCH];
	unsigned long dst[SP_BATCH];
	struct page *pg[SP_BATCH];	/* page we allocated for entry i */
	struct migrate_vma run[SP_BATCH];	/* .vma == NULL: setup failed */
};

static int sp_owner;
static struct bdev_handle *sp_bh;
static struct sp_chunk **sp_chunks;
static unsigned int sp_nr_chunks, sp_max_chunks;
static phys_addr_t sp_next_pa;
static DEFINE_MUTEX(sp_grow_lock);
static DEFINE_SPINLOCK(sp_slot_lock);	/* bitmaps, nfree, rotor, nr_chunks */
static unsigned long sp_rotor;		/* next-fit cursor, a slot number */

static DEFINE_SPINLOCK(sp_inflight_lock);
static LIST_HEAD(sp_inflight);

static DEFINE_SPINLOCK(sp_trace_lock);
static struct sp_event *sp_trace;
static u64 sp_trace_head, sp_trace_tail, sp_trace_lost;

static atomic_long_t st_used, st_evict, st_promote, st_fault, st_evac;
static atomic_long_t st_late, st_evict_wait, st_skip, st_dropped, st_fault_busy;
static atomic_long_t st_wr_err, st_rd_err, st_crc_err, st_nospc;
static atomic64_t ft_setup, ft_alloc, ft_io, ft_fin, ft_hist[SP_HIST];

static unsigned long sp_slot(struct page *dpage)
{
	struct sp_chunk *c = container_of(dpage->pgmap, struct sp_chunk, pgmap);

	return c->slot_base + page_to_pfn(dpage) - c->base_pfn;
}

static sector_t sp_sector(struct page *dpage)
{
	return (sector_t)sp_slot(dpage) << (PAGE_SHIFT - SECTOR_SHIFT);
}

static u32 sp_crc(struct page *page)
{
	void *p = kmap_local_page(page);
	u32 c = crc32c(0, p, PAGE_SIZE);

	kunmap_local(p);
	return c;
}

/* ---- event trace ----------------------------------------------------- */

/* Any context.  The oldest event is overwritten when nobody drains. */
static void sp_trace_push(u32 type, unsigned long addr, u64 ts, u64 lat)
{
	unsigned long flags;
	struct sp_event *e;

	spin_lock_irqsave(&sp_trace_lock, flags);
	if (sp_trace_head - sp_trace_tail == SP_TRACE_SIZE) {
		sp_trace_tail++;
		sp_trace_lost++;
	}
	e = &sp_trace[sp_trace_head++ & (SP_TRACE_SIZE - 1)];
	e->ts_ns = ts;
	e->addr = addr;
	e->pid = current->tgid;
	e->type = type;
	e->lat_ns = lat;
	spin_unlock_irqrestore(&sp_trace_lock, flags);
}

/* ---- slot allocator -------------------------------------------------- */

static struct page *sp_slot_page(unsigned long slot)
{
	struct sp_chunk *c = sp_chunks[slot / SP_CHUNK_PAGES];

	return pfn_to_page(c->base_pfn + slot % SP_CHUNK_PAGES);
}

static void sp_page_free(struct page *page)
{
	struct sp_chunk *c = container_of(page->pgmap, struct sp_chunk, pgmap);

	spin_lock(&sp_slot_lock);
	__clear_bit(page_to_pfn(page) - c->base_pfn, c->map);
	c->nfree++;
	spin_unlock(&sp_slot_lock);
	atomic_long_dec(&st_used);
	module_put(THIS_MODULE);
}

static vm_fault_t sp_migrate_to_ram(struct vm_fault *vmf);

static const struct dev_pagemap_ops sp_pgmap_ops = {
	.page_free = sp_page_free,
	.migrate_to_ram = sp_migrate_to_ram,
};

/*
 * request_free_mem_region() cannot be used on arm64 here: it allocates down
 * from PHYSMEM_END (2^48), beyond what vmemmap covers.  Pick a free physical
 * range below the end of the linear map ourselves.
 */
static int sp_grow(void)
{
	struct sp_chunk *c;
	struct resource *res = NULL;
	void *p;

	if (sp_nr_chunks >= sp_max_chunks)
		return -ENOSPC;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (c)
		c->map = bitmap_zalloc(SP_CHUNK_PAGES, GFP_KERNEL);
	if (!c || !c->map) {
		kfree(c);
		return -ENOMEM;
	}
	if (!sp_next_pa)
		sp_next_pa = ALIGN_DOWN((phys_addr_t)memstart_addr +
					(PAGE_END - PAGE_OFFSET), SP_CHUNK_SIZE);
	while (!res && sp_next_pa > SP_PA_FLOOR) {
		sp_next_pa -= SP_CHUNK_SIZE;
		res = request_mem_region(sp_next_pa, SP_CHUNK_SIZE, "ssdpager");
	}
	if (!res) {
		bitmap_free(c->map);
		kfree(c);
		return -ENOSPC;
	}
	c->pgmap.type = MEMORY_DEVICE_PRIVATE;
	c->pgmap.range.start = res->start;
	c->pgmap.range.end = res->end;
	c->pgmap.nr_range = 1;
	c->pgmap.ops = &sp_pgmap_ops;
	c->pgmap.owner = &sp_owner;
	p = memremap_pages(&c->pgmap, numa_node_id());
	if (IS_ERR(p)) {
		release_mem_region(res->start, SP_CHUNK_SIZE);
		bitmap_free(c->map);
		kfree(c);
		return PTR_ERR(p);
	}
	c->base_pfn = PHYS_PFN(res->start);
	c->slot_base = (unsigned long)sp_nr_chunks * SP_CHUNK_PAGES;
	c->nfree = SP_CHUNK_PAGES;

	spin_lock(&sp_slot_lock);
	sp_chunks[sp_nr_chunks++] = c;
	spin_unlock(&sp_slot_lock);
	return 0;
}

/*
 * Next-fit from the rotor.  @exact: only a run of exactly @want free slots
 * will do; otherwise take the first free slot and as many as follow it, up
 * to @want.  Returns the run length (0 = nothing) and its first slot.
 */
static unsigned int sp_take_run(unsigned int want, unsigned long *first,
				bool exact)
{
	unsigned int nr, rc, k, n = 0;
	unsigned long off;

	spin_lock(&sp_slot_lock);
	nr = sp_nr_chunks;
	rc = sp_rotor / SP_CHUNK_PAGES;
	off = sp_rotor % SP_CHUNK_PAGES;
	if (rc >= nr)
		rc = off = 0;
	/* k == nr revisits the part of the rotor's chunk below the rotor */
	for (k = 0; nr && k <= nr; k++, off = 0) {
		struct sp_chunk *c = sp_chunks[(rc + k) % nr];
		unsigned long idx, end;

		if (c->nfree < (exact ? want : 1))
			continue;
		if (exact) {
			idx = bitmap_find_next_zero_area(c->map, SP_CHUNK_PAGES,
							 off, want, 0);
			end = idx + want;
			if (end > SP_CHUNK_PAGES)
				continue;
		} else {
			idx = find_next_zero_bit(c->map, SP_CHUNK_PAGES, off);
			if (idx >= SP_CHUNK_PAGES)
				continue;
			end = find_next_bit(c->map,
					    min(idx + want, SP_CHUNK_PAGES), idx);
		}
		n = end - idx;
		bitmap_set(c->map, idx, n);
		c->nfree -= n;
		*first = c->slot_base + idx;
		sp_rotor = *first + n;
		break;
	}
	spin_unlock(&sp_slot_lock);
	return n;
}

/*
 * Allocate up to @want consecutive slots, growing the device before settling
 * for a shorter run.  Each page of the run comes back locked with one
 * reference.  Returns the run length, 0 when the device is full.
 */
static unsigned int sp_alloc_run(unsigned int want, unsigned long *first)
{
	unsigned int n = sp_take_run(want, first, true), i;

	if (!n) {
		mutex_lock(&sp_grow_lock);
		n = sp_take_run(want, first, true);
		if (!n && !sp_grow())
			n = sp_take_run(want, first, true);
		if (!n)
			n = sp_take_run(want, first, false);
		mutex_unlock(&sp_grow_lock);
	}
	if (!n) {
		atomic_long_inc(&st_nospc);
		return 0;
	}
	for (i = 0; i < n; i++) {
		struct page *p = sp_slot_page(*first + i);

		__module_get(THIS_MODULE);
		zone_device_page_init(p);
		p->zone_device_data = NULL;
	}
	atomic_long_add(n, &st_used);
	return n;
}

/* ---- block IO --------------------------------------------------------- */

static void sp_end_io(struct bio *bio)
{
	struct sp_ent *e = bio->bi_private;

	e->st = bio->bi_status;
	bio_put(bio);
	if (atomic_dec_and_test(&e->b->pending))
		complete(&e->b->done);
}

static void sp_batch_begin(struct sp_batch *b)
{
	atomic_set(&b->pending, 1);
	init_completion(&b->done);
}

static void sp_batch_submit(struct sp_batch *b, int i, blk_opf_t op,
			    struct page *mem, struct page *dpage)
{
	struct bio *bio = bio_alloc(sp_bh->bdev, 1, op, GFP_NOIO);

	bio->bi_iter.bi_sector = sp_sector(dpage);
	__bio_add_page(bio, mem, PAGE_SIZE, 0);
	bio->bi_end_io = sp_end_io;
	bio->bi_private = &b->e[i];
	b->e[i].b = b;
	atomic_inc(&b->pending);
	submit_bio(bio);
}

static void sp_batch_wait(struct sp_batch *b)
{
	if (!atomic_dec_and_test(&b->pending))
		wait_for_completion_io(&b->done);
}

/* Validate a page just read from @dpage's slot.  0 if good. */
static int sp_read_check(struct page *mem, struct page *dpage, blk_status_t st)
{
	unsigned long v = (unsigned long)dpage->zone_device_data;

	if (inject_eio > 0) {
		inject_eio--;
		st = BLK_STS_IOERR;
	}
	if (st) {
		atomic_long_inc(&st_rd_err);
		return -EIO;
	}
	if (inject_crc > 0) {
		inject_crc--;
		v ^= 1;
	}
	if (!(v & SP_CRC_VALID) || (u32)v != sp_crc(mem)) {
		atomic_long_inc(&st_crc_err);
		pr_err_ratelimited("ssdpager: checksum mismatch slot %lu\n",
				   sp_slot(dpage));
		return -EILSEQ;
	}
	return 0;
}

/* ---- batches: DRAM <-> SSD without a fault --------------------------- */

static void sp_batch_start(struct sp_batch *b, struct mm_struct *mm, bool evict)
{
	b->mm = mm;
	b->evict = evict;
	b->nr_runs = b->nr_pages = 0;
	spin_lock_irq(&sp_inflight_lock);
	list_add(&b->link, &sp_inflight);
	spin_unlock_irq(&sp_inflight_lock);
}

static bool sp_batch_full(struct sp_batch *b)
{
	return b->nr_pages == SP_BATCH || b->nr_runs == SP_BATCH;
}

/*
 * Unmap [start, end) of @vma into the batch: the pages collected are locked
 * and their PTEs are migration entries until sp_batch_run().  Caller holds
 * mmap_lock and has checked that the range fits.
 */
static void sp_batch_add(struct sp_batch *b, struct vm_area_struct *vma,
			 unsigned long start, unsigned long end)
{
	unsigned long n = (end - start) >> PAGE_SHIFT;
	struct migrate_vma *a = &b->run[b->nr_runs];
	unsigned int at = b->nr_pages;

	memset(&b->src[at], 0, n * sizeof(b->src[0]));
	memset(&b->dst[at], 0, n * sizeof(b->dst[0]));
	memset(&b->pg[at], 0, n * sizeof(b->pg[0]));
	*a = (struct migrate_vma){
		.vma = vma, .start = start, .end = end,
		.src = &b->src[at], .dst = &b->dst[at],
		.pgmap_owner = &sp_owner,
		.flags = b->evict ? MIGRATE_VMA_SELECT_SYSTEM :
				    MIGRATE_VMA_SELECT_DEVICE_PRIVATE,
	};
	/* visible to sp_wait_probe() before the first PTE changes */
	spin_lock_irq(&sp_inflight_lock);
	b->nr_runs++;
	b->nr_pages += n;
	spin_unlock_irq(&sp_inflight_lock);
	if (migrate_vma_setup(a))
		a->vma = NULL;
}

/* Queue the writes of an eviction batch; consecutive pages get consecutive slots. */
static void sp_batch_write(struct sp_batch *b)
{
	unsigned long slot = 0;
	unsigned int i, want = 0, have = 0;

	for (i = 0; i < b->nr_pages; i++) {
		struct page *spage = migrate_pfn_to_page(b->src[i]);

		/* holes, pages that could not be isolated, file and KSM pages stay */
		if (spage && (b->src[i] & MIGRATE_PFN_MIGRATE) &&
		    PageAnon(spage) && !PageKsm(spage)) {
			b->pg[i] = spage;
			want++;
		}
	}
	for (i = 0; i < b->nr_pages; i++) {
		struct page *spage = b->pg[i], *dpage;

		if (!spage)
			continue;
		b->pg[i] = NULL;
		if (!have && want)
			have = sp_alloc_run(want, &slot);
		if (!have) {
			want = 0;	/* device full: the rest stays in DRAM */
			continue;
		}
		dpage = sp_slot_page(slot++);
		have--;
		want--;
		dpage->zone_device_data = (void *)(SP_CRC_VALID | sp_crc(spage));
		b->pg[i] = dpage;
		sp_batch_submit(b, i, REQ_OP_WRITE, spage, dpage);
	}
}

/* Queue the reads of a promotion batch. */
static void sp_batch_read(struct sp_batch *b)
{
	unsigned int r;

	for (r = 0; r < b->nr_runs; r++) {
		struct migrate_vma *a = &b->run[r];
		unsigned long n = (a->end - a->start) >> PAGE_SHIFT, j;
		unsigned int at = a->src - b->src;

		if (!a->vma)
			continue;
		for (j = 0; j < n; j++) {
			struct page *dpage = migrate_pfn_to_page(a->src[j]);
			struct page *mem;

			if (!dpage || !(a->src[j] & MIGRATE_PFN_MIGRATE) ||
			    !is_device_private_page(dpage)) {
				atomic_long_inc(&st_skip);	/* not on the SSD */
				continue;
			}
			mem = alloc_page_vma(GFP_HIGHUSER_MOVABLE, a->vma,
					     a->start + (j << PAGE_SHIFT));
			if (!mem)
				continue;
			lock_page(mem);
			b->pg[at + j] = mem;
			sp_batch_submit(b, at + j, REQ_OP_READ, mem, dpage);
		}
	}
}

/* Do the IO for everything added since sp_batch_start() and remap it. */
static unsigned long sp_batch_run(struct sp_batch *b)
{
	unsigned long done = 0;
	struct blk_plug plug;
	unsigned int i, r;

	sp_batch_begin(b);
	blk_start_plug(&plug);
	if (b->evict)
		sp_batch_write(b);
	else
		sp_batch_read(b);
	blk_finish_plug(&plug);
	sp_batch_wait(b);

	for (i = 0; i < b->nr_pages; i++) {
		struct page *p = b->pg[i];
		int err;

		if (!p)
			continue;
		if (b->evict) {
			err = b->e[i].st ? -EIO : 0;
			if (err)
				atomic_long_inc(&st_wr_err);
		} else {
			err = sp_read_check(p, migrate_pfn_to_page(b->src[i]),
					    b->e[i].st);
		}
		if (err) {
			unlock_page(p);
			put_page(p);
			b->pg[i] = NULL;
			continue;
		}
		b->dst[i] = migrate_pfn(page_to_pfn(p)) |
			    (b->src[i] & MIGRATE_PFN_WRITE);
	}
	for (r = 0; r < b->nr_runs; r++)
		if (b->run[r].vma)
			migrate_vma_pages(&b->run[r]);
	for (i = 0; i < b->nr_pages; i++)
		if (b->pg[i] && (b->src[i] & MIGRATE_PFN_MIGRATE))
			done++;
	for (r = 0; r < b->nr_runs; r++)
		if (b->run[r].vma)
			migrate_vma_finalize(&b->run[r]);

	spin_lock_irq(&sp_inflight_lock);
	list_del(&b->link);
	spin_unlock_irq(&sp_inflight_lock);
	atomic_long_add(done, b->evict ? &st_evict : &st_promote);
	return done;
}

/*
 * kprobe on migration_entry_wait(mm, pmd, address): a thread touched a page
 * whose PTE is a migration entry.  If one of our batches holds that page, the
 * access is waiting for the whole batch: a prefetch that came too late, or an
 * eviction of a page that was not cold.
 */
static int sp_wait_probe(struct kprobe *kp, struct pt_regs *regs)
{
	struct mm_struct *mm = (struct mm_struct *)regs->regs[0];
	unsigned long addr = regs->regs[2], flags;
	struct sp_batch *b;
	int hit = 0;

	if (list_empty_careful(&sp_inflight))
		return 0;
	spin_lock_irqsave(&sp_inflight_lock, flags);
	list_for_each_entry(b, &sp_inflight, link) {
		unsigned int r;

		if (b->mm != mm)
			continue;
		for (r = 0; r < b->nr_runs && !hit; r++)
			if (addr >= b->run[r].start && addr < b->run[r].end)
				hit = b->evict ? SP_EV_EVICT_WAIT : SP_EV_LATE;
	}
	spin_unlock_irqrestore(&sp_inflight_lock, flags);
	if (hit) {
		atomic_long_inc(hit == SP_EV_LATE ? &st_late : &st_evict_wait);
		sp_trace_push(hit, addr & PAGE_MASK, ktime_get_ns(), 0);
	}
	return 0;
}

static struct kprobe sp_wait_kp = {
	.symbol_name = "migration_entry_wait",
	.pre_handler = sp_wait_probe,
};
static bool sp_wait_kp_on;

/* ---- demand paging ---------------------------------------------------- */

static int sp_read_sync(struct page *mem, struct page *dpage)
{
	struct bio_vec bv;
	struct bio bio;
	int err;

	bio_init(&bio, sp_bh->bdev, &bv, 1, REQ_OP_READ);
	bio.bi_iter.bi_sector = sp_sector(dpage);
	__bio_add_page(&bio, mem, PAGE_SIZE, 0);
	err = submit_bio_wait(&bio);
	bio_uninit(&bio);
	return sp_read_check(mem, dpage, err ? BLK_STS_IOERR : BLK_STS_OK);
}

/* Called from do_swap_page() with mmap_lock held for read; may sleep. */
static vm_fault_t sp_migrate_to_ram(struct vm_fault *vmf)
{
	unsigned long src = 0, dst = 0;
	struct migrate_vma args = {
		.vma = vmf->vma,
		.start = vmf->address, .end = vmf->address + PAGE_SIZE,
		.src = &src, .dst = &dst,
		.pgmap_owner = &sp_owner,
		.flags = MIGRATE_VMA_SELECT_DEVICE_PRIVATE,
		.fault_page = vmf->page,
	};
	unsigned long *gate = (unsigned long *)&vmf->page->zone_device_data;
	u64 t0 = ktime_get_ns(), t1, t2 = 0, t3 = 0, t4;
	struct page *dpage, *mem;
	vm_fault_t ret = 0;
	int tries;

	/*
	 * Threads faulting on the same page: do_swap_page() gives each of them
	 * a page reference, and migrate_vma_setup() refuses a page with
	 * references it cannot account for.  If all of them go on to
	 * migrate_vma_setup() nobody wins.  One thread per page proceeds; the
	 * others return at once, drop their reference and refault, which puts
	 * them to sleep on the winner's migration entry.
	 */
	if (test_and_set_bit(SP_FAULT_BUSY, gate)) {
		atomic_long_inc(&st_fault_busy);
		return 0;
	}
	for (tries = 0; ; tries++) {
		src = dst = 0;
		if (migrate_vma_setup(&args)) {
			ret = VM_FAULT_SIGBUS;
			goto out;
		}
		if ((src & MIGRATE_PFN_MIGRATE) || tries == SP_FAULT_TRIES)
			break;
		cpu_relax();
	}
	t1 = ktime_get_ns();

	dpage = migrate_pfn_to_page(src);
	if (dpage && (src & MIGRATE_PFN_MIGRATE)) {
		mem = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vmf->vma, vmf->address);
		if (!mem) {
			ret = VM_FAULT_OOM;
		} else {
			lock_page(mem);
			t2 = ktime_get_ns();
			if (sp_read_sync(mem, dpage)) {
				unlock_page(mem);
				put_page(mem);
				ret = VM_FAULT_SIGBUS;
			} else {
				dst = migrate_pfn(page_to_pfn(mem)) |
				      (src & MIGRATE_PFN_WRITE);
			}
			t3 = ktime_get_ns();
		}
	}
	migrate_vma_pages(&args);
	migrate_vma_finalize(&args);

	if (dst && (src & MIGRATE_PFN_MIGRATE)) {
		t4 = ktime_get_ns();
		atomic_long_inc(&st_fault);
		atomic64_add(t1 - t0, &ft_setup);
		atomic64_add(t2 - t1, &ft_alloc);
		atomic64_add(t3 - t2, &ft_io);
		atomic64_add(t4 - t3, &ft_fin);
		/* bucket = log2(total latency in us), clamped */
		atomic64_inc(&ft_hist[clamp(ilog2((t4 - t0) | 1) - 10, 0,
					    SP_HIST - 1)]);
		sp_trace_push(SP_EV_FAULT, vmf->address, t0, t4 - t0);
	}
out:
	/* vmf->page is still referenced by do_swap_page(), so not reused yet */
	clear_bit(SP_FAULT_BUSY, gate);
	return ret;
}

/* ---- evacuate everything (before unload) ----------------------------- */

static long sp_evacuate(void)
{
	unsigned long *src, *dst, i;
	unsigned int ci;
	long done = 0;

	src = kvcalloc(SP_CHUNK_PAGES, sizeof(*src), GFP_KERNEL);
	dst = kvcalloc(SP_CHUNK_PAGES, sizeof(*dst), GFP_KERNEL);
	if (!src || !dst) {
		kvfree(src);
		kvfree(dst);
		return -ENOMEM;
	}
	for (ci = 0; ci < READ_ONCE(sp_nr_chunks); ci++) {
		struct sp_chunk *c = sp_chunks[ci];

		memset(src, 0, SP_CHUNK_PAGES * sizeof(*src));
		memset(dst, 0, SP_CHUNK_PAGES * sizeof(*dst));
		migrate_device_range(src, c->base_pfn, SP_CHUNK_PAGES);
		for (i = 0; i < SP_CHUNK_PAGES; i++) {
			struct page *dpage = migrate_pfn_to_page(src[i]);
			struct page *mem;

			if (!dpage || !(src[i] & MIGRATE_PFN_MIGRATE))
				continue;
			mem = alloc_page(GFP_HIGHUSER_MOVABLE);
			if (!mem)
				continue;
			lock_page(mem);
			if (sp_read_sync(mem, dpage)) {
				unlock_page(mem);
				put_page(mem);
				continue;
			}
			dst[i] = migrate_pfn(page_to_pfn(mem)) |
				 (src[i] & MIGRATE_PFN_WRITE);
			done++;
			cond_resched();
		}
		migrate_device_pages(src, dst, SP_CHUNK_PAGES);
		migrate_device_finalize(src, dst, SP_CHUNK_PAGES);
	}
	kvfree(src);
	kvfree(dst);
	atomic_long_add(done, &st_evac);
	return done;
}

/* ---- control plane ---------------------------------------------------- */

static struct mm_struct *sp_get_mm(pid_t nr)
{
	struct pid *pid = find_get_pid(nr);
	struct task_struct *task = pid ? get_pid_task(pid, PIDTYPE_PID) : NULL;
	struct mm_struct *mm = NULL;

	put_pid(pid);
	if (task) {
		mm = get_task_mm(task);
		put_task_struct(task);
	}
	return mm;
}

static bool sp_vma_ok(struct vm_area_struct *vma, bool evict)
{
	return !(vma->vm_flags & (VM_SPECIAL | VM_HUGETLB | VM_SHARED)) &&
	       !vma_is_dax(vma) && !(evict && (vma->vm_flags & VM_LOCKED));
}

static long sp_do_range(bool evict, struct sp_range *r)
{
	unsigned long addr = r->addr & PAGE_MASK;
	unsigned long end = PAGE_ALIGN(r->addr + r->len);
	struct mm_struct *mm;
	struct sp_batch *b;
	long ret = 0;

	if (end <= addr)
		return -EINVAL;
	mm = sp_get_mm(r->pid);
	if (!mm)
		return -ESRCH;
	b = kvmalloc(sizeof(*b), GFP_KERNEL);
	if (!b) {
		mmput(mm);
		return -ENOMEM;
	}

	r->done = 0;
	while (addr < end) {
		struct vm_area_struct *vma;
		unsigned long next;

		/* Lock per batch so the target's mmap()/munmap() are not starved. */
		if (mmap_read_lock_killable(mm)) {
			ret = -EINTR;
			break;
		}
		vma = find_vma(mm, addr);
		if (!vma || vma->vm_start >= end) {
			mmap_read_unlock(mm);
			break;
		}
		addr = max(addr, vma->vm_start);
		next = min3(end, vma->vm_end, addr + SP_BATCH * PAGE_SIZE);
		if (!sp_vma_ok(vma, evict)) {
			next = vma->vm_end;
		} else {
			sp_batch_start(b, mm, evict);
			sp_batch_add(b, vma, addr, next);
			r->done += sp_batch_run(b);
		}
		mmap_read_unlock(mm);
		addr = next;
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}
		cond_resched();
	}
	kvfree(b);
	mmput(mm);
	return ret;
}

static u64 *sp_copy_addrs(u64 uptr, u32 nr)
{
	u64 *a;

	if (!nr || nr > SP_VEC_MAX)
		return ERR_PTR(-EINVAL);
	a = kvmalloc_array(nr, sizeof(*a), GFP_KERNEL);
	if (!a)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(a, u64_to_user_ptr(uptr), nr * sizeof(*a))) {
		kvfree(a);
		return ERR_PTR(-EFAULT);
	}
	return a;
}

/*
 * Scattered pages, in the caller's order (most urgent first).  Neighbouring
 * entries that are adjacent in memory share a run; a batch closes at SP_BATCH
 * pages and all its IO is in flight together.
 */
static long sp_do_vec(bool evict, struct sp_vec *v)
{
	struct mm_struct *mm;
	struct sp_batch *b;
	long ret = 0;
	u64 *addrs;
	u32 i = 0;

	addrs = sp_copy_addrs(v->addrs, v->nr);
	if (IS_ERR(addrs))
		return PTR_ERR(addrs);
	mm = sp_get_mm(v->pid);
	b = mm ? kvmalloc(sizeof(*b), GFP_KERNEL) : NULL;
	if (!b) {
		ret = mm ? -ENOMEM : -ESRCH;
		goto out;
	}

	v->done = v->dropped = 0;
	while (i < v->nr) {
		/* a prefetch that can no longer arrive in time only adds load */
		if (v->deadline_ns && ktime_get_ns() > v->deadline_ns) {
			v->dropped = v->nr - i;
			atomic_long_add(v->dropped, &st_dropped);
			break;
		}
		if (mmap_read_lock_killable(mm)) {
			ret = -EINTR;
			break;
		}
		sp_batch_start(b, mm, evict);
		while (i < v->nr && !sp_batch_full(b)) {
			unsigned long start = addrs[i++] & PAGE_MASK, end, max;
			struct vm_area_struct *vma = find_vma(mm, start);

			if (!vma || start < vma->vm_start || !sp_vma_ok(vma, evict))
				continue;
			end = start + PAGE_SIZE;
			max = min(vma->vm_end, start +
				  (SP_BATCH - b->nr_pages) * PAGE_SIZE);
			while (i < v->nr && end < max &&
			       (addrs[i] & PAGE_MASK) == end) {
				end += PAGE_SIZE;
				i++;
			}
			sp_batch_add(b, vma, start, end);
		}
		v->done += sp_batch_run(b);
		mmap_read_unlock(mm);
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}
		cond_resched();
	}
out:
	kvfree(b);
	if (mm)
		mmput(mm);
	kvfree(addrs);
	return ret;
}

/* ---- page state query ------------------------------------------------- */

/* Huge and non-present PMDs are answered here so the walker never splits. */
static int sp_q_pmd(pmd_t *pmd, unsigned long addr, unsigned long next,
		    struct mm_walk *walk)
{
	pmd_t v = pmdp_get_lockless(pmd);
	u8 *st = walk->private;

	if (!pmd_present(v)) {
		*st = pmd_none(v) ? SP_PG_NONE : SP_PG_BUSY;
		walk->action = ACTION_CONTINUE;
	} else if (pmd_leaf(v)) {
		*st = pmd_young(v) ? SP_PG_YOUNG : SP_PG_OLD;
		walk->action = ACTION_CONTINUE;
	}
	return 0;
}

static int sp_q_pte(pte_t *pte, unsigned long addr, unsigned long next,
		    struct mm_walk *walk)
{
	pte_t v = ptep_get(pte);
	u8 *st = walk->private;

	if (pte_none(v)) {
		*st = SP_PG_NONE;
	} else if (pte_present(v)) {
		*st = pte_young(v) ? SP_PG_YOUNG : SP_PG_OLD;
	} else {
		swp_entry_t e = pte_to_swp_entry(v);

		if (is_device_private_entry(e) &&
		    pfn_swap_entry_to_page(e)->pgmap->owner == &sp_owner)
			*st = SP_PG_SSD;
		else if (is_migration_entry(e))
			*st = SP_PG_BUSY;
		else
			*st = SP_PG_OTHER;
	}
	return 0;
}

static const struct mm_walk_ops sp_q_ops = {
	.pmd_entry = sp_q_pmd,
	.pte_entry = sp_q_pte,
	.walk_lock = PGWALK_RDLOCK,
};

static long sp_do_query(struct sp_query *q)
{
	struct mm_struct *mm;
	long ret = 0;
	u64 *addrs;
	u8 *st;
	u32 i;

	addrs = sp_copy_addrs(q->addrs, q->nr);
	if (IS_ERR(addrs))
		return PTR_ERR(addrs);
	mm = sp_get_mm(q->pid);
	st = mm ? kvzalloc(q->nr, GFP_KERNEL) : NULL;
	if (!st) {
		ret = mm ? -ENOMEM : -ESRCH;
		goto out;
	}
	for (i = 0; i < q->nr && !ret; ) {
		u32 stop = min(q->nr, i + SP_BATCH);

		if (mmap_read_lock_killable(mm)) {
			ret = -EINTR;
			break;
		}
		for (; i < stop; i++) {
			unsigned long a = addrs[i] & PAGE_MASK;

			/* unmapped address: stays SP_PG_NONE */
			walk_page_range(mm, a, a + PAGE_SIZE, &sp_q_ops, &st[i]);
		}
		mmap_read_unlock(mm);
		cond_resched();
	}
	if (!ret && copy_to_user(u64_to_user_ptr(q->states), st, q->nr))
		ret = -EFAULT;
out:
	kvfree(st);
	if (mm)
		mmput(mm);
	kvfree(addrs);
	return ret;
}

/* Hand the oldest pending events to user space and consume them. */
static long sp_do_trace(struct sp_trace *t)
{
	u32 want = min(t->nr, SP_TRACE_READ), n;
	struct sp_event *buf;
	long ret = 0;

	buf = kvmalloc_array(max(want, 1U), sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	spin_lock_irq(&sp_trace_lock);
	for (n = 0; n < want && sp_trace_tail != sp_trace_head; n++)
		buf[n] = sp_trace[sp_trace_tail++ & (SP_TRACE_SIZE - 1)];
	t->lost = sp_trace_lost;
	spin_unlock_irq(&sp_trace_lock);
	t->got = n;
	if (copy_to_user(u64_to_user_ptr(t->buf), buf, n * sizeof(*buf)))
		ret = -EFAULT;
	kvfree(buf);
	return ret;
}

static long sp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *up = (void __user *)arg;
	union {
		struct sp_range r;
		struct sp_vec v;
		struct sp_query q;
		struct sp_trace t;
	} u;
	long ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (cmd == SP_EVACUATE)
		return sp_evacuate();
	if (_IOC_TYPE(cmd) != 'S' || _IOC_SIZE(cmd) > sizeof(u))
		return -ENOTTY;
	if (copy_from_user(&u, up, _IOC_SIZE(cmd)))
		return -EFAULT;
	switch (cmd) {
	case SP_EVICT:
	case SP_PROMOTE:
		ret = sp_do_range(cmd == SP_EVICT, &u.r);
		break;
	case SP_EVICTV:
	case SP_PROMOTEV:
		ret = sp_do_vec(cmd == SP_EVICTV, &u.v);
		break;
	case SP_QUERY:
		return sp_do_query(&u.q);
	case SP_TRACE:
		ret = sp_do_trace(&u.t);
		break;
	default:
		return -ENOTTY;
	}
	if (copy_to_user(up, &u, _IOC_SIZE(cmd)))
		return -EFAULT;
	return ret;
}

static ssize_t sp_read(struct file *file, char __user *ubuf, size_t len,
		       loff_t *ppos)
{
	long nf = atomic_long_read(&st_fault);
	u64 d = nf ? nf : 1;
	char *buf;
	ssize_t n;
	int i;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	n = scnprintf(buf, PAGE_SIZE,
		"chunks %u/%u slots_used %ld\n"
		"evict %ld promote %ld fault %ld evacuate %ld\n"
		"wr_err %ld rd_err %ld crc_err %ld nospc %ld\n"
		"late %ld evict_wait %ld skip %ld dropped %ld trace_lost %llu wait_probe %d busy %ld\n"
		"fault_avg_ns setup %llu alloc %llu io %llu finalize %llu\n"
		"fault_hist_us",
		sp_nr_chunks, sp_max_chunks, atomic_long_read(&st_used),
		atomic_long_read(&st_evict), atomic_long_read(&st_promote), nf,
		atomic_long_read(&st_evac),
		atomic_long_read(&st_wr_err), atomic_long_read(&st_rd_err),
		atomic_long_read(&st_crc_err), atomic_long_read(&st_nospc),
		atomic_long_read(&st_late), atomic_long_read(&st_evict_wait),
		atomic_long_read(&st_skip), atomic_long_read(&st_dropped),
		READ_ONCE(sp_trace_lost), sp_wait_kp_on,
		atomic_long_read(&st_fault_busy),
		atomic64_read(&ft_setup) / d, atomic64_read(&ft_alloc) / d,
		atomic64_read(&ft_io) / d, atomic64_read(&ft_fin) / d);
	/* bucket i counts faults with total latency in [2^i, 2^(i+1)) us */
	for (i = 0; i < SP_HIST; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n, " %lld",
			       (long long)atomic64_read(&ft_hist[i]));
	n += scnprintf(buf + n, PAGE_SIZE - n, "\n");
	n = simple_read_from_buffer(ubuf, len, ppos, buf, n);
	kfree(buf);
	return n;
}

static const struct file_operations sp_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = sp_ioctl,
	.read = sp_read,
	.llseek = default_llseek,
};

static struct miscdevice sp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ssdpager",
	.fops = &sp_fops,
	.mode = 0600,
};

static int __init sp_init(void)
{
	int err;

	if (!dev)
		return -EINVAL;
	sp_bh = bdev_open_by_path(dev, BLK_OPEN_READ | BLK_OPEN_WRITE,
				  &sp_owner, NULL);
	if (IS_ERR(sp_bh))
		return PTR_ERR(sp_bh);
	sp_max_chunks = bdev_nr_bytes(sp_bh->bdev) >> ilog2(SP_CHUNK_SIZE);
	sp_chunks = kvcalloc(sp_max_chunks, sizeof(*sp_chunks), GFP_KERNEL);
	sp_trace = vmalloc(array_size(SP_TRACE_SIZE, sizeof(*sp_trace)));
	if (!sp_max_chunks || !sp_chunks || !sp_trace) {
		err = -ENOMEM;
		goto out;
	}
	/* without the probe, late/evict_wait stay 0 (see wait_probe in stats) */
	sp_wait_kp_on = !register_kprobe(&sp_wait_kp);
	err = misc_register(&sp_misc);
	if (err)
		goto out;
	pr_info("ssdpager: %s, %u chunks of %lu MiB\n", dev, sp_max_chunks,
		SP_CHUNK_SIZE >> 20);
	return 0;
out:
	if (sp_wait_kp_on)
		unregister_kprobe(&sp_wait_kp);
	vfree(sp_trace);
	kvfree(sp_chunks);
	bdev_release(sp_bh);
	return err;
}

/* Each slot in use pins the module, so every device page is free here. */
static void __exit sp_exit(void)
{
	unsigned int i;

	misc_deregister(&sp_misc);
	if (sp_wait_kp_on)
		unregister_kprobe(&sp_wait_kp);
	for (i = 0; i < sp_nr_chunks; i++) {
		struct sp_chunk *c = sp_chunks[i];

		memunmap_pages(&c->pgmap);
		release_mem_region(c->pgmap.range.start, SP_CHUNK_SIZE);
		bitmap_free(c->map);
		kfree(c);
	}
	vfree(sp_trace);
	kvfree(sp_chunks);
	bdev_release(sp_bh);
}

module_init(sp_init);
module_exit(sp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SSD paging of anonymous memory via device-private pages");
