/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec.c — migration executor for xtier-arm (DRAM -> SSD-swap).
 * Replaces xTier's kernel module + xtier_migrate_range + PTE A-bit scanner:
 *   demotion  : process_madvise(pidfd, MADV_PAGEOUT) — 5.10 whitelist, verified
 *   promotion : kernel demand paging, plus fault-neighborhood prefetch
 *               (batch process_vm_readv of the faulted page's 2MB region,
 *               run on a worker thread off the workload's critical path)
 *   cold scan : /proc/pid/pagemap + /sys/kernel/mm/page_idle/bitmap, BULK
 *               (pagemap in 512-entry preads, page_idle via word cache —
 *               exp9 found per-page syscalls were the budget-enforcement
 *               bottleneck: 65MB demoted in 63 epochs)
 *   tier truth: pagemap present/swap bits
 */
#include "xtierd.h"
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SYS_process_madvise
#define SYS_process_madvise 440
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef SYS_process_vm_readv
#define SYS_process_vm_readv 270
#endif
#define MAX_IOV 1024              // UIO_MAXIOV: process_madvise iovcnt cap

struct exec_stats XSTATS;

static uint32_t storm_threshold = 16000;   // XTIER_STORM_PSWPIN; ON above the ~12K demand-churn floor
static uint64_t storm_ewma_pswpin;        // EWMA of swap-in pages/s

static void read_vmstat_cum(uint64_t *pswpin, uint64_t *pswpout) {
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f) { *pswpin = *pswpout = 0; return; }
    char k[64]; unsigned long long v;
    while (fscanf(f, "%63s %llu", k, &v) == 2) {
        if (!strcmp(k, "pswpin")) *pswpin = v;
        else if (!strcmp(k, "pswpout")) *pswpout = v;
    }
    fclose(f);
}

static int pidfd = -1;
static int pagemap_fd = -1;
static int pageidle_fd = -1;

void exec_init(void) {
    if (G.storm_pswpin > 0) storm_threshold = G.storm_pswpin;
    char p[64];
    pidfd = (int)syscall(SYS_pidfd_open, (int)G.pid, 0);
    if (pidfd < 0) { perror("pidfd_open"); exit(1); }
    snprintf(p, sizeof(p), "/proc/%d/pagemap", (int)G.pid);
    pagemap_fd = open(p, O_RDONLY);
    pageidle_fd = open("/sys/kernel/mm/page_idle/bitmap", O_RDWR);  // must be RDWR
    printf("[*] exec: pidfd=%d pagemap=%d page_idle=%d\n", pidfd, pagemap_fd, pageidle_fd);
    if (G.mode == 3) pref_start();
}

// ---- RSS / budget ----
uint64_t exec_resident_pages(void) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/statm", (int)G.pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    unsigned long size, resident = 0;
    if (fscanf(f, "%lu %lu", &size, &resident) != 2) resident = 0;
    fclose(f);
    return resident;
}

// ---- demotion: sort, merge contiguous, MADV_PAGEOUT in <=1024-iovec calls ----
static int addr_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// returns pages actually handed to the kernel (attempted), or -1
static long demote_range_batch(uint64_t *addrs, uint32_t n) {
    qsort(addrs, n, sizeof(uint64_t), addr_cmp);
    struct iovec iovs[MAX_IOV];
    uint32_t n_iov = 0, i = 0;
    long attempted = 0;
    while (i < n && n_iov < MAX_IOV) {
        while (i + 1 < n && addrs[i + 1] == addrs[i]) { i++; n--; }   // dedup after sort
        uint64_t start = addrs[i], end = start + 4096, cnt = 1;
        while (i + cnt < n && addrs[i + cnt] == end) { end += 4096; cnt++; }
        iovs[n_iov++] = (struct iovec){ .iov_base = (void *)(uintptr_t)start, .iov_len = end - start };
        attempted += (long)cnt;
        i += cnt;
    }
    long ret = syscall(SYS_process_madvise, pidfd, iovs, n_iov, MADV_PAGEOUT, 0);
    XSTATS.demote_att += attempted;
    if (ret < 0) { XSTATS.demote_fail += attempted; return -1; }
    XSTATS.demote_ok += attempted;
    return attempted;
}

