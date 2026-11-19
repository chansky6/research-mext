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
    return *s;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: synth <mode> <ws_mb> <dur_s> [seed] [ops_per_s]\n"
                "  modes: zipf seq chain block touch tiers\n");
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
        printf("sleep 5s ...\n"); fflush(stdout);
        sleep(5);
        getrusage(RUSAGE_SELF, &a); t0 = now();
        for (size_t i = 0; i < np; i++) p[i * PAGE] = (uint8_t)i;
        getrusage(RUSAGE_SELF, &b);
        flt(&a, &b, now() - t0, "retouch");
        return 0;
    }

    if (!strcmp(mode, "chain")) {
        size_t stride = PAGE / sizeof(size_t);
        for (size_t i = 0; i < np; i++)
            ((size_t *)p)[i * stride] = (rnd64(&seed) % np) * stride;
        size_t cur = (rnd64(&seed) % np) * stride;
        size_t ops = 0;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        while (now() - t0 < dur) {
            for (int i = 0; i < 4096; i++) {
                cur = ((size_t *)p)[cur];
                ops++;
            }
        }
        getrusage(RUSAGE_SELF, &b);
        printf("chain ops=%zu end=%zu\n", ops, cur / stride);
        flt(&a, &b, now() - t0, "steady");
        return 0;
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

    if (!strcmp(mode, "zipf")) {
        getrusage(RUSAGE_SELF, &a); t0 = now();
        for (size_t i = 0; i < np; i++) p[i * PAGE] = 1;
        getrusage(RUSAGE_SELF, &b);
        flt(&a, &b, now() - t0, "warm");
        size_t ops = 0;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        while (now() - t0 < dur) {
            for (int i = 0; i < 2048; i++) {
                uint64_t u = rnd64(&seed) % np;
                if ((rnd64(&seed) & 3) != 0)
                    u = u % (np / 10 + 1);
                p[u * PAGE] = (uint8_t)u;
                ops++;
            }
        }
        getrusage(RUSAGE_SELF, &b);
        printf("zipf ops=%zu\n", ops);
        flt(&a, &b, now() - t0, "steady");
        return 0;
    }

    if (!strcmp(mode, "seq")) {
        getrusage(RUSAGE_SELF, &a); t0 = now();
        for (size_t i = 0; i < np; i++) p[i * PAGE] = 1;
        getrusage(RUSAGE_SELF, &b);
        flt(&a, &b, now() - t0, "warm");
        size_t sweeps = 0, ops = 0;
        getrusage(RUSAGE_SELF, &a); t0 = now();
        while (now() - t0 < dur) {
            for (size_t i = 0; i < np; i++) { p[i * PAGE] = (uint8_t)i; ops++; }
            sweeps++;
        }
        getrusage(RUSAGE_SELF, &b);
        printf("seq sweeps=%zu ops=%zu\n", sweeps, ops);
        flt(&a, &b, now() - t0, "steady");
        return 0;
    }

    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
