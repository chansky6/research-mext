// SPDX-License-Identifier: GPL-2.0
/* sptest: control tool and semantic tests for ssdpager. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ssdpager.h"

#define PG 4096UL
#define MB (1UL << 20)
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __func__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); exit(1); } } while (0)

static int spfd = -1;

static void sp_open(void)
{
	spfd = open("/dev/ssdpager", O_RDWR);
	CHECK(spfd >= 0, "open /dev/ssdpager: %s", strerror(errno));
}

static long sp_range(unsigned long cmd, pid_t pid, void *addr, size_t len)
{
	struct sp_range r = { .pid = pid, .addr = (uintptr_t)addr, .len = len };

	CHECK(ioctl(spfd, cmd, &r) == 0, "ioctl: %s", strerror(errno));
	return r.done;
}
#define evict(a, l)   sp_range(SP_EVICT, getpid(), a, l)
#define promote(a, l) sp_range(SP_PROMOTE, getpid(), a, l)

/* scattered pages; returns pages moved, *dropped = entries past the deadline */
static long sp_vec(unsigned long cmd, const uint64_t *addrs, uint32_t nr,
		   uint64_t deadline_ns, long *dropped)
{
	struct sp_vec v = { .pid = getpid(), .nr = nr, .addrs = (uintptr_t)addrs,
			    .deadline_ns = deadline_ns };

	CHECK(ioctl(spfd, cmd, &v) == 0, "vec ioctl: %s", strerror(errno));
	if (dropped)
		*dropped = v.dropped;
	return v.done;
}

static void sp_query(const uint64_t *addrs, uint32_t nr, uint8_t *st)
{
	struct sp_query q = { .pid = getpid(), .nr = nr, .addrs = (uintptr_t)addrs,
			      .states = (uintptr_t)st };

	CHECK(ioctl(spfd, SP_QUERY, &q) == 0, "query ioctl: %s", strerror(errno));
}

/* count the pages of addrs[] in each SP_PG_* state */
static void sp_census(const uint64_t *addrs, uint32_t nr, long cnt[6])
{
	uint8_t *st = malloc(nr);

	sp_query(addrs, nr, st);
	memset(cnt, 0, 6 * sizeof(cnt[0]));
	for (uint32_t i = 0; i < nr; i++)
		cnt[st[i]]++;
	free(st);
}

/* drain the event ring; returns the number of events stored in ev[] */
static uint32_t sp_drain(struct sp_event *ev, uint32_t max)
{
	uint32_t n = 0;

	while (n < max) {
		struct sp_trace t = { .buf = (uintptr_t)(ev + n), .nr = max - n };

		CHECK(ioctl(spfd, SP_TRACE, &t) == 0, "trace ioctl: %s", strerror(errno));
		if (!t.got)
			break;
		n += t.got;
	}
	return n;
}

static long stat_field(const char *key)
{
	char buf[4096], *p;
	int n = pread(spfd, buf, sizeof(buf) - 1, 0);

	CHECK(n > 0, "read stat");
	buf[n] = 0;
	p = strstr(buf, key);
	CHECK(p, "no stat %s", key);
	return atol(p + strlen(key));
}

static void show_stat(void)
{
	char buf[4096];
	int n = pread(spfd, buf, sizeof(buf) - 1, 0);

	if (n > 0)
		fwrite(buf, 1, n, stdout);
}

static long meminfo(const char *key)
{
	char line[256];
	long v = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key)))
			v = atol(line + strlen(key) + 1);
	fclose(f);
	return v;
}

/* pagemap: count present and non-present-but-populated ("swap") pages */
static void pagemap(void *addr, size_t len, long *present, long *swapped)
{
	int fd = open("/proc/self/pagemap", O_RDONLY);
	size_t n = len / PG, i, got;
	uint64_t *e = malloc(n * 8);

	got = pread(fd, e, n * 8, (uintptr_t)addr / PG * 8);
	CHECK(got == n * 8, "pagemap read");
	*present = *swapped = 0;
	for (i = 0; i < n; i++) {
		*present += e[i] >> 63;
		*swapped += (e[i] >> 62) & 1;
	}
	free(e);
	close(fd);
}

static inline uint64_t pat(uint64_t seed, uint64_t pg, uint64_t w)
{
	return (seed * 0x10001 + pg * 512 + w + 1) * 0x9E3779B97F4A7C15ULL;
}

static void fill_pg(void *buf, size_t pg, uint64_t seed)
{
	uint64_t *p = (uint64_t *)((char *)buf + pg * PG);

	for (int w = 0; w < 512; w++)
		p[w] = pat(seed, pg, w);
}

static long verify_pg(void *buf, size_t pg, uint64_t seed)
{
	uint64_t *p = (uint64_t *)((char *)buf + pg * PG);
	long bad = 0;

	for (int w = 0; w < 512; w++)
		bad += p[w] != pat(seed, pg, w);
	return bad;
}

static void fill(void *buf, size_t len, uint64_t seed)
{
	for (size_t i = 0; i < len / PG; i++)
		fill_pg(buf, i, seed);
}

/* verify pages [first, first+n) of buf against the pattern of page index */
static long verify_at(void *buf, size_t first, size_t n, uint64_t seed)
{
	long bad = 0;

	for (size_t i = first; i < first + n; i++)
		bad += verify_pg(buf, i, seed);
	return bad;
}

static long verify(void *buf, size_t len, uint64_t seed)
{
	return verify_at(buf, 0, len / PG, seed);
}

/*
 * Of the pages in addrs[] (ascending) that are on the SSD, the share whose
 * predecessor in the list sits in the preceding slot.  For a device-private
 * page, pagemap reports its pfn as the swap offset.
 */
static double slot_contig_at(const uint64_t *addrs, size_t n)
{
	int fd = open("/proc/self/pagemap", O_RDONLY);
	size_t i, pairs = 0, adj = 0;
	uint64_t e, prev = 0;

	for (i = 0; i < n; i++) {
		CHECK(pread(fd, &e, 8, addrs[i] / PG * 8) == 8, "pagemap read");
		if (!((e >> 62) & 1))
			continue;
		e = (e & ((1ULL << 55) - 1)) >> 5;
		if (prev) {
			pairs++;
			adj += e == prev + 1;
		}
		prev = e;
	}
	close(fd);
	return pairs ? 100.0 * adj / pairs : 0;
}

