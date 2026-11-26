// SPDX-License-Identifier: GPL-2.0
/*
 * mctrace: replay a request trace against memcached.
 *
 * TRACE is what twprep.py writes: 12-byte records { u32 key, u32 value size,
 * u16 key size, u16 op } with op 0 get, 1 set, 2 delete.  Records
 * [skip, skip + ops) are replayed; a key always goes to the same connection,
 * so its requests keep their order.  A GET that misses is followed by a SET
 * of that key (not timed), like a cache in front of a backend.
 *
 * With qps > 0 record i is due at start + i / qps and latency is measured from
 * the due time, so a stalled server shows up as latency, not as lower load.
 * With qps 0 every connection sends as fast as it gets answers.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PG		4096UL
/* memcached 1.6 item header, same offsets as bench/mcloc.c uses */
#define IT_NBYTES	32
#define IT_FLAGS	38
#define IT_NKEY		41
#define IT_LINKED	1
#define IT_CAS		2
#define KEYMIN	10
#define KEYMAX	250
#define VALMAX	(1L << 20)

struct rec {
	uint32_t key, vsize;
	uint16_t ksize, op;
};

struct worker {
	pthread_t th;
	unsigned int id;
	long done, err, gets, miss;
	int *lat;
};

static const char *host;
static int port, threads;
static const struct rec *trace;
static long ops;
static double qps;
static struct timespec start;
static char *value;

static void die(const char *m)
{
	perror(m);
	exit(1);
}

static int send_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = send(fd, p, n, 0);

		if (w <= 0)
			return -1;
		p += w;
		n -= w;
	}
	return 0;
}

/* Read one reply that ends in tail; 1 when the reply is nothing but tail. */
static int reply(int fd, char *buf, size_t cap, const char *tail)
{
	size_t off = 0, tl = strlen(tail);

	while (off < cap) {
		ssize_t r = recv(fd, buf + off, cap - off, 0);

		if (r <= 0)
			return -1;
		off += r;
		if (off >= tl && !memcmp(buf + off - tl, tail, tl))
			return off == tl;
	}
	return -1;
}

static int put_key(char *p, const struct rec *r)
{
	int n = sprintf(p, "k%09u", r->key), want = r->ksize;

	if (want > KEYMAX)
		want = KEYMAX;
	while (n < want)
		p[n++] = 'x';
	return n;
}

static double since_start(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec - start.tv_sec + (t.tv_nsec - start.tv_nsec) / 1e9;
}

static void *work(void *p)
{
	struct worker *w = p;
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	size_t cap = VALMAX + 1024;
	char *req = malloc(cap), *resp = malloc(cap);
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	long i;

	w->lat = malloc(ops * sizeof(int));
	if (fd < 0 || !req || !resp || !w->lat || inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
	    connect(fd, (struct sockaddr *)&sa, sizeof(sa)))
		die("connect");
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	for (i = 0; i < ops; i++) {
		const struct rec *r = &trace[i];
		long vs = r->vsize < VALMAX ? r->vsize : VALMAX;
		double due = qps > 0 ? i / qps : 0, now;
		int n, res, set;

		if (r->key % threads != w->id)
			continue;
		now = since_start();
		if (qps > 0 && due > now) {
			struct timespec d = { .tv_sec = (time_t)(due - now),
					      .tv_nsec = (long)((due - now - (time_t)(due - now)) * 1e9) };

			nanosleep(&d, NULL);
		}
		if (qps <= 0)
			due = now;
		set = r->op == 1;
		if (r->op == 2) {
			n = sprintf(req, "delete ");
			n += put_key(req + n, r);
			n += sprintf(req + n, "\r\n");
			res = send_all(fd, req, n) ? -1 : reply(fd, resp, cap, "\r\n");
		} else if (!set) {
			n = sprintf(req, "get ");
			n += put_key(req + n, r);
			n += sprintf(req + n, "\r\n");
			res = send_all(fd, req, n) ? -1 : reply(fd, resp, cap, "END\r\n");
			w->gets++;
		} else {
			res = 0;
		}
		if (res >= 0 && r->op != 2 && (set || res == 1)) {
			int timed = set;
			double t = since_start();

			w->miss += !set;
			if (!set)	/* the GET ends here, the refill is not part of it */
				w->lat[w->done] = (t - due) * 1e6;
			n = sprintf(req, "set ");
			n += put_key(req + n, r);
			n += sprintf(req + n, " 0 0 %ld\r\n", vs);
			memcpy(req + n, value, vs);
			memcpy(req + n + vs, "\r\n", 2);
			res = send_all(fd, req, n + vs + 2) ? -1 : reply(fd, resp, cap, "\r\n");
			if (res >= 0 && timed)
				w->lat[w->done] = (since_start() - due) * 1e6;
		} else if (res >= 0) {
			w->lat[w->done] = (since_start() - due) * 1e6;
		}
		if (res < 0) {
			w->err++;
			break;
		}
		w->done++;
	}
	close(fd);
	free(req);
	free(resp);
	return NULL;
}