// demote a list of candidate pages; sets state->tier=2 + cooldown on success
void exec_demote(uint64_t *addrs, uint32_t n) {
    // stack/exec guard (dedup happens after sort in demote_range_batch)
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t pg = addrs[i];
        const struct meta_val *mv = pol_meta_get(pg);
        if (mv && (mv->page_type == 3 || mv->page_type == 1)) continue;
        addrs[m++] = pg;
    }
    if (m == 0) return;
    long done = demote_range_batch(addrs, m);
    if (done < 0) return;
    // mark state: swapped out, cooldown anchor = this epoch
    uint32_t ep = pol_cur_epoch();
    for (long i = 0; i < done; i++) {
        struct pstate *st = pol_state_get(addrs[i], 0);
        if (st) { st->tier = 2; st->last_mig_epoch = ep; }
    }
}

// =====================================================================
// BULK COLD SCAN (page_idle + pagemap)
//
// exp9 lesson: per-page idle queries (2-3 syscalls each) capped budget
// enforcement at ~65MB/63 epochs and found almost no idle pages in the
// headroom regime. Bulk version:
//   - pagemap: one pread per 512 entries (4 KB read per 2 MB of VA)
//   - page_idle: a 256-slot direct-mapped word cache; PFNs inside a VMA
//     are mostly contiguous, so one word covers 64 pages and one pwrite
//     re-marks a whole group. Per-page syscall cost drops ~50-100x.
// Semantics (matches xTier's two-round A-bit scanner): every scan marks
// present pages idle; a page whose bit is STILL 1 at the next scan (never
// accessed in between) is cold.
// =====================================================================
static uint64_t scan_resume_addr;
static int scan_first_pass = 1;

#define PM_BULK 512
static uint64_t pm_bulk[PM_BULK];

#define WC_SLOTS 256
static uint64_t wc_off[WC_SLOTS];
static uint64_t wc_word[WC_SLOTS];
static uint8_t  wc_dirty[WC_SLOTS];

static int idle_proc(uint64_t pfn, int mark, int *out) {
    // bitmap layout: word = pfn/64 (8 bytes), bit = pfn%64
    uint64_t off = (pfn / 64) * 8;
    uint32_t i = (uint32_t)((pfn / 64) & (WC_SLOTS - 1));
    if (wc_off[i] != off) {
        if (wc_dirty[i]) {
            if (pwrite(pageidle_fd, &wc_word[i], 8, (off_t)wc_off[i]) != 8) return -1;
            wc_dirty[i] = 0;
        }
        if (pread(pageidle_fd, &wc_word[i], 8, (off_t)off) != 8) return -1;
        wc_off[i] = off;
    }
    uint64_t bit = 1ULL << (pfn % 64);
    *out = (int)((wc_word[i] & bit) != 0);
    if (mark && !(wc_word[i] & bit)) { wc_word[i] |= bit; wc_dirty[i] = 1; }
    return 0;
}

static void wc_flush_all(void) {
    for (uint32_t i = 0; i < WC_SLOTS; i++) {
        if (wc_dirty[i] && wc_off[i]) {
            if (pwrite(pageidle_fd, &wc_word[i], 8, (off_t)wc_off[i]) != 8) { /* best effort */ }
            wc_dirty[i] = 0;
        }
    }
}

