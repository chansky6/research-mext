/* SPDX-License-Identifier: GPL-2.0 */
/* xtierd.c — main: epoch loop, modes (monitor/collect/evaluate/run). */
#include "xtierd.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

struct xt_cfg G = {
    .mode = 0, .epoch_ms = 250, .budget_pages = 0, .topk_admit_fraction = 0.15,
    .cooldown_epochs = COOLDOWN_EPOCHS, .thresh_dyn = 0, .out_zpC_cache = 0,
    .mig_budget = 2048, .state_bits = 22, .spe_interval = 4096, .spe_stores = 0,
    .cold_scan_chunk = 50000,
};

static volatile int stop_flag;
static void sig_handler(int sig) { (void)sig; stop_flag = 1; }

static void env_i(const char *name, int *out) {
    const char *e = getenv(name);
    if (e && *e) { int v = atoi(e); if (v > 0) *out = v; }
}

int main(int argc, char **argv) {
    if (argc < 5) {
        printf("Usage: %s <pid> <spe_interval> <epoch_ms> <monitor|collect|evaluate|run>\n", argv[0]);
        return 1;
    }
    G.pid = (pid_t)atoi(argv[1]);
    if (G.pid <= 0) { fprintf(stderr, "bad pid\n"); return 1; }
    G.spe_interval = atoi(argv[2]);
    if (G.spe_interval < 1024) G.spe_interval = 1024;   // PMU min_interval
    G.epoch_ms = (uint32_t)atoi(argv[3]);
    if (G.epoch_ms < 10) G.epoch_ms = 10;
    const char *m = argv[4];
    if (!strcmp(m, "monitor")) G.mode = 0;
    else if (!strcmp(m, "collect")) G.mode = 1;
    else if (!strcmp(m, "evaluate")) G.mode = 2;
    else if (!strcmp(m, "run")) G.mode = 3;
    else { fprintf(stderr, "unknown mode %s\n", m); return 1; }

    env_i("XTIER_SPE_INTERVAL", &G.spe_interval);
    if (G.spe_interval < 1024) G.spe_interval = 1024;
    G.spe_stores = getenv("XTIER_SPE_STORES") && atoi(getenv("XTIER_SPE_STORES"));
    env_i("XTIER_STATE_BITS", (int *)&G.state_bits);
    env_i("XTIER_MIG_BUDGET", (int *)&G.mig_budget);
    env_i("XTIER_COLD_SCAN_CHUNK", (int *)&G.cold_scan_chunk);
    { const char *e = getenv("XTIER_TOPK_FRACTION");
      if (e && *e) { double v = atof(e); if (v > 0 && v < 1) G.topk_admit_fraction = v; } }
    { const char *e = getenv("XTIER_COOLDOWN_EPOCHS");
      if (e && *e) { int v = atoi(e); if (v >= 0) G.cooldown_epochs = (uint32_t)v; } }
    { const char *e = getenv("XTIER_STORM_PSWPIN");
      if (e && *e) { int v = atoi(e); if (v > 0) G.storm_pswpin = (uint32_t)v; } }
    { const char *sc = getenv("XTIER_SCORER");
      if (sc && *sc) {
          static const char *names[] = { "mlp", "freq", "recency", "ewma", "random" };
          int found = -1;
          for (int i = 0; i < 5; i++) if (!strcmp(sc, names[i])) { found = i; break; }
          if (found < 0) { fprintf(stderr, "[!] XTIER_SCORER='%s' unrecognized\n", sc); return 1; }
          G.scorer_id = (uint32_t)found;
      } }
    { const char *e = getenv("XTIER_DRAM_BUDGET_MB");
      if (e && *e) { uint64_t mb = strtoull(e, NULL, 10);
          G.budget_pages = mb * (1024 * 1024) / 4096; } }

    { static const char *sn[] = { "mlp", "freq", "recency", "ewma", "random" };
      printf("[*] scorer: %s (id=%u)\n", sn[G.scorer_id], G.scorer_id); }
    printf("[*] xtierd pid=%d mode=%s epoch_ms=%u interval=%d budget=%llu pages%s\n",
           (int)G.pid, m, G.epoch_ms, G.spe_interval,
           (unsigned long long)G.budget_pages,
           G.budget_pages ? "" : " (top-K disabled)");
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    FILE *features_csv = NULL, *stats_csv = NULL, *exec_csv = NULL;
    if (G.mode == 1 || G.mode == 2) {
        const char *p = getenv("XTIER_FEATURES_CSV");
        features_csv = fopen(p ? p : "features.csv", "w");
        if (!features_csv) { perror("features.csv"); return 1; }
    }
    { const char *p = getenv("XTIER_STATS_CSV");
      stats_csv = fopen(p ? p : "stats.csv", "w");
      if (stats_csv) { fprintf(stats_csv, "epoch,pages,inferences,keep,dem_cands,thresh,proc_ms\n"); } }
    { const char *p = getenv("XTIER_EXEC_CSV");
      exec_csv = fopen(p ? p : "exec.csv", "w");
      if (exec_csv) { fprintf(exec_csv, "t_s,dmajflt_s,pswpin_s,pswpout_s,resident_pages,demote_ok,demote_fail,cold_found,majflt_cum,cold_scanned,pref_pages,pref_regions\n"); } }

    if (spe_init() != 0) return 1;
    pol_init();
    exec_init();
    spe_rescan_threads();
    pol_load_meta();

    struct timespec last_ep, last_sec;
    clock_gettime(CLOCK_MONOTONIC, &last_ep);
    clock_gettime(CLOCK_MONOTONIC, &last_sec);

    while (!stop_flag) {
        struct pollfd pfd;
        pfd.fd = -1; pfd.events = POLLIN;
        int to_ms = 10;
        poll(&pfd, 0, to_ms);   // sleep slice; aux rings are polled by drain

        spe_drain_all(pol_sample, NULL);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double dt_ep = (now.tv_sec - last_ep.tv_sec) * 1e3 + (now.tv_nsec - last_ep.tv_nsec) / 1e6;
        double dt_sec = (now.tv_sec - last_sec.tv_sec) + (now.tv_nsec - last_sec.tv_nsec) / 1e9;

        if (dt_sec >= 1.0) {
            spe_rescan_threads();
            spe_close_dead();
            pol_load_meta();
            exec_second(exec_csv);
            last_sec = now;
        }

        if (dt_ep >= G.epoch_ms) {
            uint32_t ep_fin = pol_cur_epoch();
            pol_process_epoch(ep_fin, features_csv, stats_csv);
            exec_epoch(ep_fin);
            pol_next_epoch();
            // SPE interval adaptation: keep sample density inside the
            // rankable window [4K, 32K) samples/epoch. Reacts every 4 epochs.
            {
                static uint64_t last_samp;
                static uint32_t adapt_tick;
                if (++adapt_tick >= 4) {
                    adapt_tick = 0;
                    uint64_t cur_samp = spe_sample_count();
                    uint64_t delta = cur_samp - last_samp;   // per ~4 epochs
                    last_samp = cur_samp;
                    if (delta / 4 < 4000 && G.spe_interval > 1024) {
                        spe_set_interval((uint64_t)G.spe_interval / 2);
                        printf("[ADAPT] samples/epoch=%llu -> interval=%d\n",
                               (unsigned long long)(delta / 4), G.spe_interval);
                    } else if (delta / 4 > 32000 && G.spe_interval < 65536) {
                        spe_set_interval((uint64_t)G.spe_interval * 2);
                        printf("[ADAPT] samples/epoch=%llu -> interval=%d\n",
                               (unsigned long long)(delta / 4), G.spe_interval);
                    }
                }
            }
            last_ep = now;
        }
    }

    printf("[EXIT] samples=%llu inferences=%llu demote_ok=%llu demote_fail=%llu cold_found=%llu majflt=%llu\n",
           (unsigned long long)spe_sample_count(),
           (unsigned long long)pol_inferences(),
           (unsigned long long)XSTATS.demote_ok, (unsigned long long)XSTATS.demote_fail,
           (unsigned long long)XSTATS.cold_found, (unsigned long long)XSTATS.majflt_cum);
    if (features_csv) fclose(features_csv);
    if (stats_csv) fclose(stats_csv);
    if (exec_csv) fclose(exec_csv);
    return 0;
}