static int cmp_int(const void *x, const void *y)
{
	return (*(const int *)x > *(const int *)y) - (*(const int *)x < *(const int *)y);
}

/* ---- map: every key's item address, for page-level analysis ---------------- */

#define IT_PAD		48	/* key offset when the item carries no CAS field */
#define SCAN_CHUNK	(4UL << 20)
#define SCAN_CARRY	512UL

struct addr {
	uint64_t addr;
	uint32_t nbytes;
	uint32_t ksize;
};

/* Is the "k%09u"+"x"* at b[i] the key of a linked item of ours?  Returns the
 * header length (48, or 56 with CAS), 0 when no; sets *k and *nk. */
static unsigned int item_at(const unsigned char *b, size_t i, unsigned int *k, unsigned int *nk)
{
	unsigned int hdr, j;

	for (*k = 0, j = 1; j < 10; j++) {
		if (b[i + j] < '0' || b[i + j] > '9')
			return 0;
		*k = *k * 10 + b[i + j] - '0';
	}
	for (hdr = 48; hdr <= 56; hdr += 8) {
		const unsigned char *it = b + i - hdr;
		int32_t nbytes;
		uint16_t flags;
		unsigned int n;

		if (i < hdr)
			continue;
		memcpy(&nbytes, it + IT_NBYTES, sizeof(nbytes));
		memcpy(&flags, it + IT_FLAGS, sizeof(flags));
		n = it[IT_NKEY];
		if (n < 10 || n > KEYMAX || nbytes < 2 || nbytes > (int32_t)VALMAX + 2 ||
		    !(flags & IT_LINKED) || !!(flags & IT_CAS) != (hdr == 56))
			continue;
		for (j = 10; j < n; j++)
			if (b[i + j] != 'x')
				break;
		if (j < n)
			continue;
		*nk = n;
		return hdr;
	}
	return 0;
}