static double slot_contig(void *addr, size_t len)
{
	size_t n = len / PG, i;
	uint64_t *a = malloc(n * sizeof(*a));
	double r;

	for (i = 0; i < n; i++)
		a[i] = (uintptr_t)addr + i * PG;
	r = slot_contig_at(a, n);
	free(a);
	return r;
}

static void shuffle(uint64_t *a, size_t n, unsigned seed)
{
	srand(seed);
	for (size_t i = n; i > 1; i--) {
		size_t j = (size_t)rand() % i;
		uint64_t t = a[i - 1];

		a[i - 1] = a[j];
		a[j] = t;
	}
}

static void *anon(size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	CHECK(p != MAP_FAILED, "mmap");
	madvise(p, len, MADV_NOHUGEPAGE);
	return p;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* ---- V1 / V4: round trip, accounting, fault latency ------------------- */
static void t_basic(size_t mb)
{
	size_t len = mb * MB, n = len / PG, half = len / 2, i;
	char *buf = anon(len);
	long pr, sw, done, free0, free1, f0;
	uint64_t *lat = malloc(n / 2 * 8), t0, sum = 0;
	volatile uint64_t sink = 0;

	fill(buf, len, 1);
	/* free + page cache: other processes' file IO moves pages between the two */
	free0 = meminfo("MemFree:") + meminfo("Cached:");
	t0 = now_ns();
	done = evict(buf, len);
	t0 = now_ns() - t0;
	free1 = meminfo("MemFree:") + meminfo("Cached:");
	pagemap(buf, len, &pr, &sw);
	printf("evict: %ld/%zu pages in %.2fs (%.0f MB/s); pagemap present=%ld nonpresent=%ld; MemFree+Cached +%ld MiB; SwapTotal=%ld kB\n",
	       done, n, t0 / 1e9, done * PG / 1e6 / (t0 / 1e9), pr, sw,
	       (free1 - free0) / 1024, meminfo("SwapTotal:"));
	CHECK(done == n && pr == 0 && sw == n, "not fully evicted");
	CHECK(meminfo("SwapTotal:") == 0, "swap is configured");

	/* first half: demand faults, one timed touch per page */
	f0 = stat_field("fault ");
	for (i = 0; i < n / 2; i++) {
		t0 = now_ns();
		/* SP_RANDOM: odd-stride permutation, exact when n/2 is a power of two */
		sink += *(uint64_t *)(buf + (getenv("SP_RANDOM") ?
					     i * 40503 % (n / 2) : i) * PG);
		lat[i] = now_ns() - t0;
		sum += lat[i];
	}
	qsort(lat, n / 2, 8, cmp_u64);
	printf("demand fault (user-visible, us): mean %.1f p50 %.1f p99 %.1f max %.1f; kernel faults %ld\n",
	       sum / 1e3 / (n / 2), lat[n / 4] / 1e3, lat[n / 2 * 99 / 100] / 1e3,
	       lat[n / 2 - 1] / 1e3, stat_field("fault ") - f0);
	CHECK(verify(buf, half, 1) == 0, "data mismatch after demand fault");

	/* second half: proactive promote, must not fault afterwards */
	t0 = now_ns();
	done = promote(buf + half, half);
	t0 = now_ns() - t0;
	pagemap(buf + half, half, &pr, &sw);
	f0 = stat_field("fault ");
	CHECK(verify_at(buf, n / 2, n / 2, 1) == 0, "data mismatch after promote");
	printf("promote: %ld pages in %.2fs (%.0f MB/s); present=%ld; faults during verify=%ld\n",
	       done, t0 / 1e9, done * PG / 1e6 / (t0 / 1e9), pr,
	       stat_field("fault ") - f0);
	CHECK(done == n / 2 && pr == n / 2 && stat_field("fault ") == f0, "promote incomplete");
	CHECK(stat_field("slots_used ") == 0, "slots leaked");
	show_stat();
}

/* ---- V2: fork / COW ---------------------------------------------------- */
static void t_fork(void)
{
	size_t len = 64 * MB;
	char *buf = anon(len);
	int st;
	pid_t pid;

	/* evict, then fork: device-private entries are copied */
	fill(buf, len, 2);
	CHECK(evict(buf, len) == len / PG, "evict");
	pid = fork();
	if (!pid) {
		if (verify(buf, len, 2))
			_exit(1);
		fill(buf, len, 3);		/* COW in child */
		_exit(verify(buf, len, 3) ? 2 : 0);
	}
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "child A status %x", st);
	CHECK(verify(buf, len, 2) == 0, "parent data changed by child");

	/* fork, then evict pages that are mapped by two processes */
	pid = fork();
	if (!pid) {
		long done = evict(buf, len);

		if (done != len / PG) {
			printf("shared evict %ld\n", done);
			_exit(3);
		}
		if (verify(buf, len, 2))
			_exit(1);
		_exit(0);
	}
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "child B status %x", st);
	fill(buf, len / 2, 4);
	CHECK(verify(buf, len / 2, 4) == 0 &&
	      verify_at(buf, len / 2 / PG, len / 2 / PG, 2) == 0, "parent verify");
	munmap(buf, len);
	CHECK(stat_field("slots_used ") == 0, "slots leaked");
}

/* ---- V2: concurrent faults -------------------------------------------- */
struct targ { char *buf; size_t first, n; long bad; };

static void *th_verify(void *p)
{
	struct targ *a = p;

	for (size_t i = a->first; i < a->first + a->n; i++)
		a->bad += verify_pg(a->buf, i, 5);
	return NULL;
}

static void t_threads(int nt)
{
	size_t len = 256 * MB, n = len / PG;
	char *buf = anon(len);
	pthread_t th[nt];
	struct targ a[nt];
	long bad = 0;

	fill(buf, len, 5);
	for (int round = 0; round < 2; round++) {
		CHECK(evict(buf, len) == n, "evict");
		for (int i = 0; i < nt; i++) {
			/* round 0: everyone faults the same pages; round 1: stripes */
			a[i] = round ? (struct targ){ buf, n / nt * i, n / nt, 0 }
				     : (struct targ){ buf, 0, n, 0 };
			pthread_create(&th[i], NULL, th_verify, &a[i]);
		}
		for (int i = 0; i < nt; i++) {
			pthread_join(th[i], NULL);
			bad += a[i].bad;
		}
	}
	CHECK(bad == 0, "%ld mismatches", bad);
	munmap(buf, len);
}

