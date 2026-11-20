// SPDX-License-Identifier: GPL-2.0
// synth:predswap 合成微基准负载。
// 模式(第一参数):
//   zipf <ws_mb> <dur_s> <seed>          稳态偏斜随机(无节流,紧密循环)
//   seq   <ws_mb> <dur_s>                顺序反复扫描
//   chain <ws_mb> <dur_s> <seed>         指针追逐(随机链,反复走;链断即 SIGSEGV,
//                                        G6 数据完整性测试用它)
//   block <ws_mb> <seed>                 成块冷热:全量触碰 → sleep → 重触碰
//   touch <ws_mb>                        只触碰一遍,立即退出
//   tiers <ws_mb> <dur_s> <seed> <ops_per_s>  三层访问(B1/B2 专用):
//         hot = 前 2% 页(70% ops)、warm = 接着 18% 页(25% ops)、
//         cold = 其余 80% 页(5% ops);按 ops_per_s 节流,让冷页真正空闲。
// 输出:各阶段 majflt/minflt/耗时;地址打印到 stderr 供 BPF 事件流核对。
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/resource.h>

#define PAGE 4096UL

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void flt(struct rusage *a, struct rusage *b, double dt, const char *tag)
{
    printf("%-14s majflt=%ld minflt=%ld time=%.3fs\n", tag,
           b->ru_majflt - a->ru_majflt, b->ru_minflt - a->ru_minflt, dt);
    fflush(stdout);
}