static int do_map(int pid, unsigned long nkeys, const char *out)
{
	unsigned char *buf = malloc(SCAN_CARRY + SCAN_CHUNK);
	char path[64], line[512];
	struct addr *map = calloc(nkeys, sizeof(*map));
	unsigned long cand = 0, bytes = 0;
	unsigned long found = 0, dup = 0;
	FILE *maps, *f;
	int mem;

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	maps = fopen(path, "r");
	snprintf(path, sizeof(path), "/proc/%d/mem", pid);
	mem = open(path, O_RDONLY);
	if (!buf || !map || !maps || mem < 0)
		die("map");
	while (fgets(line, sizeof(line), maps)) {
		unsigned long s, e, ino, a;
		char perm[8], name[256] = "";
		size_t have = 0;

		if (sscanf(line, "%lx-%lx %7s %*x %*s %lu %255s", &s, &e, perm, &ino, name) < 4)
			continue;
		if (strcmp(perm, "rw-p") || ino || (name[0] && strcmp(name, "[heap]")))
			continue;
		for (a = s; a < e; ) {
			size_t want = e - a < SCAN_CHUNK ? e - a : SCAN_CHUNK, i;
			ssize_t r = pread(mem, buf + have, want, a);

			if (r <= 0) {
				a += PG;
				have = 0;
				continue;
			}
			a += r;
			have += r;
			bytes += r;
			for (i = 56; i + KEYMAX + 56 <= have; i++) {
				unsigned int k, nk, hdr;
				uint64_t at;

				if (buf[i] != 'k')
					continue;
				cand++;
				if (!(hdr = item_at(buf, i, &k, &nk)) || k >= nkeys)
					continue;
				at = a - have + i - hdr;
				dup += map[k].addr && map[k].addr != at;
				if (!map[k].addr) {
					map[k].addr = at;
					memcpy(&map[k].nbytes, buf + i - hdr + IT_NBYTES, 4);
					map[k].ksize = nk;
					found++;
				}
			}
			if (have > SCAN_CARRY) {
				memmove(buf, buf + have - SCAN_CARRY, SCAN_CARRY);
				have = SCAN_CARRY;
			}
		}
	}
	f = fopen(out, "w");
	if (!f || fwrite(map, sizeof(*map), nkeys, f) != nkeys || fclose(f))
		die(out);
	printf("map items=%lu dup=%lu scanned=%lu MiB candidates=%lu\n", found, dup, bytes >> 20, cand);
	return 0;
}

int main(int argc, char **argv)
{
	struct worker *w;
	struct timespec t1;
	struct stat st;
	long done = 0, err = 0, gets = 0, miss = 0, skip, i;
	int t, fd, *all;
	double dt;
	void *m;

	if (argc == 5 && !strcmp(argv[1], "map"))
		return do_map(atoi(argv[2]), strtoul(argv[3], NULL, 10), argv[4]);
	if (argc != 7 && argc != 8) {
		fprintf(stderr, "usage: mctrace host port threads TRACE skip ops [qps]\n"
				"       mctrace map PID keys MAP\n");
		return 2;
	}
	host = argv[1];
	port = atoi(argv[2]);
	threads = atoi(argv[3]);
	skip = atol(argv[5]);
	ops = atol(argv[6]);
	qps = argc == 8 ? atof(argv[7]) : 0;
	fd = open(argv[4], O_RDONLY);
	if (fd < 0 || fstat(fd, &st))
		die(argv[4]);
	if (threads < 1 || skip < 0 || ops < 1 || (skip + ops) * (long)sizeof(struct rec) > st.st_size) {
		fprintf(stderr, "trace has %ld records\n", (long)(st.st_size / sizeof(struct rec)));
		return 2;
	}
	m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
	value = malloc(VALMAX);
	w = calloc(threads, sizeof(*w));
	all = malloc(ops * sizeof(int));
	if (m == MAP_FAILED || !value || !w || !all)
		die("alloc");
	trace = (const struct rec *)m + skip;
	for (i = 0; i < VALMAX; i++)
		value[i] = 'a' + i % 26;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (t = 0; t < threads; t++) {
		w[t].id = t;
		if (pthread_create(&w[t].th, NULL, work, &w[t]))
			die("pthread_create");
	}
	for (t = 0; t < threads; t++) {
		pthread_join(w[t].th, NULL);
		memcpy(all + done, w[t].lat, w[t].done * sizeof(int));
		done += w[t].done;
		err += w[t].err;
		gets += w[t].gets;
		miss += w[t].miss;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	dt = t1.tv_sec - start.tv_sec + (t1.tv_nsec - start.tv_nsec) / 1e9;
	if (!done)
		return 1;
	qsort(all, done, sizeof(int), cmp_int);
	printf("steady ops=%ld seconds=%.3f rate=%.0f/s p50=%dus p99=%dus p999=%dus err=%ld get_pct=%ld pattern=trace hint_lost=0 miss=%ld qps=%.0f\n",
	       done, dt, done / dt, all[done / 2], all[done * 99 / 100], all[done * 999 / 1000],
	       err, gets * 100 / done, miss, qps);
	return 0;
}
