// SPDX-License-Identifier: GPL-2.0
// mcbench: fixed-work memcached client. Preload fills the keyspace; steady runs
// an exact GET/SET mix over a deterministic Zipf distribution and reports
// per-op latency percentiles. One connection per thread, depth 1.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static uint64_t rnd(uint64_t *s)
{
    uint64_t x = *s + 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return *s = x ^ (x >> 31);
}

static void die(const char *m)
{
    perror(m);
    exit(1);
}

static int connect_to(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) die("inet_pton");
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) die("connect");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int send_all(int fd, const char *b, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, b + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

// Reads until the terminator line appears; returns bytes consumed or -1.
static ssize_t read_until(int fd, char *buf, size_t cap, const char *tail)
{
    size_t n = 0, tl = strlen(tail);
    while (n < cap) {
        ssize_t r = recv(fd, buf + n, cap - n, 0);
        if (r <= 0) return -1;
        n += (size_t)r;
        if (n >= tl && memcmp(buf + n - tl, tail, tl) == 0) return (ssize_t)n;
    }
    return -1;
}

static void key_of(char *out, unsigned k)
{
    sprintf(out, "k%07u", k);
}

static long value_len = 2000;
static char *value;

static void build_value(void)
{
    value = malloc((size_t)value_len);
    if (!value) die("malloc value");
    for (long i = 0; i < value_len; i++) value[i] = (char)('a' + i % 26);
}

static char carry[8];
static size_t carry_n;

// Reads one chunk of pipelined replies, counts STORED, keeps a split trailer.
static long pump_stored(int fd, char *buf, size_t cap)
{
    memcpy(buf, carry, carry_n);
    ssize_t r = recv(fd, buf + carry_n, cap - carry_n, 0);
    if (r <= 0) die("preload recv");
    size_t n = carry_n + (size_t)r, i = 0;
    long ok = 0;
    while (i + 8 <= n) {
        if (!memcmp(buf + i, "STORED\r\n", 8)) { ok++; i += 8; continue; }
        if (!memcmp(buf + i, "ERROR", 5) || !memcmp(buf + i, "CLIENT_ERROR", 12) ||
            !memcmp(buf + i, "SERVER_ERROR", 12)) {
            fprintf(stderr, "mcbench: server refused SET: %.*s\n", 32, buf + i);
            exit(1);
        }
        i++;
    }
    carry_n = n - i;
    if (carry_n > sizeof carry) carry_n = 0; // never a partial reply
    memcpy(carry, buf + i, carry_n);
    return ok;
}

// Preload: pipelined SETs, no latency capture.
static void preload(const char *host, int port, unsigned keys)
{
    int fd = connect_to(host, port);
    size_t cap = 1 << 20;
    char *req = malloc(cap), *resp = malloc(cap);
    if (!req || !resp) die("malloc");
    double t0 = now();
    unsigned k = 0;
    while (k < keys) {
        size_t o = 0;
        unsigned batch = 0;
        while (k < keys && batch < 512 && o + 64 + (size_t)value_len < cap) {
            char key[16];
            key_of(key, k++);
            o += (size_t)sprintf(req + o, "set %s 0 0 %ld\r\n", key, value_len);
            memcpy(req + o, value, (size_t)value_len);
            o += (size_t)value_len;
            memcpy(req + o, "\r\n", 2);
            o += 2;
            batch++;
        }
        if (send_all(fd, req, o) < 0) die("send preload");
        carry_n = 0;
        long stored = 0;
        while (stored < (long)batch) stored += pump_stored(fd, resp, cap);
    }
    double dt = now() - t0;
    printf("preload keys=%u value=%ld seconds=%.2f rate=%.0f/s\n", keys, value_len, dt, keys / dt);
    close(fd);
    free(req);
    free(resp);
}

// Zipf CDF over `keys` ranks, exponent s.
static double *cdf;
static unsigned keyspace;

static void build_cdf(unsigned keys, double s)
{
    cdf = malloc(sizeof(double) * keys);
    if (!cdf) die("malloc cdf");
    double acc = 0;
    for (unsigned i = 0; i < keys; i++) {
        acc += 1.0 / pow((double)(i + 1), s);
        cdf[i] = acc;
    }
    for (unsigned i = 0; i < keys; i++) cdf[i] /= acc;
    keyspace = keys;
}

static unsigned zipf(uint64_t *seed)
{
    double u = (double)(rnd(seed) >> 11) / 9007199254740992.0; // [0,1)
    unsigned lo = 0, hi = keyspace;
    while (lo + 1 < hi) {
        unsigned mid = (lo + hi) / 2;
        if (cdf[mid] < u) lo = mid; else hi = mid;
    }
    return lo;
}

struct arg {
    const char *host;
    int port;
    unsigned thread;
    long ops;
    int get_pct;
    uint64_t seed;
    double p50, p99, p999;
    long err;
    int *lat;
};

static void *worker(void *p)
{
    struct arg *a = p;
    char key[16], req[64], resp[8192];
    int fd = connect_to(a->host, a->port);
    uint64_t seed = a->seed + a->thread * 0x9e3779b97f4a7c15ULL;
    long n = 0;
    a->lat = malloc(sizeof(int) * (size_t)a->ops);
    if (!a->lat) die("malloc lat");
    while (n < a->ops) {
        unsigned k = zipf(&seed);
        key_of(key, k);
        int is_get = (int)(rnd(&seed) % 100) < a->get_pct;
        int len;
        if (is_get) len = sprintf(req, "get %s\r\n", key);
        else len = sprintf(req, "set %s 0 0 %ld\r\n%s\r\n", key, value_len, value);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (send_all(fd, req, (size_t)len) < 0) { a->err++; break; }
        ssize_t r = read_until(fd, resp, sizeof resp, is_get ? "END\r\n" : "STORED\r\n");
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (r < 0) { a->err++; break; }
        if (is_get && memcmp(resp, "VALUE ", 6) != 0) a->err++;
        a->lat[n++] = (int)((t1.tv_sec - t0.tv_sec) * 1000000 +
                            (t1.tv_nsec - t0.tv_nsec) / 1000);
    }
    a->ops = n;
    close(fd);
    return NULL;
}

static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

static double pct(const struct arg *a, int p)
{
    return a->lat[(long)((double)(a->ops - 1) * p / 100.0)];
}

static void steady(const char *host, int port, int threads, long ops, int get_pct, uint64_t seed)
{
    pthread_t th[threads];
    struct arg a[threads];
    double t0 = now();
    for (int i = 0; i < threads; i++) {
        a[i] = (struct arg){.host = host, .port = port, .thread = (unsigned)i,
                            .ops = ops / threads, .get_pct = get_pct, .seed = seed};
        if (pthread_create(&th[i], NULL, worker, &a[i])) die("pthread_create");
    }
    long done = 0, err = 0;
    int *all = malloc(sizeof(int) * (size_t)ops);
    long at = 0;
    if (!all) die("malloc all");
    for (int i = 0; i < threads; i++) {
        pthread_join(th[i], NULL);
        done += a[i].ops;
        err += a[i].err;
        memcpy(all + at, a[i].lat, sizeof(int) * (size_t)a[i].ops);
        at += a[i].ops;
        free(a[i].lat);
    }
    double dt = now() - t0;
    qsort(all, (size_t)done, sizeof(int), cmp_int);
    printf("steady ops=%ld seconds=%.3f rate=%.0f/s p50=%dus p99=%dus p999=%dus err=%ld get_pct=%d\n",
           done, dt, done / dt,
           done ? all[(long)(0.50 * (double)(done - 1))] : 0,
           done ? all[(long)(0.99 * (double)(done - 1))] : 0,
           done ? all[(long)(0.999 * (double)(done - 1))] : 0,
           err, get_pct);
    free(all);
    (void)pct;
}

int main(int argc, char **argv)
{
    if (argc < 10) {
        fprintf(stderr, "usage: mcbench preload|steady host port threads keys value ops zipf get_pct [seed]\n");
        return 2;
    }
    const char *mode = argv[1], *host = argv[2];
    int port = atoi(argv[3]), threads = atoi(argv[4]);
    unsigned keys = (unsigned)strtoul(argv[5], NULL, 10);
    value_len = atol(argv[6]);
    long ops = atol(argv[7]);
    double s = atof(argv[8]);
    int get_pct = atoi(argv[9]);
    uint64_t seed = argc > 10 ? strtoull(argv[10], NULL, 10) : 42;
    build_value();
    if (!strcmp(mode, "preload")) preload(host, port, keys);
    else if (!strcmp(mode, "steady")) {
        build_cdf(keys, s);
        steady(host, port, threads, ops, get_pct, seed);
    } else return 2;
    return 0;
}