static uint64_t rnd64(uint64_t *s)
{
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    /* splitmix64 终结器:xorshift 的低位有短周期,% 2^k 会泄漏(2026-11 实测) */
    uint64_t x = *s + 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: synth <mode> <ws_mb> <dur_s> [seed] [ops_per_s]\n"
                "  modes: uniform zipf seq chain block touch tiers cb\n");
        return 2;
    }
    const char *mode = argv[1];
    size_t ws = (size_t)atol(argv[2]) << 20;
    double dur = atof(argv[3]);
    uint64_t seed = argc > 4 ? strtoull(argv[4], NULL, 10) : 42;

    size_t np = ws / PAGE;
    uint8_t *p = mmap(NULL, ws, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    fprintf(stderr, "synth: mode=%s ws=%zuMB pages=%zu base=%p pid=%d\n",
            mode, ws >> 20, np, (void *)p, getpid());

    struct rusage a, b;
    double t0;

    if (!strcmp(mode, "touch") || !strcmp(mode, "block")) {
        getrusage(RUSAGE_SELF, &a); t0 = now();
        for (size_t i = 0; i < np; i++) p[i * PAGE] = (uint8_t)i;
        getrusage(RUSAGE_SELF, &b);
        flt(&a, &b, now() - t0, "warm");
        if (!strcmp(mode, "touch")) return 0;
        long sleep_s = argc > 5 ? atol(argv[5]) : 5;
        printf("sleep %lds ...\n", sleep_s); fflush(stdout);
        sleep(sleep_s);
        getrusage(RUSAGE_SELF, &a); t0 = now();
        for (size_t i = 0; i < np; i++) p[i * PAGE] = (uint8_t)i;
        getrusage(RUSAGE_SELF, &b);
        flt(&a, &b, now() - t0, "retouch");
        return 0;
    }

    if (!strcmp(mode, "chain")) {
        size_t stride = PAGE / sizeof(size_t);
        long ops_per_sec = argc > 5 ? atol(argv[5]) : 0; /* 0 = 不节流 */
        /* 链数据幂等可校验:每页第 0 个字存后继索引 */
        for (size_t i = 0; i < np; i++)
            ((size_t *)p)[i * stride] = (rnd64(&seed) % np) * stride;
        size_t cur = (rnd64(&seed) % np) * stride;
        size_t ops = 0;
        struct timespec ts;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        double next_t = t0;
        while (now() - t0 < dur) {
            for (int i = 0; i < 64; i++) {
                cur = ((size_t *)p)[cur];
                ops++;
            }
            if (ops_per_sec > 0) {
                next_t += 64.0 / (double)ops_per_sec;
                double late = next_t - now();
                if (late > 0) {
                    ts.tv_sec = (time_t)late;
                    ts.tv_nsec = (long)((late - ts.tv_sec) * 1e9);
                    nanosleep(&ts, NULL);
                } else {
                    next_t = now();
                }
            }
        }
        getrusage(RUSAGE_SELF, &b);
        /* 校验:链值恒等于建立时的后继(幂等,不随遍历次数改变) */
        long bad = 0;
        for (size_t i = 0; i < np; i++)
            if (((size_t *)p)[i * stride] / stride >= np)
                bad++;
        printf("chain ops=%zu end=%zu verify_bad=%ld\n", ops, cur / stride, bad);
        flt(&a, &b, now() - t0, "steady");
        return bad ? 3 : 0;
    }

    if (!strcmp(mode, "cb")) {
        /* 冷块模式(P2 余量拓扑验证):
           hot = 前 2560 页(10M),warm = 接着 71680 页(280M),
           cold = 其余(按 256 页块轮转,块内顺序扫,块间隔 > 冷却/判冷阈值)。
           ops 分配:hot 25% / warm 65% / cold 10%;ops_per_s 节流。 */
        long ops_per_sec = argc > 5 ? atol(argv[5]) : 20000;
        /* Optional fixed operation count: no pacing and identical work per paired run. */
        size_t fixed_ops = argc > 8 ? strtoull(argv[8], NULL, 10) : 0;
        if (fixed_ops % 64 || ops_per_sec <= 0) return 2;
        size_t blk = 256;
        /* hot_n/warm_n 可调:cb <ws> <dur> <seed> <ops> [hot_pages] [warm_pages] */
        long hot_arg = argc > 6 ? atol(argv[6]) : 2560;
        long warm_arg = argc > 7 ? atol(argv[7]) : 71680;
        size_t hot_n = (size_t)hot_arg < np / 4 ? (size_t)hot_arg : np / 4;
        size_t warm_n = (size_t)warm_arg < np * 3 / 4 ? (size_t)warm_arg : np * 3 / 4 - hot_n;
        size_t cold_n = np - hot_n - warm_n;
        if (cold_n < blk * 2) { fprintf(stderr, "cb: ws too small\n"); return 2; }
        size_t nblk = cold_n / blk;
        size_t cur_blk = 0, cur_off = 0;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        for (size_t i = 0; i < np; i++) p[i * PAGE] = (uint8_t)i; /* 幂等,可校验 */
        getrusage(RUSAGE_SELF, &b);
        flt(&a, &b, now() - t0, "warm");
        size_t ops = 0;
        double batch_sec = 64.0 / (double)ops_per_sec;
        uint64_t s2 = seed ^ 0x9e3779b9ULL;
        struct timespec ts;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        double next_t = t0;
        while (fixed_ops ? ops < fixed_ops : now() - t0 < dur) {
            for (int i = 0; i < 64; i++) {
                uint64_t u = rnd64(&s2) % 1000;
                size_t pg;
                if (u < 250)
                pg = rnd64(&seed) % hot_n;
            else if (u < 950)
                pg = hot_n + rnd64(&seed) % warm_n;
                else {
                    pg = hot_n + warm_n + cur_blk * blk + cur_off;
                    cur_off++;
                    if (cur_off >= blk) {
                        cur_off = 0;
                        cur_blk = (cur_blk + 1) % nblk;
                    }
                }
                p[pg * PAGE] = (uint8_t)pg;
                ops++;
            }
            if (fixed_ops) continue;
            next_t += batch_sec;
            double late = next_t - now();
            if (late > 0) {
                ts.tv_sec = (time_t)late;
                ts.tv_nsec = (long)((late - ts.tv_sec) * 1e9);
                nanosleep(&ts, NULL);
            } else {
                next_t = now();
            }
        }
        getrusage(RUSAGE_SELF, &b);
        printf("cb ops=%zu rate=%.0f/s blocks=%zu cycle=%.1fs\n",
               ops, (double)ops / (now() - t0), nblk,
               (double)nblk * blk * 10.0 / (double)ops_per_sec); /* 冷占比 10% */
        flt(&a, &b, now() - t0, "steady");
        long bad = 0;
        for (size_t i = 0; i < np; i++)
            if (p[i * PAGE] != (uint8_t)i)
                bad++;
        printf("cb verify bad=%ld\n", bad);
        return bad ? 3 : 0;
    }

    if (!strcmp(mode, "tiers")) {
        long ops_per_sec = argc > 5 ? atol(argv[5]) : 20000;
        size_t hot_n = np / 50, warm_n = np * 9 / 50;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        for (size_t i = 0; i < np; i++) p[i * PAGE] = (uint8_t)i; /* 幂等模式,可校验 */
        getrusage(RUSAGE_SELF, &b);
        flt(&a, &b, now() - t0, "warm");
        size_t ops = 0;
        double batch_sec = 64.0 / (double)ops_per_sec;
        uint64_t s2 = seed ^ 0x9e3779b9ULL;
        struct timespec ts;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        double next_t = t0;
        while (now() - t0 < dur) {
            for (int i = 0; i < 64; i++) {
                uint64_t u = rnd64(&s2) % 1000;
                size_t pg;
                if (u < 700)
                    pg = rnd64(&seed) % (hot_n ? hot_n : 1);
                else if (u < 950)
                    pg = hot_n + rnd64(&seed) % warm_n;
                else
                    pg = hot_n + warm_n + rnd64(&seed) % (np - hot_n - warm_n);
                p[pg * PAGE] = (uint8_t)pg;
                ops++;
            }
            next_t += batch_sec;
            double late = next_t - now();
            if (late > 0.0) {
                ts.tv_sec = (time_t)late;
                ts.tv_nsec = (long)((late - ts.tv_sec) * 1e9);
                nanosleep(&ts, NULL);
            } else {
                next_t = now();
            }
        }
        getrusage(RUSAGE_SELF, &b);
        printf("tiers ops=%zu rate=%.0f/s\n", ops, (double)ops / (now() - t0));
        flt(&a, &b, now() - t0, "steady");
        long bad = 0;
        for (size_t i = 0; i < np; i++)
            if (p[i * PAGE] != (uint8_t)i)
                bad++;
        printf("tiers verify bad=%ld\n", bad);
        return bad ? 3 : 0;
    }

    int skew = !strcmp(mode, "zipf");
    if (skew || !strcmp(mode, "uniform")) {
        getrusage(RUSAGE_SELF, &a); t0 = now();
        for (size_t i = 0; i < np; i++) p[i * PAGE] = 1;
        getrusage(RUSAGE_SELF, &b);
        flt(&a, &b, now() - t0, "warm");
        size_t ops = 0;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        while (now() - t0 < dur) {
            for (int i = 0; i < 2048; i++) {
                uint64_t u = rnd64(&seed) % np;
                if (skew && (rnd64(&seed) & 3) != 0)
                    u = u % (np / 10 + 1);
                p[u * PAGE] = (uint8_t)u;
                ops++;
            }
        }
        getrusage(RUSAGE_SELF, &b);
        printf("%s ops=%zu\n", mode, ops);
        flt(&a, &b, now() - t0, "steady");
        return 0;
    }

    if (!strcmp(mode, "seq")) {
        long ops_per_sec = argc > 4 ? atol(argv[4]) : 0; /* 0 = 不节流 */
        getrusage(RUSAGE_SELF, &a); t0 = now();
        for (size_t i = 0; i < np; i++) p[i * PAGE] = (uint8_t)i;
        getrusage(RUSAGE_SELF, &b);
        flt(&a, &b, now() - t0, "warm");
        size_t sweeps = 0, ops = 0;
        struct timespec ts;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        double next_t = t0;
        size_t i = 0;
        while (now() - t0 < dur) {
            for (int k = 0; k < 64; k++) {
                p[i * PAGE] = (uint8_t)i;
                i = (i + 1) % np;
                ops++;
            }
            sweeps = ops / np;
            if (ops_per_sec > 0) {
                next_t += 64.0 / (double)ops_per_sec;
                double late = next_t - now();
                if (late > 0) {
                    ts.tv_sec = (time_t)late;
                    ts.tv_nsec = (long)((late - ts.tv_sec) * 1e9);
                    nanosleep(&ts, NULL);
                } else {
                    next_t = now();
                }
            }
        }
        getrusage(RUSAGE_SELF, &b);
        long bad = 0;
        for (size_t j = 0; j < np; j++)
            if (p[j * PAGE] != (uint8_t)j)
                bad++;
        printf("seq sweeps=%zu ops=%zu verify_bad=%ld\n", sweeps, ops, bad);
        flt(&a, &b, now() - t0, "steady");
        return 0;
    }

    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