// scan one chunk; appends cold candidates to out[]. returns pages scanned.
static uint32_t cold_scan_chunk(uint64_t *out, uint32_t max_out, uint32_t *n_out) {
    if (pageidle_fd < 0 || pagemap_fd < 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)G.pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char *line = NULL; size_t cap = 0;
    uint32_t scanned = 0;
    uint64_t addr = scan_resume_addr;

    while (getline(&line, &cap, fp) > 0) {
        unsigned long s, e; char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %4s", &s, &e, perms) != 3) continue;
        char *pathname = strchr(line, '/');
        if (pathname && strncmp(pathname, "/dev/", 5) == 0) continue;
        if (addr >= e) continue;
        if (addr < s) addr = s;
        if (strchr(perms, 'w') == NULL) { if (addr >= s && addr < e) addr = e; continue; }  // writable anon-ish only
        while (addr < e && scanned < G.cold_scan_chunk && *n_out < max_out) {
            uint64_t span = (e - addr) / 4096;
            uint64_t want = G.cold_scan_chunk - scanned;
            size_t n = span < want ? (size_t)span : (size_t)want;
            if (n > PM_BULK) n = PM_BULK;
            ssize_t r = pread(pagemap_fd, pm_bulk, n * 8, (off_t)(addr / 4096 * 8));
            if (r <= 0) { addr += n * 4096; continue; }
            size_t got = (size_t)r / 8;
            for (size_t k = 0; k < got && scanned < G.cold_scan_chunk && *n_out < max_out; k++) {
                uint64_t ent = pm_bulk[k];
                if ((ent >> 63) & 1) {
                    uint64_t pfn = ent & 0x007FFFFFFFFFFFFFULL;
                    int bit = 0;
                    if (idle_proc(pfn, 1, &bit) == 0) {
                        if (!scan_first_pass && bit) out[(*n_out)++] = addr + k * 4096;  // idle through the whole interval -> cold
                    }
                    scanned++;
                }
            }
            addr += got * 4096;
        }
        if (scanned >= G.cold_scan_chunk || *n_out >= max_out) break;
    }
    free(line); fclose(fp);
    wc_flush_all();
    XSTATS.cold_scanned += scanned;
    XSTATS.cold_found += *n_out;
    if (addr == 0 || addr >= (1UL << 47)) {
        scan_resume_addr = 0;
        scan_first_pass = 0;
        XSTATS.cold_passes++;
    } else {
        scan_resume_addr = addr;
    }
    return scanned;
}

// =====================================================================
// FAULT-NEIGHBORHOOD PREFETCH (predictive push-back, the MEXT analog)
//
// A sample on a tier-2 page means the workload just paid a major fault for
// it. Its 2MB region neighbors are statistically next (data locality), so
// the worker thread batch-swaps-in the region's swapped pages BEFORE the
// workload touches them: demand faults become daemon-side batched reads,
// off the workload's critical path. Faulted-in pages are pinned through the
// results ring so demotion does not immediately evict them.
//
// Producer (main thread: pol_sample on tier 2->1) -> req ring; consumer is
// the worker thread; results flow back through the done ring and are
// applied by the main thread at the epoch boundary (pstate table is
// main-thread-only). Storm feedback pauses the worker: prefetching during
// a swap-in storm just adds queue depth.
// =====================================================================
#define PFR_RING   8192
#define PFR_DONE   8192
#define REG_SHIFT_ 21
#define REG_MASK_  (~(4096ULL * 512 - 1))
#define PFR_PAGES_PER_SEC 4096     // XTIER_PREF_BPS: ~16MB/s swap-in budget

static uint64_t pfr_req[PFR_RING];
static _Atomic uint64_t pfr_req_h;               // producer cursor (main)
static _Atomic uint64_t pfr_req_t;               // consumer cursor (worker)
static uint64_t pfr_done[PFR_DONE];
static _Atomic uint64_t pfr_done_h;              // producer cursor (worker)
static _Atomic uint64_t pfr_done_t;              // consumer cursor (main)

static uint64_t reg_seen[1024];                   // dedup: last region base per slot
static uint32_t reg_epoch[1024];

static volatile int pref_run;
static uint32_t pref_bps = PFR_PAGES_PER_SEC;
static uint32_t pref_pause = 24000;       // XTIER_PREF_PAUSE: pswpin/s spiral guard

void pref_note_fault(uint64_t page) {
    uint64_t region = page & REG_MASK_;
    uint32_t slot = (uint32_t)((region >> 21) & 1023);
    uint32_t ep = pol_cur_epoch();
    if (reg_seen[slot] == region && reg_epoch[slot] == ep) return;  // already queued this epoch
    reg_seen[slot] = region; reg_epoch[slot] = ep;
    uint64_t h = atomic_load(&pfr_req_h);
    if (h - atomic_load(&pfr_req_t) >= PFR_RING) return;             // full: drop
    pfr_req[h & (PFR_RING - 1)] = region;
    atomic_store(&pfr_req_h, h + 1);
}

