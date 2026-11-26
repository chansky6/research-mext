// SPDX-License-Identifier: GPL-2.0
/*
 * mcloc: fixed-work memcached client whose key sequence has locality.
 * Keys and values are those of mcbench preload ("k%07u", `value` bytes).
 * One connection per thread, depth 1.
 *
 *   run:L        pick a key uniformly, then touch it and the L-1 keys after it
 *   session:S:L  S fixed lists of L random keys; pick a list uniformly and
 *                touch its keys in order
 *
 * Oracle hints: `mcloc map` finds every key's item in the server's memory
 * (run it while everything is in DRAM: the scan faults pages in).  A run given
 * that map writes, for the request each connection sends LOOKAHEAD requests
 * from now, the pages of that item to FIFO as raw u64 addresses (spd -P hint).
 * `map` also records the items assoc_find() walks before reaching the item
 * (the bucket head down to it, found by inverting h_next): memcached reads
 * the whole chain, so hinting all of it removes the faults the client never
 * asks for (report §5.14.2).
 * A SET moves the item.  With one connection the new place is the chunk the
 * previous SET freed (the slab free list is LIFO); with more, the item is
 * unknown from then on.
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
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PG		4096UL
#define KEYLEN		8
/* memcached 1.6 item header: nbytes at 32, it_flags at 38, nkey at 41, key at 48 or 56 (CAS) */
#define IT_NBYTES	32
#define IT_FLAGS	38
#define IT_NKEY		41
#define IT_HNEXT	16	/* struct _stritem: next, prev, h_next */
#define IT_LINKED	1
#define IT_CAS		2
#define CHAIN		8	/* map slots per key: the item then its hash-chain peers */
#define CHUNK		(4UL << 20)
#define CARRY		128UL

static const char *host;
static int port, get_pct, is_session, threads, hint_fd = -1;
static unsigned int keys, plen, nsess;
static long value_len, ops_each, lookahead;
static uint64_t seed0;
static char *value;
static uint64_t *map;	/* CHAIN item addresses per key: the item, then its hash-chain peers */

struct gen {
	uint64_t rs;
	unsigned int pos, base;
};

struct worker {
	pthread_t th;
	unsigned int id;
	long done, err, hinted, hint_lost;
	int *lat;
};

static void die(const char *m)
{
	perror(m);
	exit(1);
}

