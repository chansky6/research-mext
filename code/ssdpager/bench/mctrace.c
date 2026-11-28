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
 *
 * Oracle hints: given the item map `mctrace map` wrote (take it while
 * everything is in DRAM), a FIFO and LOOKAHEAD, each connection writes the
 * pages of the item it will ask for LOOKAHEAD of its own requests from now to
 * the FIFO as raw u64 addresses (spd -P hint).  A SET moves the item, so its
 * key is not hinted again.  Without CHAIN only the item itself is hinted.
 * CHAIN (written by `mctrace map ... CHAIN`) names, per key, the items
 * assoc_find() compares before reaching it (its hash bucket from the head
 * down, found by inverting h_next, at most CHAIN_UP of them); their header and
 * key pages are hinted too.  LRU neighbours are never hinted.
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

struct addr {
	uint64_t addr;
	uint32_t nbytes;
	uint32_t ksize;
};

struct worker {
	pthread_t th;
	unsigned int id;
	long done, err, gets, miss;
	long ahead, hinted, hint_lost;	/* trace cursor of the hints, own requests hinted */
	int *lat;
};

static const char *host;
static int port, threads, hint_fd = -1;
static struct addr *imap;	/* item of each key, addr 0 when unknown */
static unsigned long imap_keys;
static long lookahead;
#define CHAIN_UP	7
#define NOKEY		0xffffffffU
static uint32_t *chain;		/* CHAIN_UP keys per key, nearest first, NOKEY when none */
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