static void *pref_worker(void *arg) {
    (void)arg;
    static uint64_t pm[512];
    static char rdbuf[512 * 4096];                 // 2 MB local sink
    struct iovec liov[512], riov[512];
    for (int i = 0; i < 512; i++) liov[i] = (struct iovec){ .iov_base = rdbuf + (size_t)i * 4096, .iov_len = 4096 };
    uint64_t pages_this_sec = 0;
    struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
    while (pref_run) {
        uint64_t h = atomic_load(&pfr_req_h), t = atomic_load(&pfr_req_t);
        if (h == t) { usleep(5000); continue; }
        // Pause under swap-in pressure, but ABOVE the demand-churn floor of a
        // tight-budget regime (~16-17K pswpin/s measured in exp12): pausing at
        // 8000 kept prefetch dead exactly where it is most needed. The
        // per-second page budget is the primary throttle; this is only a
        // spiral guard (26K+, exp6b).
        if (storm_ewma_pswpin > pref_pause) { usleep(20000); continue; }
        // per-second page budget
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (now.tv_sec - ts0.tv_sec) + (now.tv_nsec - ts0.tv_nsec) / 1e9;
        if (dt >= 1.0) { pages_this_sec = 0; ts0 = now; }
        if (pages_this_sec >= pref_bps) { usleep(50000); continue; }

        uint64_t region = pfr_req[t & (PFR_RING - 1)];
        atomic_store(&pfr_req_t, t + 1);

        ssize_t r = pread(pagemap_fd, pm, sizeof(pm), (off_t)(region / 4096 * 8));
        if (r <= 0) continue;
        size_t got = (size_t)r / 8;
        uint32_t n_sw = 0;
        for (size_t k = 0; k < got; k++) {
            // swapped: present=0 and swap entry bit 62 set
            if (!((pm[k] >> 63) & 1) && ((pm[k] >> 62) & 1)) {
                riov[n_sw++] = (struct iovec){ .iov_base = (void *)(uintptr_t)(region + k * 4096), .iov_len = 4096 };
            }
        }
        if (n_sw == 0) continue;
        // batched swap-in; keep local/remote lengths matched by trimming
        uint32_t n = n_sw < 512 ? n_sw : 512;
        ssize_t gotn = syscall(SYS_process_vm_readv, (int)G.pid, liov, n, riov, n, 0);
        if (gotn <= 0) continue;
        uint32_t ok_pages = (uint32_t)(gotn / 4096);       // contiguous from iov[0]... process_vm_readv returns total bytes; on partial error we take the pessimistic view
        // record the pages that are now resident
        for (uint32_t k = 0; k < ok_pages; k++) {
            uint64_t dh = atomic_load(&pfr_done_h);
            if (dh - atomic_load(&pfr_done_t) < PFR_DONE) {
                pfr_done[dh & (PFR_DONE - 1)] = (uint64_t)(uintptr_t)riov[k].iov_base;
                atomic_store(&pfr_done_h, dh + 1);
            }
        }
        pages_this_sec += ok_pages;
        XSTATS.pref_pages += ok_pages;
        XSTATS.pref_regions++;
    }
    return NULL;
}

void pref_start(void) {
    const char *e = getenv("XTIER_PREF_BPS");
    if (e && *e) { uint32_t v = (uint32_t)strtoul(e, NULL, 10); if (v) pref_bps = v; }
    const char *pe = getenv("XTIER_PREF_PAUSE");
    if (pe && *pe) { uint32_t v = (uint32_t)strtoul(pe, NULL, 10); if (v) pref_pause = v; }
    pref_run = 1;
    pthread_t tid;
    if (pthread_create(&tid, NULL, pref_worker, NULL) != 0) perror("prefetch thread");
    printf("[*] prefetch: worker started, budget=%u pages/s\n", pref_bps);
}

// main thread: apply prefetched pages to the state table (pin for cooldown epochs)
static void pref_drain(void) {
    uint64_t h = atomic_load(&pfr_done_h), t = atomic_load(&pfr_done_t);
    uint32_t ep = pol_cur_epoch();
    while (t != h) {
        uint64_t page = pfr_done[t & (PFR_DONE - 1)];
        struct pstate *st = pol_state_get(page, 0);
        if (st) { st->tier = 1; st->last_mig_epoch = ep; }
        t++;
    }
    atomic_store(&pfr_done_t, t);
}

// ---- 1Hz: majflt / swap traffic ----
static uint64_t last_majflt, last_pswpin, last_pswpout;
static void read_vmstat(uint64_t *pswpin, uint64_t *pswpout) { read_vmstat_cum(pswpin, pswpout); }
static uint64_t read_majflt(void) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/stat", (int)G.pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    char *c = buf; int field = 0;
    while (field < 11 && c) { c = strchr(c + 1, ' '); field++; }
    return c ? strtoull(c + 1, NULL, 10) : 0;
}