/* ---- V2: VMA operations on evicted ranges ------------------------------ */
static void t_vmops(void)
{
	size_t len = 64 * MB, q = len / 4;
	char *buf = anon(len), *moved;
	long used;

	fill(buf, len, 6);
	CHECK(evict(buf, len) == len / PG, "evict");
	used = stat_field("slots_used ");

	CHECK(munmap(buf, q) == 0, "munmap");		/* quarter 0 gone */
	CHECK(stat_field("slots_used ") == used - q / PG, "munmap did not free slots");

	CHECK(madvise(buf + q, q, MADV_DONTNEED) == 0, "dontneed");	/* quarter 1 zeroed */
	CHECK(stat_field("slots_used ") == used - 2 * q / PG, "DONTNEED did not free slots");
	for (size_t i = 0; i < q; i += PG)
		CHECK(*(uint64_t *)(buf + q + i) == 0, "DONTNEED page not zero");

	CHECK(mprotect(buf + 2 * q, q, PROT_READ) == 0, "mprotect");	/* quarter 2 RO */
	CHECK(verify_at(buf, 2 * q / PG, q / PG, 6) == 0, "RO verify");
	CHECK(mprotect(buf + 2 * q, q, PROT_READ | PROT_WRITE) == 0, "mprotect rw");

	moved = mremap(buf + 3 * q, q, q, MREMAP_MAYMOVE | MREMAP_FIXED,
		       (char *)anon(q));		/* quarter 3 moved while on SSD */
	CHECK(moved != MAP_FAILED, "mremap");
	for (size_t i = 0; i < q / PG; i++) {
		uint64_t *p = (uint64_t *)(moved + i * PG);

		for (int w = 0; w < 512; w++)
			CHECK(p[w] == pat(6, 3 * q / PG + i, w), "mremap verify");
	}
	CHECK(stat_field("slots_used ") == 0, "slots leaked");
}

/* ---- V2: GUP paths ------------------------------------------------------ */
static void t_gup(void)
{
	size_t len = 16 * MB;
	char *buf = anon(len), *copy = anon(len);
	struct iovec l = { copy, len }, r = { buf, len };
	char path[] = "/home/sptest.XXXXXX";
	int fd, mfd;

	fill(buf, len, 7);
	CHECK(evict(buf, len) == len / PG, "evict");
	CHECK(process_vm_readv(getpid(), &l, 1, &r, 1, 0) == len, "process_vm_readv");
	CHECK(verify(copy, len, 7) == 0, "process_vm_readv data");

	CHECK(evict(buf, len) == len / PG, "evict 2");
	mfd = open("/proc/self/mem", O_RDONLY);
	memset(copy, 0, len);
	CHECK(pread(mfd, copy, len, (uintptr_t)buf) == len, "/proc/self/mem");
	CHECK(verify(copy, len, 7) == 0, "/proc/self/mem data");
	close(mfd);

	/* O_DIRECT read pins the (evicted) destination pages for write */
	fill(copy, len, 8);
	fd = mkstemp(path);
	CHECK(write(fd, copy, len) == len, "write tmp");
	close(fd);
	fd = open(path, O_RDONLY | O_DIRECT);
	CHECK(fd >= 0, "open O_DIRECT");
	CHECK(evict(buf, len) == len / PG, "evict 3");
	CHECK(pread(fd, buf, len, 0) == len, "O_DIRECT read");
	CHECK(verify(buf, len, 8) == 0, "O_DIRECT data");
	close(fd);
	unlink(path);
	munmap(buf, len);
	CHECK(stat_field("slots_used ") == 0, "slots leaked");
}

