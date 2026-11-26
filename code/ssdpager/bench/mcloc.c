// SPDX-License-Identifier: GPL-2.0
/*
 * mcloc: fixed-work memcached client whose key sequence has locality.
 * Keys and values are those of mcbench preload ("k%07u", `value` bytes).
 * One connection per thread, depth 1.
 *
 *   run:L        pick a key uniformly, then touch it and the L-1 keys after it
 *   session:S:L  S fixed lists of L random keys; pick a list uniformly and
 *                touch its keys in order
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
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

static const char *host;
static int port, get_pct, is_session;
static unsigned int keys, plen, nsess;
static long value_len, ops_each;
static uint64_t seed0;
static char *value;

struct worker {
	pthread_t th;
	unsigned int id;
	long done, err;
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

static void *work(void *p)
{
	struct worker *w = p;
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	uint64_t rs = mix(seed0 + w->id);
	char *req = malloc(value_len + 64);
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	unsigned int pos = plen, base = 0;

	w->lat = malloc(ops_each * sizeof(int));
	if (fd < 0 || !req || !w->lat || inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
	    connect(fd, (struct sockaddr *)&sa, sizeof(sa)))
		die("connect");
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	while (w->done < ops_each) {
		struct timespec t0, t1;
		unsigned int k;
		int n, get, r;

		if (pos == plen) {
			rs = mix(rs);
			base = rs % (is_session ? nsess : keys);
			pos = 0;
		}
		/* a session's keys depend only on the seed, so every visit repeats them */
		k = is_session ? mix(seed0 ^ ((uint64_t)base << 20 | pos)) % keys : (base + pos) % keys;
		pos++;
		rs = mix(rs);
		get = (int)(rs % 100) < get_pct;
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

int main(int argc, char **argv)
{
	struct timespec t0, t1;
	struct worker *w;
	long done = 0, err = 0, i;
	int threads, t, *all;
	double dt;

	if (argc != 10 ||
	    (sscanf(argv[9], "session:%u:%u", &nsess, &plen) != 2 && sscanf(argv[9], "run:%u", &plen) != 1) ||
	    !plen) {
		fprintf(stderr, "usage: mcloc host port threads keys value ops get_pct seed run:L|session:S:L\n");
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
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	dt = t1.tv_sec - t0.tv_sec + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	qsort(all, done, sizeof(int), cmp_int);
	if (!done)
		return 1;
	printf("steady ops=%ld seconds=%.3f rate=%.0f/s p50=%dus p99=%dus p999=%dus err=%ld get_pct=%d pattern=%s\n",
	       done, dt, done / dt, all[done / 2], all[done * 99 / 100], all[done * 999 / 1000],
	       err, get_pct, argv[9]);
	return 0;
}