static uint64_t mix(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

/* The next request of a connection: its key, and whether it is a GET. */
static unsigned int gen_next(struct gen *g, int *get)
{
	unsigned int k;

	if (g->pos == plen) {
		g->rs = mix(g->rs);
		g->base = g->rs % (is_session ? nsess : keys);
		g->pos = 0;
	}
	/* a session's keys depend only on the seed, so every visit repeats them */
	k = is_session ? mix(seed0 ^ ((uint64_t)g->base << 20 | g->pos)) % keys : (g->base + g->pos) % keys;
	g->pos++;
	g->rs = mix(g->rs);
	*get = (int)(g->rs % 100) < get_pct;
	return k;
}

static int xfer(int fd, const char *req, size_t n, const char *tail)
{
	char resp[8192];
	size_t off = 0, tl = strlen(tail);

	while (off < n) {
		ssize_t w = send(fd, req + off, n - off, 0);

		if (w <= 0)
			return -1;
		off += w;
	}
	for (off = 0; off < sizeof(resp); ) {
		ssize_t r = recv(fd, resp + off, sizeof(resp) - off, 0);

		if (r <= 0)
			return -1;
		off += r;
		if (off >= tl && !memcmp(resp + off - tl, tail, tl))
			return memcmp(resp, tail, tl) ? 0 : 1;	/* 1: bare END, a miss */
	}
	return -1;
}

static void hint(struct worker *w, unsigned int k)
{
	uint64_t pages[2 * CHAIN], a, it;
	size_t n = 0, j;
	int e;

	for (e = 0; e < CHAIN; e++) {
		it = map[k * CHAIN + e];
		if (!it)
			continue;
		for (a = it & ~(PG - 1); a < it + 56 + KEYLEN + 8 + value_len + 2 && n < 2 * CHAIN; a += PG) {
			for (j = 0; j < n && pages[j] != a; j++)
				;
			if (j == n)
				pages[n++] = a;
		}
	}
	if (n && write(hint_fd, pages, n * sizeof(*pages)) < 0)
		w->hint_lost++;
}

static void *work(void *p)
{
	struct worker *w = p;
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	struct gen g = { .rs = mix(seed0 + w->id), .pos = plen }, ahead = g;
	char *req = malloc(value_len + 64);
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	uint64_t freed = 0;

	w->lat = malloc(ops_each * sizeof(int));
	if (fd < 0 || !req || !w->lat || inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
	    connect(fd, (struct sockaddr *)&sa, sizeof(sa)))
		die("connect");
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	while (w->done < ops_each) {
		struct timespec t0, t1;
		unsigned int k;
		int n, get, r;

		for (; map && w->hinted < ops_each && w->hinted <= w->done + lookahead; w->hinted++)
			hint(w, gen_next(&ahead, &r));
		k = gen_next(&g, &get);
		if (get) {
			n = sprintf(req, "get k%07u\r\n", k);
		} else {
			n = sprintf(req, "set k%07u 0 0 %ld\r\n", k, value_len);
			memcpy(req + n, value, value_len);
			memcpy(req + n + value_len, "\r\n", 2);
			n += value_len + 2;
		}
		clock_gettime(CLOCK_MONOTONIC, &t0);
		r = xfer(fd, req, n, get ? "END\r\n" : "STORED\r\n");
		clock_gettime(CLOCK_MONOTONIC, &t1);
		if (r < 0) {
			w->err++;
			break;
		}
		if (map && !get) {
			uint64_t old = map[k * CHAIN];

			map[k * CHAIN] = threads == 1 ? freed : 0;
			memset(map + k * CHAIN + 1, 0, (CHAIN - 1) * sizeof(*map));
			freed = old;
		}
		w->err += get && r;
		w->lat[w->done++] = (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000;
	}
	close(fd);
	free(req);
	return NULL;
}

static int cmp_int(const void *x, const void *y)
{
	return (*(const int *)x > *(const int *)y) - (*(const int *)x < *(const int *)y);
}

/* Is the "k%07u" at b[i] the key of a linked item of ours?  Returns its header length. */
static unsigned int item_at(const unsigned char *b, size_t i, unsigned int *k)
{
	unsigned int hdr, j;

	for (*k = 0, j = 1; j < KEYLEN; j++) {
		if (b[i + j] < '0' || b[i + j] > '9')
			return 0;
		*k = *k * 10 + b[i + j] - '0';
	}
	for (hdr = 48; hdr <= 56; hdr += 8) {
		const unsigned char *it = b + i - hdr;
		int32_t nbytes;
		uint16_t flags;

		memcpy(&nbytes, it + IT_NBYTES, sizeof(nbytes));
		memcpy(&flags, it + IT_FLAGS, sizeof(flags));
		if (it[IT_NKEY] == KEYLEN && nbytes == value_len + 2 && (flags & IT_LINKED) &&
		    !(flags & IT_CAS) == (hdr == 48))
			return hdr;
	}
	return 0;
}

/* h_next links (addr -> next) collected during the scan, for reverse lookup. */
struct link {
	uint64_t addr, prev;
};

static struct link *ltab;
static size_t ltab_sz;

static void ltab_put(uint64_t addr, uint64_t prev)
{
	size_t h = (addr >> 4) & (ltab_sz - 1);

	while (ltab[h].addr)
		h = (h + 1) & (ltab_sz - 1);
	ltab[h].addr = addr;
	ltab[h].prev = prev;
}

/* The item that h_next points at addr, 0 if none. */
static uint64_t ltab_prev(uint64_t addr)
{
	size_t h = (addr >> 4) & (ltab_sz - 1);

	while (ltab[h].addr) {
		if (ltab[h].addr == addr)
			return ltab[h].prev;
		h = (h + 1) & (ltab_sz - 1);
	}
	return 0;
}

/* peers[0] is the item; append the items assoc_find() walks through before it,
 * i.e. the rest of its bucket from the bucket head down to the item. */
static int chain_fill(int mem, uint64_t *peers)
{
	uint64_t cur = peers[0];
	int n = 1;

	while (n < CHAIN) {
		uint64_t prev = ltab_prev(cur);

		if (!prev)
			break;
		peers[n++] = prev;
		cur = prev;
	}
	return n;
}

static int do_map(int pid, const char *out)
{
	unsigned char *buf = malloc(CARRY + CHUNK);
	char path[64], line[512];
	unsigned long found = 0, dup = 0, chain_len = 0;
	unsigned int k;
	FILE *maps, *f;
	int mem;

	map = calloc((size_t)keys * CHAIN, sizeof(*map));
	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	maps = fopen(path, "r");
	snprintf(path, sizeof(path), "/proc/%d/mem", pid);
	mem = open(path, O_RDONLY);
	if (!buf || !map || !maps || mem < 0)
		die("map");
	while (fgets(line, sizeof(line), maps)) {
		unsigned long s, e, ino, a;
		char perm[8], name[256] = "";
		size_t have = 0;	/* bytes of buf that end at address a */

		if (sscanf(line, "%lx-%lx %7s %*x %*s %lu %255s", &s, &e, perm, &ino, name) < 4)
			continue;
		if (strcmp(perm, "rw-p") || ino || (name[0] && strcmp(name, "[heap]")))
			continue;
		for (a = s; a < e; ) {
			size_t want = e - a < CHUNK ? e - a : CHUNK, i;
			ssize_t r = pread(mem, buf + have, want, a);

			if (r <= 0) {	/* unreadable: skip a page, items do not span it */
				a += PG;
				have = 0;
				continue;
			}
			a += r;
			have += r;
			for (i = 56; i + KEYLEN <= have; i++) {
				unsigned int k, hdr;

				if (buf[i] != 'k' || !(hdr = item_at(buf, i, &k)) || k >= keys)
					continue;
				dup += map[k * CHAIN] && map[k * CHAIN] != a - have + i - hdr;
				found += !map[k * CHAIN];
				map[k * CHAIN] = a - have + i - hdr;
			}
			if (have > CARRY) {
				memmove(buf, buf + have - CARRY, CARRY);
				have = CARRY;
			}
		}
	}
	/* Second pass: h_next of every item, inverted so each item can find its
	 * predecessors; assoc_find() walks those before reaching the item. */
	ltab_sz = 1;
	while (ltab_sz < (size_t)keys * 2)
		ltab_sz <<= 1;
	ltab = calloc(ltab_sz, sizeof(*ltab));
	if (!ltab)
		die("calloc");
	for (k = 0; k < keys; k++) {
		uint64_t next;

		if (!map[k * CHAIN])
			continue;
		if (pread(mem, &next, sizeof(next), map[k * CHAIN] + IT_HNEXT) != sizeof(next))
			continue;
		if (next)
			ltab_put(next, map[k * CHAIN]);
	}
	for (k = 0; k < keys; k++) {
		uint64_t peers[CHAIN];
		int np, j;

		if (!map[k * CHAIN])
			continue;
		peers[0] = map[k * CHAIN];
		np = chain_fill(mem, peers);
		chain_len += np;
		for (j = 1; j < np; j++)
			map[k * CHAIN + j] = peers[j];
	}
	f = fopen(out, "w");
	if (!f || fwrite(map, sizeof(*map), (size_t)keys * CHAIN, f) != (size_t)keys * CHAIN || fclose(f))
		die(out);
	printf("map keys=%u found=%lu dup=%lu chain_avg=%.2f\n", keys, found, dup,
	       found ? (double)chain_len / found : 0.0);
	return found == keys && !dup ? 0 : 1;
}

int main(int argc, char **argv)
{
	struct timespec t0, t1;
	struct worker *w;
	long done = 0, err = 0, hint_lost = 0, i;
	int t, *all;
	double dt;

	if (argc == 6 && !strcmp(argv[1], "map")) {
		keys = strtoul(argv[3], NULL, 10);
		value_len = atol(argv[4]);
		return do_map(atoi(argv[2]), argv[5]);
	}
	if ((argc != 10 && argc != 13) ||
	    (sscanf(argv[9], "session:%u:%u", &nsess, &plen) != 2 && sscanf(argv[9], "run:%u", &plen) != 1) ||
	    !plen) {
		fprintf(stderr,
			"usage: mcloc host port threads keys value ops get_pct seed run:L|session:S:L [MAP FIFO LOOKAHEAD]\n"
			"       mcloc map PID keys value MAP\n");
		return 2;
	}
	is_session = nsess > 0;
	host = argv[1];
	port = atoi(argv[2]);
	threads = atoi(argv[3]);
	keys = strtoul(argv[4], NULL, 10);
	value_len = atol(argv[5]);
	ops_each = atol(argv[6]) / threads;
	get_pct = atoi(argv[7]);
	seed0 = strtoull(argv[8], NULL, 10);
	value = malloc(value_len);
	w = calloc(threads, sizeof(*w));
	all = malloc(ops_each * threads * sizeof(int));
	if (!value || !w || !all)
		die("malloc");
	if (argc == 13) {
		FILE *f = fopen(argv[10], "r");

		map = malloc((size_t)keys * CHAIN * sizeof(*map));
		if (!f || !map || fread(map, sizeof(*map), (size_t)keys * CHAIN, f) != (size_t)keys * CHAIN)
			die(argv[10]);
		fclose(f);
		hint_fd = open(argv[11], O_WRONLY | O_NONBLOCK);
		if (hint_fd < 0)
			die(argv[11]);
		lookahead = atol(argv[12]);
	}
	for (i = 0; i < value_len; i++)
		value[i] = 'a' + i % 26;
	clock_gettime(CLOCK_MONOTONIC, &t0);
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
		hint_lost += w[t].hint_lost;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	dt = t1.tv_sec - t0.tv_sec + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	qsort(all, done, sizeof(int), cmp_int);
	if (!done)
		return 1;
	printf("steady ops=%ld seconds=%.3f rate=%.0f/s p50=%dus p99=%dus p999=%dus err=%ld get_pct=%d pattern=%s hint_lost=%ld\n",
	       done, dt, done / dt, all[done / 2], all[done * 99 / 100], all[done * 999 / 1000],
	       err, get_pct, argv[9], hint_lost);
	return 0;
}