/* ---- V2: THP and mlock --------------------------------------------------- */
static void t_thp(int wait_s)
{
	size_t len = 64 * MB;
	char *raw = mmap(NULL, len + 2 * MB, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	char *buf = (char *)(((uintptr_t)raw + 2 * MB - 1) & ~(2 * MB - 1));
	long done, pr, sw, sw2;

	madvise(buf, len, MADV_HUGEPAGE);
	fill(buf, len, 9);
	printf("AnonHugePages before evict: %ld kB\n", meminfo("AnonHugePages:"));
	done = evict(buf, len);
	pagemap(buf, len, &pr, &sw);
	printf("thp evict: %ld/%zu pages, present=%ld nonpresent=%ld\n",
	       done, len / PG, pr, sw);
	if (wait_s) {			/* does khugepaged pull pages back? */
		sleep(wait_s);
		pagemap(buf, len, &pr, &sw2);
		printf("after %ds idle: nonpresent %ld -> %ld (khugepaged pulled back %ld)\n",
		       wait_s, sw, sw2, sw - sw2);
	}
	CHECK(verify(buf, len, 9) == 0, "thp data");
	CHECK(done == len / PG, "thp not fully evicted");
}

static void t_mlock(void)
{
	size_t len = 16 * MB;
	char *buf = anon(len);
	long pr, sw;

	fill(buf, len, 10);
	CHECK(mlock(buf, len) == 0, "mlock");
	CHECK(evict(buf, len) == 0, "evicted mlocked pages");
	munlock(buf, len);
	CHECK(evict(buf, len) == len / PG, "evict");
	CHECK(mlock(buf, len) == 0, "mlock 2");		/* must fault everything in */
	pagemap(buf, len, &pr, &sw);
	CHECK(pr == len / PG, "mlock left %ld pages on SSD", sw);
	CHECK(verify(buf, len, 10) == 0, "mlock data");
}

/* ---- V2: evict / promote / access racing -------------------------------- */
static char *s_buf;
static size_t s_n;
static volatile int s_stop;
static uint32_t *s_ver;
#define S_WORKERS 4

static void *s_worker(void *p)
{
	long id = (long)p, bad = 0, ops = 0;
	size_t stripe = s_n / S_WORKERS, base = stripe * id;
	unsigned int seed = id + 1;

	while (!s_stop) {
		size_t pg = base + rand_r(&seed) % stripe;

		bad += verify_pg(s_buf, pg, s_ver[pg]);
		if (rand_r(&seed) & 1)
			fill_pg(s_buf, pg, ++s_ver[pg]);
		ops++;
	}
	printf("worker %ld: %ld ops, %ld bad words\n", id, ops, bad);
	return (void *)bad;
}

static void *s_mover(void *p)
{
	unsigned long cmd = (unsigned long)p;
	unsigned int seed = cmd;
	long moved = 0;

	while (!s_stop) {
		size_t pg = rand_r(&seed) % s_n, cnt = 1 + rand_r(&seed) % 2048;

		if (pg + cnt > s_n)
			cnt = s_n - pg;
		moved += sp_range(cmd, getpid(), s_buf + pg * PG, cnt * PG);
	}
	printf("%s: %ld pages\n", cmd == SP_EVICT ? "evictor" : "promoter", moved);
	return NULL;
}

static void t_stress(int secs)
{
	pthread_t w[S_WORKERS], ev[2], prm;
	long bad = 0;
	void *r;

	s_n = 512 * MB / PG;
	s_buf = anon(s_n * PG);
	s_ver = calloc(s_n, sizeof(*s_ver));
	fill(s_buf, s_n * PG, 0);
	for (long i = 0; i < S_WORKERS; i++)
		pthread_create(&w[i], NULL, s_worker, (void *)i);
	pthread_create(&ev[0], NULL, s_mover, (void *)SP_EVICT);
	pthread_create(&ev[1], NULL, s_mover, (void *)SP_EVICT);
	pthread_create(&prm, NULL, s_mover, (void *)SP_PROMOTE);
	sleep(secs);
	s_stop = 1;
	for (int i = 0; i < S_WORKERS; i++) {
		pthread_join(w[i], &r);
		bad += (long)r;
	}
	pthread_join(ev[0], NULL);
	pthread_join(ev[1], NULL);
	pthread_join(prm, NULL);
	for (size_t i = 0; i < s_n; i++)
		bad += verify_pg(s_buf, i, s_ver[i]);
	CHECK(bad == 0, "%ld bad words", bad);
	munmap(s_buf, s_n * PG);
	CHECK(stat_field("slots_used ") == 0, "slots leaked");
	show_stat();
}

/* ---- V3: read failure is confined to the faulting process --------------- */
static void t_inject(const char *param)
{
	size_t len = 4 * MB;
	char *buf = anon(len), path[128];
	int st, fd;
	pid_t pid;

	fill(buf, len, 11);
	CHECK(evict(buf, len) == len / PG, "evict");
	snprintf(path, sizeof(path), "/sys/module/ssdpager/parameters/%s", param);
	fd = open(path, O_WRONLY);
	CHECK(fd >= 0 && write(fd, "1", 1) == 1, "set %s", param);
	close(fd);
	pid = fork();
	if (!pid)
		_exit(verify(buf, len, 11) ? 1 : 0);	/* first read fails */
	waitpid(pid, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGBUS, "child status %x, want SIGBUS", st);
	/* the page is still on SSD and intact for everybody else */
	CHECK(verify(buf, len, 11) == 0, "parent data after injected failure");
	printf("%s: child got SIGBUS, parent data intact\n", param);
}

/* ---- victim: knows nothing about ssdpager -------------------------------- */
static void t_victim(size_t mb)
{
	size_t len = mb * MB;
	char *buf = anon(len);

	fill(buf, len, 12);
	printf("victim pid %d ready %p %zu\n", getpid(), buf, len);
	fflush(stdout);
	for (long round = 0; ; round++) {
		long bad = verify(buf, len, 12);

		printf("victim round %ld bad %ld\n", round, bad);
		fflush(stdout);
		if (bad)
			exit(1);
		sleep(1);
	}
}

/* ---- perf: pure DRAM vs half on SSD, random one-pass, demand paging ------ */

static long period_arg(int argc, char **argv)
{
	for (int i = 2; i < argc; i++)
		if (argv[i][0] >= '0' && argv[i][0] <= '9')
			return atol(argv[i]);
	return 300;
}

static int has_arg(int argc, char **argv, const char *s)
{
	for (int i = 2; i < argc; i++)
		if (!strcmp(argv[i], s))
			return 1;
	return 0;
}

static uint64_t cnt_freq(void)
{
	uint64_t f;

	asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f;
}

static inline uint64_t rd_cnt(void)
{
	uint64_t v;

	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static void lat_report(const char *tag, uint64_t *lat, size_t n, uint64_t wall_ns,
		       long faults)
{
	uint64_t sum = 0, slow = 0, i;

	qsort(lat, n, sizeof(*lat), cmp_u64);
	for (i = 0; i < n; i++) {
		sum += lat[i];
		slow += lat[i] > 10000;
	}
	printf("  %-9s %8.3f s  mean %6.0f ns  p50 %6.0f  p90 %8.0f  p99 %8.0f  "
	       "max %9.0f ns  >10us %zu  faults %ld\n",
	       tag, (double)wall_ns / 1e9, (double)sum / n,
	       (double)lat[n / 2], (double)lat[n * 9 / 10], (double)lat[n * 99 / 100],
	       (double)lat[n - 1], (size_t)slow, faults);
	/*
	 * In a mixed run the overall quantiles straddle two populations (p50 of a
	 * half-evicted region is the fastest fault), so report the slow ones alone.
	 */
	if (slow * 100 >= n)
		printf("           stalls only: n %zu  min %.0f  p50 %.0f  p90 %.0f  "
		       "p99 %.0f ns\n", (size_t)slow, (double)lat[n - slow],
		       (double)lat[n - slow + slow / 2],
		       (double)lat[n - slow + slow * 9 / 10],
		       (double)lat[n - slow + slow * 99 / 100]);
}

static void t_perf(size_t mb, int mixed, int seq)
{
	size_t len = mb * MB, npg = len / PG, i;
	uint64_t freq = cnt_freq();
	uint64_t *lat = malloc(npg * sizeof(*lat));
	uint32_t *perm = malloc(npg * sizeof(*perm));
	long ev = 0, bad;
	uint64_t seed = 0x5eed1234;
	void *buf = anon(len);

	CHECK(lat && perm, "malloc");
	fill(buf, len, seed);

	srand(12345);
	for (i = 0; i < npg; i++)
		perm[i] = i;
	if (!seq)
		for (i = npg - 1; i > 0; i--) {
			size_t j = (size_t)rand() % (i + 1), t = perm[i];

			perm[i] = perm[j];
			perm[j] = t;
		}

	if (mixed)
		ev = evict(buf, len / 2);
	printf("perf: region %zu MiB (%zu pages), %s, %s, evicted %ld pages\n",
	       mb, npg, mixed ? "half on SSD" : "pure DRAM",
	       seq ? "sequential" : "random", ev);

	for (int pass = 1; pass <= 2; pass++) {
		long f0 = stat_field("fault ");
		uint64_t t0 = now_ns(), sum = 0;

		for (i = 0; i < npg; i++) {
			volatile unsigned char *p =
				(volatile unsigned char *)buf + (size_t)perm[i] * PG;
			uint64_t a = rd_cnt();

			sum += *p;
			lat[i] = (rd_cnt() - a) * 1000000000ULL / freq;
		}
		lat_report(pass == 1 ? "pass1" : "pass2", lat, npg, now_ns() - t0,
			   stat_field("fault ") - f0);
		(void)sum;
	}

	bad = verify(buf, len, seed);
	printf("  verify: bad %ld\n", bad);
	CHECK(!bad, "data mismatch");
	free(lat);
	free(perm);
	munmap(buf, len);
}

/* ---- V6: required lead time and prefetch throughput ---------------------- */

static uint64_t g_freq;

/* keep every thread on NUMA node 0 so promoted pages are local */
static void pin_node0(void)
{
	cpu_set_t set;
	int n = 0;

	CPU_ZERO(&set);
	for (int i = 0; i < 144; i++, n++)
		CPU_SET(i, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		printf("  (pinning failed: %s)\n", strerror(errno));
	(void)n;
}

static inline uint64_t ns2cnt(uint64_t ns)
{
	return ns * g_freq / 1000000000ULL;
}

struct lead_ctx {
	char *base;
	const uint32_t *perm;
	size_t npg;
	uint64_t t0, period_ns, lead_ns;
	/* out: issues started >10us behind schedule, worst lag, ioctl time */
	uint64_t late, max_lag, svc;
};

static void *lead_prefetch(void *arg)
{
	struct lead_ctx *c = arg;

	for (size_t i = 0; i < c->npg; i++) {
		uint64_t t = c->t0 + ns2cnt(i * c->period_ns) - ns2cnt(c->lead_ns);
		char *p = c->base + (size_t)c->perm[i] * PG;

		uint64_t a;

		while ((a = rd_cnt()) < t)
			;
		if (a - t > ns2cnt(10000))
			c->late++;
		if (a - t > c->max_lag)
			c->max_lag = a - t;
		promote(p, PG);
		c->svc += rd_cnt() - a;
	}
	return NULL;
}

/*
 * Read page i at t0 + i*period and time only the load.  A reader that is
 * behind schedule gets a longer effective lead than asked for, so count it.
 */
static void paced_read(char *base, const uint32_t *perm, size_t npg, uint64_t t0,
		       uint64_t period_ns, uint64_t *lat, uint64_t *sum,
		       uint64_t *late)
{
	*late = 0;
	for (size_t i = 0; i < npg; i++) {
		uint64_t t = t0 + ns2cnt(i * period_ns), a;

		while ((a = rd_cnt()) < t)
			;
		*late += a - t > ns2cnt(10000);
		*sum += *(volatile unsigned char *)(base + (size_t)perm[i] * PG);
		lat[i] = (rd_cnt() - a) * 1000000000ULL / g_freq;
	}
}

static void lead_point(const char *tag, char *base, const uint32_t *perm,
		       size_t npg, long lead_us, long period_us, uint64_t *lat,
		       uint64_t *sum)
{
	long f0 = stat_field("fault "), p0 = stat_field("promote ");
	uint64_t t0 = rd_cnt() + ns2cnt(lead_us * 1000 + 200000);
	uint64_t w0, rlate;
	pthread_t th;
	int pref = lead_us >= 0;
	struct lead_ctx c = {
		.base = base, .perm = perm, .npg = npg, .t0 = t0,
		.period_ns = period_us * 1000,
		.lead_ns = pref ? lead_us * 1000 : 0,
	};

	if (pref)
		pthread_create(&th, NULL, lead_prefetch, &c);
	w0 = now_ns();
	paced_read(base, perm, npg, t0, period_us * 1000, lat, sum, &rlate);
	w0 = now_ns() - w0;
	if (pref)
		pthread_join(th, NULL);
	/* measured wall time: exceeds npg * period when the reader falls behind */
	lat_report(tag, lat, npg, w0, stat_field("fault ") - f0);
	printf("           promoted %ld  reader late %lu  prefetch late %lu "
	       "maxlag %lu us  ioctl mean %lu us\n",
	       stat_field("promote ") - p0, (unsigned long)rlate,
	       (unsigned long)c.late,
	       (unsigned long)(c.max_lag * 1000000ULL / g_freq),
	       (unsigned long)(c.svc * 1000000ULL / g_freq / npg));
}

static void *bulk_promote(void *arg)
{
	promote(arg, 16 * MB);
	return NULL;
}

static void t_lead(int rnd, long period_us)
{
	static const long lead_us[] = { 0, 50, 100, 200, 300, 500, 1000, 2000, 5000 };
	size_t sl = 16 * MB, npg = sl / PG, nl = sizeof(lead_us) / sizeof(lead_us[0]), i;
	size_t total = (nl + 2) * sl;	/* lead points, "inf" slice, DRAM slice */
	uint64_t *lat = malloc(npg * sizeof(*lat)), sum = 0;
	pthread_t th;
	uint32_t *perm = malloc(npg * sizeof(*perm));
	void *base = anon(total);
	char tag[40];

	g_freq = cnt_freq();
	pin_node0();
	CHECK(lat && perm, "malloc");
	fill(base, total, 0x5eed1234);
	for (i = 0; i < npg; i++)
		perm[i] = i;
	if (rnd) {
		srand(777);
		for (i = npg - 1; i > 0; i--) {
			size_t j = (size_t)rand() % (i + 1), t = perm[i];

			perm[i] = perm[j];
			perm[j] = t;
		}
	}
	printf("lead: %zu points x %zu MiB (%zu pages), %s walk, pacing %ld us/page\n",
	       nl, sl >> 20, npg, rnd ? "random" : "sequential", period_us);

	/* DRAM reference before anything is evicted */
	lead_point("DRAM(ref)", (char *)base + (nl + 1) * sl, perm, npg, -1,
		   period_us, lat, &sum);
	evict(base, (nl + 1) * sl);
	for (i = 0; i < nl; i++) {
		snprintf(tag, sizeof(tag), "lead %ldus", lead_us[i]);
		lead_point(tag, (char *)base + i * sl, perm, npg, lead_us[i],
			   period_us, lat, &sum);
	}
	/*
	 * Controls that separate "lead too short" from "first touch after a
	 * promote": a slice promoted by another thread 1 s earlier, and a second
	 * walk over a slice that was promoted and already read once.
	 */
	pthread_create(&th, NULL, bulk_promote, (char *)base + nl * sl);
	pthread_join(th, NULL);
	sleep(1);
	lead_point("lead inf", (char *)base + nl * sl, perm, npg, -1, period_us,
		   lat, &sum);
	lead_point("re-read", (char *)base + (nl - 1) * sl, perm, npg, -1,
		   period_us, lat, &sum);
	lead_point("DRAM(last)", (char *)base + (nl + 1) * sl, perm, npg, -1,
		   period_us, lat, &sum);
	(void)sum;
	CHECK(verify(base, total, 0x5eed1234) == 0, "data mismatch");
	free(lat);
	free(perm);
	munmap(base, total);
}

struct rate_ctx {
	uint64_t *pg;
	size_t n;
	int k;		/* 0: one SP_PROMOTE per page; else SP_PROMOTEV of k pages */
	long done;
};

static void *rate_worker(void *arg)
{
	struct rate_ctx *c = arg;

	for (size_t i = 0; i < c->n; i += c->k ? c->k : 1) {
		if (!c->k)
			c->done += promote((void *)c->pg[i], PG) > 0;
		else
			c->done += sp_vec(SP_PROMOTEV, c->pg + i,
					  c->n - i < (size_t)c->k ? c->n - i : c->k,
					  0, NULL);
	}
	return NULL;
}

/* promote n scattered pages with nt threads, k pages per call */
static void rate_scat(uint64_t *pg, size_t n, int nt, int k)
{
	struct rate_ctx c[8] = { 0 };
	pthread_t th[8];
	uint64_t t0 = now_ns(), dt;
	long moved = 0;
	int i;

	for (i = 0; i < nt; i++)
		c[i] = (struct rate_ctx){ .pg = pg + i * (n / nt), .n = n / nt, .k = k };
	for (i = 0; i < nt; i++)
		pthread_create(&th[i], NULL, rate_worker, &c[i]);
	for (i = 0; i < nt; i++) {
		pthread_join(th[i], NULL);
		moved += c[i].done;
	}
	dt = now_ns() - t0;
	if (k)
		printf("  scattered  vec k=%-3d x %d thr %6.3f s  %7.0f pages/s  moved %ld\n",
		       k, nt, (double)dt / 1e9, (double)moved * 1e9 / dt, moved);
	else
		printf("  scattered  1 page/ioctl x %d thr %6.3f s  %7.0f pages/s  moved %ld\n",
		       nt, (double)dt / 1e9, (double)moved * 1e9 / dt, moved);
}

static void t_rate(int rev)
{
	static const int ks[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };
	size_t nk = sizeof(ks) / sizeof(ks[0]);
	size_t sl = 16 * MB, npg = sl / PG;
	size_t region = (nk + 8) * sl;
	void *base = anon(region);
	size_t nscat = 2048, nscat_all = 8 * npg, i;
	uint64_t *scat = malloc(nscat_all * sizeof(*scat));

	g_freq = cnt_freq();
	pin_node0();
	fill(base, region, 0x5eed1234);
	evict(base, (nk + 8) * sl);
	printf("rate: promote %zu pages per point, %s order; "
	       "slot contiguity %.1f%%\n", npg,
	       rev ? "descending" : "ascending", slot_contig(base, region));

	for (i = 0; i < nk; i++) {
		char *p = (char *)base + i * sl;
		int k = ks[i];
		uint64_t t0 = now_ns(), dt;
		size_t moved = 0, nb = (npg + k - 1) / k;

		/* rev: same batches, issued in descending page order */
		for (size_t b = 0; b < nb; b++) {
			size_t first = rev ? (nb - 1 - b) * k : b * k;
			int c = k < (int)(npg - first) ? k : (int)(npg - first);

			moved += promote(p + first * PG, (size_t)c * PG);
		}
		dt = now_ns() - t0;
		printf("  contiguous k=%-3d %6.3f s  %7.0f pages/s  %6.0f MB/s  moved %zu\n",
		       k, (double)dt / 1e9, (double)npg * 1e9 / dt,
		       (double)npg * PG / 1048576.0 * 1e9 / dt, moved);
	}

	/* scattered pages: every point gets its own nscat pages */
	static const struct { int nt, k; } sc[] = {
		{ 1, 0 }, { 4, 0 }, { 1, 8 }, { 1, 32 }, { 1, 256 }, { 4, 32 }, { 4, 256 },
	};

	CHECK(scat, "malloc");
	for (i = 0; i < nscat_all; i++)
		scat[i] = (uintptr_t)base + (nk * npg + i) * PG;
	shuffle(scat, nscat_all, 4321);
	for (i = 0; i < sizeof(sc) / sizeof(sc[0]); i++)
		rate_scat(scat + i * nscat, nscat, sc[i].nt, sc[i].k);

	CHECK(verify(base, region, 0x5eed1234) == 0, "data mismatch");
	free(scat);
	munmap(base, region);
}

/* ---- V7: slot layout, scattered batches, trace and accounting ---------- */

/*
 * Churn the slot space the way a long-running system would (random demand
 * faults punch holes, re-eviction refills) and report, for the pages written
 * in each round, how many follow their predecessor on disk.  With a small backing device the last rounds
 * run with the device full, which exercises the partial-run fallback.
 */
static void t_slots(size_t mb)
{
	size_t len = mb * MB, n = len / PG, i;
	char *buf = anon(len);
	uint64_t *pg = malloc(n * sizeof(*pg));
	long moved, nospc0 = stat_field("nospc ");

	fill(buf, len, 77);
	moved = evict(buf, len);
	printf("slots: %zu MiB; first evict moved %ld of %zu, contiguity %.1f%%\n",
	       mb, moved, n, slot_contig(buf, len));
	for (int round = 1; round <= 4; round++) {
		volatile char sink;

		for (i = 0; i < n; i++)
			pg[i] = (uintptr_t)buf + i * PG;
		shuffle(pg, n, 100 + round);
		for (i = 0; i < n / 2; i++)	/* random half faults back in */
			sink = *(volatile char *)pg[i];
		(void)sink;
		moved = evict(buf, len);
		qsort(pg, n / 2, sizeof(*pg), cmp_u64);
		printf("  round %d: re-evicted %ld, contiguity of this round %.1f%%, "
		       "nospc +%ld\n", round, moved, slot_contig_at(pg, n / 2),
		       stat_field("nospc ") - nospc0);
	}
	CHECK(verify(buf, len, 77) == 0, "data mismatch");
	munmap(buf, len);
	free(pg);
	CHECK(stat_field("slots_used ") == 0, "slots leaked");
}

static void t_vec(void)
{
	size_t len = 64 * MB, n = len / PG, i, half = n / 2;
	char *buf = anon(len);
	uint64_t *pg = malloc((n + 8) * sizeof(*pg));
	long cnt[6], dropped, done, f0;
	volatile char sink;

	fill(buf, len, 99);
	for (i = 0; i < n; i++)
		pg[i] = (uintptr_t)buf + i * PG;
	shuffle(pg, n, 7);

	/* evict a random half, in random order */
	done = sp_vec(SP_EVICTV, pg, half, 0, NULL);
	CHECK(done == (long)half, "evictv moved %ld of %zu", done, half);
	sp_census(pg, half, cnt);
	CHECK(cnt[SP_PG_SSD] == (long)half, "evicted half: %ld on SSD", cnt[SP_PG_SSD]);
	sp_census(pg + half, half, cnt);
	CHECK(cnt[SP_PG_OLD] + cnt[SP_PG_YOUNG] == (long)half, "other half left DRAM");

	/* promote a quarter: in DRAM, not yet accessed, and reading it is free */
	done = sp_vec(SP_PROMOTEV, pg, half / 2, 0, NULL);
	CHECK(done == (long)(half / 2), "promotev moved %ld", done);
	sp_census(pg, half / 2, cnt);
	CHECK(cnt[SP_PG_OLD] == (long)(half / 2), "promoted pages: old %ld young %ld",
	      cnt[SP_PG_OLD], cnt[SP_PG_YOUNG]);
	f0 = stat_field("fault ");
	for (i = 0; i < half / 4; i++)
		sink = *(volatile char *)pg[i];
	(void)sink;
	CHECK(stat_field("fault ") == f0, "promoted pages faulted");
	sp_census(pg, half / 4, cnt);
	CHECK(cnt[SP_PG_YOUNG] == (long)(half / 4), "touched pages: young %ld",
	      cnt[SP_PG_YOUNG]);
	sp_census(pg + half / 4, half / 4, cnt);
	CHECK(cnt[SP_PG_OLD] == (long)(half / 4), "untouched pages: old %ld",
	      cnt[SP_PG_OLD]);

	/* deadline already passed: nothing is read */
	done = sp_vec(SP_PROMOTEV, pg + half / 2, 1024, now_ns() - 1, &dropped);
	CHECK(done == 0 && dropped == 1024, "expired: done %ld dropped %ld", done, dropped);
	/* deadline in the middle of a long request: a prefix is done */
	done = sp_vec(SP_PROMOTEV, pg + half / 2, half / 2, now_ns() + 20000000,
		      &dropped);
	CHECK(done + dropped == (long)(half / 2) && done > 0 && dropped > 0,
	      "20 ms deadline: done %ld dropped %ld", done, dropped);
	printf("vec: 20 ms deadline on %zu pages: done %ld, dropped %ld\n",
	       half / 2, done, dropped);

	/* duplicates, unmapped and unaligned addresses are skipped, not fatal */
	uint64_t odd[6] = { pg[n - 1], pg[n - 1], 0x1000, pg[n - 2] + 123,
			    (uintptr_t)buf + len + (1UL << 30), pg[n - 1] };

	done = sp_vec(SP_EVICTV, odd, 6, 0, NULL);
	CHECK(done == 2, "odd evictv moved %ld", done);
	done = sp_vec(SP_PROMOTEV, odd, 6, 0, NULL);
	CHECK(done == 2, "odd promotev moved %ld", done);

	CHECK(verify(buf, len, 99) == 0, "data mismatch");
	munmap(buf, len);
	free(pg);
	CHECK(stat_field("slots_used ") == 0, "slots leaked");
}

struct tr_ctx {
	uint64_t *pg;
	uint32_t n;
	unsigned long cmd;
};

static void *tr_mover(void *arg)
{
	struct tr_ctx *c = arg;

	for (uint32_t i = 0; i < c->n; i += 8)
		sp_vec(c->cmd, c->pg + i, 8, 0, NULL);
	return NULL;
}

/*
 * Read pg[0..n) while another thread runs cmd on the same pages, 8 per call
 * and in the same order; returns how many reads were logged as @type.
 */
static long tr_race(uint64_t *pg, uint32_t n, unsigned long cmd, uint32_t type,
		    struct sp_event *ev, uint32_t max)
{
	struct tr_ctx c = { .pg = pg, .n = n, .cmd = cmd };
	volatile char sink;
	pthread_t th;
	uint32_t got, i;
	long hit = 0;

	pthread_create(&th, NULL, tr_mover, &c);
	for (i = 0; i < n; i++)
		sink = *(volatile char *)pg[i];
	(void)sink;
	pthread_join(th, NULL);
	got = sp_drain(ev, max);
	for (i = 0; i < got; i++)
		hit += ev[i].type == type && ev[i].pid == getpid();
	return hit;
}

static void t_trace(void)
{
	size_t len = 64 * MB, n = len / PG, i;
	uint32_t np = 1024, nmiss = 256, max = 1 << 16, got;
	char *buf = anon(len);
	uint64_t *pg = malloc(n * sizeof(*pg));
	struct sp_event *ev = malloc(max * sizeof(*ev));
	long cnt[6], bad = 0, late, f0, l0;
	volatile char sink;
	uint64_t t0, t1;

	CHECK(stat_field("wait_probe ") == 1, "kprobe on migration_entry_wait missing");
	fill(buf, len, 55);
	for (i = 0; i < n; i++)
		pg[i] = (uintptr_t)buf + i * PG;
	shuffle(pg, n, 9);
	evict(buf, len);
	sp_drain(ev, max);

	/* 1. every demand fault is logged: who, where, when, how long */
	t0 = now_ns();
	for (i = 0; i < nmiss; i++)
		sink = *(volatile char *)pg[i];
	t1 = now_ns();
	got = sp_drain(ev, max);
	CHECK(got == nmiss, "trace: %u events for %u faults", got, nmiss);
	for (i = 0; i < got; i++)
		bad += ev[i].type != SP_EV_FAULT || ev[i].addr != pg[i] ||
		       ev[i].pid != getpid() || ev[i].ts_ns < t0 ||
		       ev[i].ts_ns + ev[i].lat_ns > t1 || ev[i].lat_ns < 10000;
	CHECK(bad == 0, "trace: %ld bad records", bad);
	printf("trace: %u faults logged in order, lat %llu..%llu us\n", got,
	       (unsigned long long)ev[0].lat_ns / 1000,
	       (unsigned long long)ev[got - 1].lat_ns / 1000);

	/*
	 * 2. prefetch accounting.  P = np prefetched pages; the application then
	 * reads half of P plus nmiss pages nobody prefetched.
	 */
	uint64_t *P = pg + nmiss, *M = P + np;

	f0 = stat_field("fault ");
	l0 = stat_field("late ");
	CHECK(sp_vec(SP_PROMOTEV, P, np, 0, NULL) == np, "prefetch");
	for (i = 0; i < np / 2; i++)
		sink = *(volatile char *)P[i];
	for (i = 0; i < nmiss; i++)
		sink = *(volatile char *)M[i];
	sp_census(P, np, cnt);
	got = sp_drain(ev, max);
	printf("accounting: prefetched %u -> hit %ld, wasted %ld, late %ld; "
	       "missed (faults) %u\n", np, cnt[SP_PG_YOUNG], cnt[SP_PG_OLD],
	       stat_field("late ") - l0, got);
	if (cnt[SP_PG_YOUNG] != np / 2) {
		uint8_t *st = malloc(np);

		sp_query(P, np, st);
		for (i = 0; i < np; i++)
			if ((st[i] == SP_PG_YOUNG) != (i < np / 2))
				printf("  unexpected: P[%zu] page %zu state %d\n", i,
				       (size_t)((P[i] - (uintptr_t)buf) / PG), st[i]);
		free(st);
	}
	CHECK(cnt[SP_PG_YOUNG] == np / 2 && cnt[SP_PG_OLD] == np / 2, "hit/waste");
	CHECK(got == nmiss && stat_field("fault ") - f0 == nmiss, "misses");
	for (i = 0, bad = 0; i < got; i++)
		bad += ev[i].type != SP_EV_FAULT || ev[i].addr != M[i];
	CHECK(bad == 0, "miss records");

	/* 3. late: the reader gets to the pages while the prefetch is in flight */
	uint64_t *L = M + nmiss;

	f0 = stat_field("fault ");
	late = tr_race(L, 2048, SP_PROMOTEV, SP_EV_LATE, ev, max);
	printf("race, 2048 reads against a prefetcher on the same pages: "
	       "late %ld, missed %ld, in time %ld\n", late,
	       stat_field("fault ") - f0, 2048 - late - (stat_field("fault ") - f0));
	CHECK(late > 0 && late == stat_field("late ") - l0, "late accounting");

	/* 4. evict_wait: the reader hits pages that are being written out */
	l0 = stat_field("evict_wait ");
	late = tr_race(P, np, SP_EVICTV, SP_EV_EVICT_WAIT, ev, max);
	printf("evict_wait: %ld reads waited for an in-flight eviction "
	       "(stat +%ld)\n", late, stat_field("evict_wait ") - l0);
	CHECK(late == stat_field("evict_wait ") - l0, "evict_wait accounting");

	(void)sink;
	CHECK(verify(buf, len, 55) == 0, "data mismatch");
	munmap(buf, len);
	free(pg);
	free(ev);
	CHECK(stat_field("slots_used ") == 0, "slots leaked");
}

/* ---- external control: every private mapping of a pid -------------------- */
static void ctl_all(unsigned long cmd, pid_t pid)
{
	char path[64], line[512], perms[8];
	unsigned long s, e;
	long done = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	f = fopen(path, "r");
	CHECK(f, "open %s", path);
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3 && perms[3] == 'p')
			done += sp_range(cmd, pid, (void *)s, e - s);
	fclose(f);
	printf("%s pid %d: %ld pages\n", cmd == SP_EVICT ? "evict" : "promote", pid, done);
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";
	long arg = argc > 2 ? atol(argv[2]) : 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!strcmp(c, "victim")) {
		t_victim(arg ? arg : 256);
		return 0;
	}
	sp_open();
	if (!strcmp(c, "basic")) t_basic(arg ? arg : 1024);
	else if (!strcmp(c, "fork")) t_fork();
	else if (!strcmp(c, "threads")) t_threads(arg ? arg : 16);
	else if (!strcmp(c, "vmops")) t_vmops();
	else if (!strcmp(c, "gup")) t_gup();
	else if (!strcmp(c, "thp")) t_thp(arg);
	else if (!strcmp(c, "mlock")) t_mlock();
	else if (!strcmp(c, "stress")) t_stress(arg ? arg : 60);
	else if (!strcmp(c, "inject")) t_inject(argc > 2 ? argv[2] : "inject_eio");
	else if (!strcmp(c, "evict") && arg) ctl_all(SP_EVICT, arg);
	else if (!strcmp(c, "promote") && arg) ctl_all(SP_PROMOTE, arg);
	else if (!strcmp(c, "perf")) t_perf(arg ? arg : 2048, has_arg(argc, argv, "mixed"),
					    has_arg(argc, argv, "seq"));
	else if (!strcmp(c, "lead")) t_lead(has_arg(argc, argv, "rand"),
					    period_arg(argc, argv));
	else if (!strcmp(c, "rate")) t_rate(has_arg(argc, argv, "rev"));
	else if (!strcmp(c, "slots")) t_slots(arg ? arg : 256);
	else if (!strcmp(c, "vec")) t_vec();
	else if (!strcmp(c, "trace")) t_trace();
	else if (!strcmp(c, "evacuate")) printf("evacuated %d pages\n", ioctl(spfd, SP_EVACUATE));
	else if (!strcmp(c, "stat")) { show_stat(); return 0; }
	else {
		fprintf(stderr, "usage: sptest basic [MB]|fork|threads [N]|vmops|gup|thp [wait_s]|mlock|stress [s]|inject PARAM|victim [MB]|perf [MB] [mixed|seq]|lead [rand] [period_us]|rate [rev]|slots [MB]|vec|trace|evict PID|promote PID|evacuate|stat\n");
		return 2;
	}
	printf("PASS %s\n", c);
	return 0;
}