static void hint(struct worker *w, uint32_t key)
{
	uint64_t pages[4 + 2 * CHAIN_UP], a, end;
	size_t n = 0;
	int e;

	if (key >= imap_keys || !imap[key].addr)
		return;
	end = imap[key].addr + 56 + imap[key].ksize + imap[key].nbytes + 2;
	for (a = imap[key].addr & ~(PG - 1); a < end && n < 4; a += PG)
		pages[n++] = a;
	for (e = 0; chain && e < CHAIN_UP; e++) {
		uint32_t k = chain[(size_t)key * CHAIN_UP + e];

		if (k == NOKEY)
			break;
		if (!imap[k].addr)
			continue;
		end = imap[k].addr + 56 + imap[k].ksize;
		for (a = imap[k].addr & ~(PG - 1); a < end; a += PG)
			pages[n++] = a;
	}
	if (write(hint_fd, pages, n * sizeof(*pages)) < 0)
		w->hint_lost++;
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
		for (; imap && w->ahead < ops && w->hinted <= w->done + lookahead; w->ahead++) {
			if (trace[w->ahead].key % threads != w->id)
				continue;
			w->hinted++;
			hint(w, trace[w->ahead].key);
		}
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
			if (imap && r->key < imap_keys)
				imap[r->key].addr = 0;
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
#define IT_HNEXT	16
#define SCAN_CHUNK	(4UL << 20)
#define SCAN_CARRY	512UL


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

struct ak {
	uint64_t addr;
	uint32_t key;
};

static int cmp_ak(const void *x, const void *y)
{
	uint64_t a = ((const struct ak *)x)->addr, b = ((const struct ak *)y)->addr;

	return (a > b) - (a < b);
}

/* For every key, the keys whose items precede it in its hash bucket. */
static void write_chain(const struct addr *map, const uint64_t *hnext, unsigned long nkeys, const char *out)
{
	struct ak *by = malloc(nkeys * sizeof(*by)), want;
	uint32_t *pred = malloc(nkeys * sizeof(*pred));
	uint32_t *ch = malloc(nkeys * CHAIN_UP * sizeof(*ch));
	unsigned long k, n = 0, links = 0, hist[CHAIN_UP + 1] = { 0 };
	FILE *f;
	int e, j;

	if (!by || !pred || !ch)
		die("malloc");
	for (k = 0; k < nkeys; k++) {
		pred[k] = NOKEY;
		if (map[k].addr)
			by[n++] = (struct ak){ map[k].addr, k };
	}
	qsort(by, n, sizeof(*by), cmp_ak);
	for (k = 0; k < nkeys; k++) {
		struct ak *hit;

		if (!map[k].addr || !hnext[k])
			continue;
		want.addr = hnext[k];
		hit = bsearch(&want, by, n, sizeof(*by), cmp_ak);
		if (hit) {
			pred[hit->key] = k;
			links++;
		}
	}
	for (k = 0; k < nkeys; k++) {
		uint32_t cur = map[k].addr ? k : NOKEY;

		for (e = 0; e < CHAIN_UP && cur != NOKEY; e++) {
			cur = pred[cur];
			if (cur == NOKEY)
				break;
			ch[k * CHAIN_UP + e] = cur;
		}
		for (j = e; j < CHAIN_UP; j++)
			ch[k * CHAIN_UP + j] = NOKEY;
		hist[e] += !!map[k].addr;
	}
	f = fopen(out, "w");
	if (!f || fwrite(ch, sizeof(*ch), nkeys * CHAIN_UP, f) != nkeys * CHAIN_UP || fclose(f))
		die(out);
	printf("chain links=%lu items by ancestors (0..%d+):", links, CHAIN_UP);
	for (e = 0; e <= CHAIN_UP; e++)
		printf(" %lu", hist[e]);
	printf("\n");
}

static int do_map(int pid, unsigned long nkeys, const char *out, const char *chain_out)
{
	unsigned char *buf = malloc(SCAN_CARRY + SCAN_CHUNK);
	char path[64], line[512];
	struct addr *map = calloc(nkeys, sizeof(*map));
	uint64_t *hnext = calloc(nkeys, sizeof(*hnext));
	unsigned long cand = 0, bytes = 0;
	unsigned long found = 0, dup = 0;
	FILE *maps, *f;
	int mem;

	if (!hnext)
		die("calloc");

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
					memcpy(&hnext[k], buf + i - hdr + IT_HNEXT, 8);
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
	if (chain_out)
		write_chain(map, hnext, nkeys, chain_out);
	return 0;
}

int main(int argc, char **argv)
{
	struct worker *w;
	struct timespec t1;
	struct stat st;
	long done = 0, err = 0, gets = 0, miss = 0, hint_lost = 0, skip, i;
	int t, fd, *all;
	double dt;
	void *m;

	if ((argc == 5 || argc == 6) && !strcmp(argv[1], "map"))
		return do_map(atoi(argv[2]), strtoul(argv[3], NULL, 10), argv[4], argc == 6 ? argv[5] : NULL);
	if (argc != 7 && argc != 8 && argc != 11 && argc != 12) {
		fprintf(stderr, "usage: mctrace host port threads TRACE skip ops [qps [MAP FIFO lookahead [CHAIN]]]\n"
				"       mctrace map PID keys MAP [CHAIN]\n");
		return 2;
	}
	host = argv[1];
	port = atoi(argv[2]);
	threads = atoi(argv[3]);
	skip = atol(argv[5]);
	ops = atol(argv[6]);
	qps = argc >= 8 ? atof(argv[7]) : 0;
	if (argc >= 11) {
		FILE *f = fopen(argv[8], "r");

		if (!f || fseek(f, 0, SEEK_END))
			die(argv[8]);
		imap_keys = ftell(f) / sizeof(*imap);
		imap = malloc(imap_keys * sizeof(*imap));
		rewind(f);
		if (!imap || fread(imap, sizeof(*imap), imap_keys, f) != imap_keys)
			die(argv[8]);
		fclose(f);
		hint_fd = open(argv[9], O_WRONLY | O_NONBLOCK);
		if (hint_fd < 0)
			die(argv[9]);
		lookahead = atol(argv[10]);
		if (argc == 12) {
			size_t nc = imap_keys * CHAIN_UP;

			f = fopen(argv[11], "r");
			chain = malloc(nc * sizeof(*chain));
			if (!f || !chain || fread(chain, sizeof(*chain), nc, f) != nc)
				die(argv[11]);
			fclose(f);
		}
	}
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
		hint_lost += w[t].hint_lost;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	dt = t1.tv_sec - start.tv_sec + (t1.tv_nsec - start.tv_nsec) / 1e9;
	if (!done)
		return 1;
	qsort(all, done, sizeof(int), cmp_int);
	printf("steady ops=%ld seconds=%.3f rate=%.0f/s p50=%dus p99=%dus p999=%dus err=%ld get_pct=%ld pattern=trace hint_lost=%ld miss=%ld qps=%.0f\n",
	       done, dt, done / dt, all[done / 2], all[done * 99 / 100], all[done * 999 / 1000],
	       err, gets * 100 / done, hint_lost, miss, qps);
	return 0;
}
