// SPDX-License-Identifier: GPL-2.0
/*
 * spd: ssdpager policy daemon for one process.
 *
 * Scan thread: every interval, read and clear the access flag of the target's
 * private anonymous pages (SP_SCAN), age the pages that were not touched, and
 * evict the oldest ones until the resident set fits the budget.
 *
 * Prefetch thread: drain the demand-fault trace (SP_TRACE), ask a predictor
 * which pages come next, and bring those back with SP_PROMOTEV.  A prefetched
 * page is a hit when a later scan finds it accessed, wasted when it stays
 * untouched for SP_PF_SCANS scans or is evicted again.
 */
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
#include <time.h>
#include <unistd.h>
#include "ssdpager.h"

#define PG		4096UL
#define SCAN_PAGES	(1UL << 18)	/* SP_SCAN limit */
#define VEC_MAX		4096U
#define TRACE_MAX	4096U
#define PF_BATCH	256U
#define PF_SCANS	2		/* scans a prefetched page may stay unused */
#define MK_BITS		20		/* successor table: 2^20 pages */
#define MK_WAYS		2

enum { P_NONE, P_NEXT, P_MARKOV, P_BOTH };

struct region {
	uint64_t start, end;
	uint8_t *age;	/* scans since last seen accessed, saturating */
	uint8_t *pf;	/* 0, or 1 + scans since it was prefetched */
};

static int fd, pid, predictor = P_NONE;
static unsigned long budget_pages, rate_pages = 20000;
static unsigned int interval_ms = 1000, min_age = 1, pf_width = 8;
static uint64_t pf_deadline_ns = 5000000;
static volatile sig_atomic_t stop;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;	/* regions and their arrays */
static struct region *regs;
static size_t nregs;

static struct { uint64_t page, succ[MK_WAYS]; } *mk;

/* totals; the prefetch thread owns the pf_ and ev_ ones except hit/waste */
static unsigned long n_evict, n_hit, n_waste;
static unsigned long ev_fault, ev_late, ev_evict_wait, ev_lost, pf_asked, pf_done, pf_dropped;

static void die(const char *m)
{
	perror(m);
	exit(1);
}

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000ULL + t.tv_nsec;
}

/* ---- regions ---------------------------------------------------------- */

static struct region *region_of(uint64_t addr)
{
	size_t lo = 0, hi = nregs;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;

		if (addr < regs[mid].start)
			hi = mid;
		else if (addr >= regs[mid].end)
			lo = mid + 1;
		else
			return &regs[mid];
	}
	return NULL;
}

/*
 * Re-read the target's writable private anonymous mappings, carrying the
 * per-page history over wherever old and new mappings overlap.
 * Returns 0 once the process is gone.
 */
static int regions_refresh(void)
{
	struct region *nr = NULL, *old;
	size_t n = 0, cap = 0, i, j, oldn;
	char path[64], line[512];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long s, e, ino;
		char perm[8], name[256] = "";

		if (sscanf(line, "%lx-%lx %7s %*x %*s %lu %255s", &s, &e, perm, &ino, name) < 4)
			continue;
		if (strcmp(perm, "rw-p") || ino || (name[0] && strcmp(name, "[heap]")))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			nr = realloc(nr, cap * sizeof(*nr));
			if (!nr)
				die("realloc");
		}
		nr[n].start = s;
		nr[n].end = e;
		nr[n].age = calloc((e - s) / PG, 1);
		nr[n].pf = calloc((e - s) / PG, 1);
		if (!nr[n].age || !nr[n].pf)
			die("calloc");
		n++;
	}
	fclose(f);

	pthread_mutex_lock(&lock);
	for (i = 0, j = 0; i < n && j < nregs; ) {
		uint64_t s = nr[i].start > regs[j].start ? nr[i].start : regs[j].start;
		uint64_t e = nr[i].end < regs[j].end ? nr[i].end : regs[j].end;

		if (s < e) {
			memcpy(nr[i].age + (s - nr[i].start) / PG,
			       regs[j].age + (s - regs[j].start) / PG, (e - s) / PG);
			memcpy(nr[i].pf + (s - nr[i].start) / PG,
			       regs[j].pf + (s - regs[j].start) / PG, (e - s) / PG);
		}
		if (nr[i].end < regs[j].end)
			i++;
		else
			j++;
	}
	old = regs;
	oldn = nregs;
	regs = nr;
	nregs = n;
	pthread_mutex_unlock(&lock);
	for (j = 0; j < oldn; j++) {
		free(old[j].age);
		free(old[j].pf);
	}
	free(old);
	return 1;
}