void exec_second(FILE *stats_csv) {
    XSTATS.majflt_cum = read_majflt();
    uint64_t pi = 0, po = 0;
    read_vmstat(&pi, &po);
    XSTATS.pswpin_cum = pi;
    XSTATS.pswpout_cum = po;
    uint64_t dmaj = XSTATS.majflt_cum - last_majflt;
    uint64_t dso = po - last_pswpout;
    uint64_t dsi = pi - last_pswpin;
    last_majflt = XSTATS.majflt_cum; last_pswpin = pi; last_pswpout = po;
    if (stats_csv) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(stats_csv, "%ld.3,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                ts.tv_sec, (unsigned long long)dmaj, (unsigned long long)dsi, (unsigned long long)dso,
                (unsigned long long)exec_resident_pages(), (unsigned long long)XSTATS.demote_ok,
                (unsigned long long)XSTATS.demote_fail, (unsigned long long)XSTATS.cold_found,
                (unsigned long long)XSTATS.majflt_cum, (unsigned long long)XSTATS.cold_scanned,
                (unsigned long long)XSTATS.pref_pages, (unsigned long long)XSTATS.pref_regions);
        fflush(stats_csv);
    }
}

// ---- per-epoch: budget enforcement ----
void exec_epoch(uint32_t ep_fin) {
    if (G.mode != 3) return;
    pref_drain();
    if (G.budget_pages == 0) return;

    // Fault-storm stand-down with hysteresis (16K on / 13.6K off): the gate
    // catches the 26K collapsed-sampling spiral (exp6b), not the ~10-12K
    // irreducible demand churn (exp5-10 all configs).
    uint64_t pi_cum = 0, po_cum = 0;
    read_vmstat_cum(&pi_cum, &po_cum);
    int storm_on;
    {
        static uint64_t last_pi;
        static struct timespec last_t;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (now.tv_sec - last_t.tv_sec) + (now.tv_nsec - last_t.tv_nsec) / 1e9;
        if (dt < 0.05) dt = 0.05;
        double cur = (double)(pi_cum - last_pi) / dt;   // pages/s
        last_pi = pi_cum; last_t = now;
        storm_ewma_pswpin = (uint64_t)(storm_ewma_pswpin * 0.7 + cur * 0.3);
        static int storm_state = 0;
        if (!storm_state && storm_ewma_pswpin > storm_threshold) {
            storm_state = 1;
            printf("[STORM] ON  (pswpin ewma=%llu > %u) -- ML channel standing down\n",
                   (unsigned long long)storm_ewma_pswpin, storm_threshold);
        } else if (storm_state && storm_ewma_pswpin < storm_threshold * 85 / 100) {
            storm_state = 0;
            printf("[STORM] OFF (pswpin ewma=%llu) -- ML channel rearming\n",
                   (unsigned long long)storm_ewma_pswpin);
        }
        if (storm_state) XSTATS.storm_epochs++;
        storm_on = storm_state;
    }

    uint64_t resident = exec_resident_pages();
    if (resident <= G.budget_pages) return;

    // Cold-scan demotion ALWAYS runs (hardware idle-bit evidence; now bulk).
    static uint64_t cold_out[4096];
    uint32_t n_cold = 0;
    cold_scan_chunk(cold_out, 4096, &n_cold);

    uint64_t budget = G.mig_budget;
    if (n_cold > 0 && budget > 0) {
        uint64_t n = n_cold < budget ? n_cold : budget;
        for (uint64_t off = 0; off < n; off += 1024) {
            uint32_t m = (uint32_t)((n - off) < 1024 ? (n - off) : 1024);
            exec_demote(cold_out + off, m);
            budget -= m;
        }
    }
    if (storm_on) return;   // fault storm: only hardware-evidence demotion continues
    if (budget > 0) {
        // take the lowest-scored ML candidates, in 1024-page chunks
        struct dem_cand *pool = pol_dem_pool();
        uint32_t pn = pol_dem_n();
        if (pool && pn > 0) {
            static uint64_t addrs[1024];
            uint32_t i = 0;
            while (i < pn && budget > 0) {
                uint32_t n = 0;
                while (i < pn && n < 1024 && budget > 0) {
                    addrs[n++] = pool[i++].page;
                    budget--;
                }
                exec_demote(addrs, n);
            }
        }
    }
    (void)ep_fin;
}