/* ---- scan and evict --------------------------------------------------- */

static unsigned long evict_vec(uint64_t *addrs, unsigned long n)
{
	unsigned long done = 0, i;

	for (i = 0; i < n && !stop; i += VEC_MAX) {
		struct sp_vec v = { .pid = pid, .addrs = (uintptr_t)(addrs + i),
				    .nr = n - i < VEC_MAX ? n - i : VEC_MAX };

		if (ioctl(fd, SP_EVICTV, &v))
			break;
		done += v.done;
	}
	return done;
}

static void *scan_thread(void *arg)
{
	uint8_t *st = malloc(SCAN_PAGES);
	uint64_t t0 = now_ns();
	uint64_t *victims = malloc((rate_pages * interval_ms / 1000 + 1) * sizeof(*victims));

	if (!st || !victims)
		die("malloc");
	while (!stop && regions_refresh()) {
		unsigned long resident = 0, ssd = 0, hist[256] = { 0 };
		unsigned long need, nv = 0, got = 0, left;
		uint64_t t = now_ns(), ts;
		unsigned int thr;
		size_t r;

		/* regs is replaced only by this thread, so it may read it unlocked */
		for (r = 0; r < nregs; r++) {
			struct region *g = &regs[r];
			uint64_t a;

			for (a = g->start; a < g->end; a += SCAN_PAGES * PG) {
				unsigned long n = (g->end - a) / PG, base = (a - g->start) / PG, i;
				struct sp_scan s = { .pid = pid, .flags = SP_SCAN_CLEAR,
						     .addr = a, .states = (uintptr_t)st };

				if (n > SCAN_PAGES)
					n = SCAN_PAGES;
				s.len = n * PG;
				if (ioctl(fd, SP_SCAN, &s))
					continue;
				pthread_mutex_lock(&lock);
				for (i = 0; i < n; i++) {
					uint8_t *age = &g->age[base + i], *pf = &g->pf[base + i];

					switch (st[i]) {
					case SP_PG_YOUNG:
						*age = 0;
						n_hit += !!*pf;
						*pf = 0;
						resident++;
						break;
					case SP_PG_OLD:
						if (*age < 255)
							++*age;
						if (*pf && ++*pf > 1 + PF_SCANS) {
							n_waste++;
							*pf = 0;
						}
						hist[*age]++;
						resident++;
						break;
					case SP_PG_SSD:
						*age = 0;
						/* a prefetch that was dropped or failed leaves no trace */
						*pf = 0;
						ssd++;
						break;
					}
				}
				pthread_mutex_unlock(&lock);
			}
		}
		ts = now_ns() - t;

		need = resident > budget_pages ? resident - budget_pages : 0;
		left = rate_pages * interval_ms / 1000;
		if (need > left)
			need = left;
		/* oldest first: the lowest age that still yields `need` pages */
		for (thr = 255, left = 0; thr > min_age && left + hist[thr] < need; thr--)
			left += hist[thr];
		for (r = 0; r < nregs && nv < need; r++) {
			struct region *g = &regs[r];
			unsigned long n = (g->end - g->start) / PG, i;

			for (i = 0; i < n && nv < need; i++)
				if (g->age[i] >= thr && g->age[i]) {
					victims[nv++] = g->start + i * PG;
					/* an unused prefetched page going back out */
					pthread_mutex_lock(&lock);
					n_waste += !!g->pf[i];
					g->pf[i] = 0;
					pthread_mutex_unlock(&lock);
					g->age[i] = 0;
				}
		}
		if (nv)
			got = evict_vec(victims, nv);
		n_evict += got;
		printf("t=%.1f resident_mb=%lu ssd_mb=%lu scan_ms=%.1f want=%lu evict=%lu age>=%u "
		       "fault=%lu late=%lu evict_wait=%lu pf_asked=%lu pf_done=%lu pf_dropped=%lu "
		       "hit=%lu waste=%lu lost=%lu\n",
		       (t - t0) / 1e9, resident >> 8, ssd >> 8, ts / 1e6, need, got, thr,
		       ev_fault, ev_late, ev_evict_wait, pf_asked, pf_done, pf_dropped,
		       n_hit, n_waste, ev_lost);
		fflush(stdout);
		t += interval_ms * 1000000ULL;
		while (!stop && now_ns() < t)
			usleep(10000);
	}
	stop = 1;
	return NULL;
}

/* ---- predictors ------------------------------------------------------- */

static size_t mk_slot(uint64_t page)
{
	size_t h = (page / PG * 0x9e3779b97f4a7c15ULL) >> (64 - MK_BITS);

	return h;
}

/* Remember that `next` faulted right after `page` (direct-mapped, newest first). */
static void mk_learn(uint64_t page, uint64_t next)
{
	size_t h = mk_slot(page);

	if (mk[h].page != page) {
		mk[h].page = page;
		memset(mk[h].succ, 0, sizeof(mk[h].succ));
	}
	if (mk[h].succ[0] == next)
		return;
	memmove(&mk[h].succ[1], &mk[h].succ[0], (MK_WAYS - 1) * sizeof(uint64_t));
	mk[h].succ[0] = next;
}

static unsigned int predict(uint64_t page, uint64_t *out, unsigned int n, unsigned int max)
{
	unsigned int i;

	if (predictor == P_NEXT || predictor == P_BOTH)
		for (i = 1; i <= pf_width && n < max; i++)
			out[n++] = page + i * PG;
	if (predictor == P_MARKOV || predictor == P_BOTH) {
		uint64_t cur = page;

		/* follow the most recent successor chain, plus the alternates */
		for (i = 0; i < pf_width && n < max; i++) {
			size_t h = mk_slot(cur);
			unsigned int w;

			if (mk[h].page != cur || !mk[h].succ[0])
				break;
			for (w = 0; w < MK_WAYS && n < max; w++)
				if (mk[h].succ[w])
					out[n++] = mk[h].succ[w];
			cur = mk[h].succ[0];
		}
	}
	return n;
}

static void *prefetch_thread(void *arg)
{
	struct sp_event *ev = malloc(TRACE_MAX * sizeof(*ev));
	uint64_t cand[PF_BATCH], want[PF_BATCH], last = 0;
	uint8_t st[PF_BATCH];

	if (!ev)
		die("malloc");
	while (!stop) {
		struct sp_trace t = { .buf = (uintptr_t)ev, .nr = TRACE_MAX };
		unsigned int i, j, nc = 0, nw = 0;

		if (ioctl(fd, SP_TRACE, &t))
			die("SP_TRACE");
		ev_lost = t.lost;
		if (!t.got) {
			usleep(200);
			continue;
		}
		for (i = 0; i < t.got; i++) {
			if (ev[i].pid != pid)
				continue;
			if (ev[i].type == SP_EV_LATE)
				ev_late++;
			else if (ev[i].type == SP_EV_EVICT_WAIT)
				ev_evict_wait++;
			if (ev[i].type != SP_EV_FAULT)
				continue;
			ev_fault++;
			if (predictor == P_MARKOV || predictor == P_BOTH) {
				if (last)
					mk_learn(last, ev[i].addr);
				last = ev[i].addr;
			}
		}
		if (predictor == P_NONE)
			continue;
		/* the newest faults say most about what comes next */
		for (i = t.got; i-- > 0 && nc < PF_BATCH; )
			if (ev[i].pid == pid && ev[i].type == SP_EV_FAULT)
				nc = predict(ev[i].addr, cand, nc, PF_BATCH);
		if (nc) {
			struct sp_query q = { .pid = pid, .nr = nc, .addrs = (uintptr_t)cand,
					      .states = (uintptr_t)st };

			if (ioctl(fd, SP_QUERY, &q))
				continue;
		}
		for (i = 0; i < nc; i++) {
			if (st[i] != SP_PG_SSD)
				continue;
			for (j = 0; j < nw && want[j] != cand[i]; j++)
				;
			if (j == nw)
				want[nw++] = cand[i];
		}
		if (nw) {
			struct sp_vec v = { .pid = pid, .nr = nw, .addrs = (uintptr_t)want,
					    .deadline_ns = now_ns() + pf_deadline_ns };
			struct sp_query q = { .pid = pid, .nr = nw, .addrs = (uintptr_t)want,
					      .states = (uintptr_t)st };

			pf_asked += nw;
			if (ioctl(fd, SP_PROMOTEV, &v))
				continue;
			pf_done += v.done;
			pf_dropped += v.dropped;
			/* which of them made it: the rest must not count as hits */
			if (!v.done || ioctl(fd, SP_QUERY, &q))
				continue;
			pthread_mutex_lock(&lock);
			for (i = 0; i < nw; i++) {
				struct region *g = region_of(want[i]);

				if (g && (st[i] == SP_PG_OLD || st[i] == SP_PG_YOUNG))
					g->pf[(want[i] - g->start) / PG] = 1;
			}
			pthread_mutex_unlock(&lock);
		}
	}
	return NULL;
}

static void on_signal(int sig)
{
	stop = 1;
}

int main(int argc, char **argv)
{
	static const char *const names[] = { "none", "next", "markov", "both" };
	pthread_t ts, tp;
	int c;

	while ((c = getopt(argc, argv, "p:b:P:i:r:a:w:d:")) != -1) {
		switch (c) {
		case 'p': pid = atoi(optarg); break;
		case 'b': budget_pages = strtoul(optarg, NULL, 0) << 8; break;
		case 'i': interval_ms = atoi(optarg); break;
		case 'r': rate_pages = strtoul(optarg, NULL, 0); break;
		case 'a': min_age = atoi(optarg); break;
		case 'w': pf_width = atoi(optarg); break;
		case 'd': pf_deadline_ns = strtoull(optarg, NULL, 0) * 1000; break;
		case 'P':
			for (predictor = 0; predictor < 4 && strcmp(optarg, names[predictor]); predictor++)
				;
			break;
		default: pid = 0;
		}
	}
	if (pid <= 0 || !budget_pages || predictor > P_BOTH || !interval_ms || !min_age ||
	    min_age > 255 || !rate_pages) {
		fprintf(stderr,
			"usage: spd -p PID -b BUDGET_MB [-P none|next|markov|both] [-i scan_ms=1000]\n"
			"           [-r evict_pages_per_s=20000] [-a min_age_scans=1]\n"
			"           [-w prefetch_width=8] [-d prefetch_deadline_us=5000]\n");
		return 2;
	}
	fd = open("/dev/ssdpager", O_RDWR);
	if (fd < 0)
		die("/dev/ssdpager");
	mk = calloc(1UL << MK_BITS, sizeof(*mk));
	if (!mk)
		die("calloc");
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	pthread_create(&ts, NULL, scan_thread, NULL);
	pthread_create(&tp, NULL, prefetch_thread, NULL);
	pthread_join(ts, NULL);
	pthread_join(tp, NULL);
	printf("total evict=%lu fault=%lu late=%lu pf_asked=%lu pf_done=%lu hit=%lu waste=%lu\n",
	       n_evict, ev_fault, ev_late, pf_asked, pf_done, n_hit, n_waste);
	return 0;
}
