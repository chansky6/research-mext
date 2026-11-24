/* SPDX-License-Identifier: GPL-2.0 */
/* xtier-arm — shared definitions (ported from xTier common_kern.h + us_*) */
#ifndef XTIERD_H
#define XTIERD_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- MODEL DIMENSIONS (fixed, matches xTier) ---
#define L0_IN   32
#define L0_OUT  192
#define L1_OUT  96

// --- LAYOUT / FEATURE CONSTANTS (xTier common_kern.h) ---
#define PAGE_SHIFT   12u
#define REG_SHIFT    21u
#define HIST_STEPS   6
#define FP_HIST      4
#define FP_WORDS_PER_SLOT (FP_SLOT_BITS / 64)   /* FP_SLOT_BITS = 256 */
#define FP_SLOT_BITS 256
#define OFF_BUCKETS  128
#define MAX_PAGES_PER_EPOCH 32768

// --- POLICY CONSTANTS (pebs_mlp_kern.c / us_*) ---
#define HOT_THRESHOLD     1
#define WARMUP_EPOCHS     3
#define COOLDOWN_EPOCHS   15
#define VAR_THRESHOLD_INT 25
#define K_PERSISTENT      0
#define COLD_EPOCHS       10
#define TOPK_WARMUP_EPOCHS 3
#define TOPK_THRESH_MAX    253
#define SCORE_HIST_BUCKETS 256

struct meta_val {
    uint8_t perm_r, perm_w, perm_x;
    uint8_t page_type;      // 0 other, 1 exec, 2 rw/anon, 3 stack, 4 heap
    uint8_t vma_size_bucket;
    uint8_t anon, shared;
    uint8_t _pad;
};

struct feature_row {
    uint32_t epoch;
    uint64_t page;
    uint32_t hits;
    uint32_t lat_sum;               // == hits (xTier's current impl)
    uint32_t hits_prev[HIST_STEPS];
    uint32_t lat_prev[HIST_STEPS];
    int32_t  delta_hits[HIST_STEPS];
    uint16_t recency_epochs;
    uint16_t epochs_since_promotion;
    uint64_t fp[FP_HIST][FP_WORDS_PER_SLOT];   // zeroed: training ignores
    uint8_t perm_r, perm_w, perm_x;
    uint8_t page_type;
    uint8_t vma_size_bucket;
    uint8_t anon;
    uint8_t shared;
    uint16_t off_bucket;
    uint64_t reg_id;
    uint32_t pid;
    uint32_t last_mig_epoch;
};

struct qparams { int32_t in_zp0, out_zp0, out_zp1, out_zpC, out_zpR, cls_thr_q; };
struct row0 { int32_t w[L0_IN];  int32_t b; };
struct row1 { int32_t w[L0_OUT]; int32_t b; };
struct rowO { int32_t w[L1_OUT]; int32_t b; };
struct mul  { int32_t M; int32_t shift; };

// --- PAGE STATE (us_pstate + demotion-ranking bookkeeping) ---
struct pstate {
    uint64_t page;              // 0 = empty slot
    uint64_t cur_hits;
    uint32_t hits_prev[HIST_STEPS];
    uint32_t last_epoch_seen;
    uint32_t last_mig_epoch;    // cooldown anchor (epoch of last demotion)
    uint32_t last_enq_epoch;
    uint32_t owner_pid;
    uint16_t recency_epochs;
    uint8_t  tier;              // 0 unknown, 1 DRAM-resident, 2 swapped-out
    uint8_t  _pad;
    int32_t  last_score;        // last MLP-derived keep score (raw - zpC)
    uint32_t last_inf_epoch;
};

// runtime config (env-populated)
struct xt_cfg {
    pid_t     pid;
    int       mode;             // 0 monitor, 1 collect, 2 evaluate, 3 run
    uint32_t  epoch_ms;
    uint64_t  budget_pages;     // 0 = disabled (static threshold path)
    double    topk_admit_fraction;
    uint32_t  cooldown_epochs;
    uint32_t  thresh_dyn;
    int32_t   out_zpC_cache;
    uint64_t  mig_budget;
    uint32_t  state_bits;
    int       spe_interval;
    int       spe_stores;
    uint32_t  cold_scan_chunk;
    uint32_t  storm_pswpin;     // fault-storm stand-down threshold (pswpin/s EWMA)
};
extern struct xt_cfg G;

// spe.c
typedef void (*spe_sample_fn)(void *ctx, uint64_t page, int is_store);
int  spe_init(void);
void spe_rescan_threads(void);
void spe_close_dead(void);
void spe_drain_all(spe_sample_fn cb, void *ctx);
uint64_t spe_sample_count(void);

// policy.c
void pol_init(void);
void pol_load_meta(void);
void pol_sample(void *ctx, uint64_t page, int is_store);
void pol_process_epoch(uint32_t ep_fin, FILE *features_csv, FILE *stats_csv);
void pol_next_epoch(void);
uint32_t pol_cur_epoch(void);
uint64_t pol_inferences(void);
struct pstate *pol_state_get(uint64_t page, int create);
const struct meta_val *pol_meta_get(uint64_t page);
struct pstate *pol_state_peek(uint32_t idx, uint64_t *page_out);
uint32_t pol_state_slots(void);
uint64_t mlp_forward_(const uint8_t in[L0_IN]);

// spe.c interval adaptation
void spe_set_interval(uint64_t period);

// fault_ch.c (do_swap_page kprobe channel)
int fault_ch_init(pid_t pid);
void fault_ch_poll(void);

// promotion model seam: see model_if.h (the only model decision in the
// loop is promotion; demotion is scan-rule driven, exec.c)

// exec.c
void exec_init(void);
void pref_note_fault(uint64_t page);
void pol_build_l0_input(const struct feature_row *row, uint8_t in[L0_IN]);
void pref_note_score(uint64_t page, int32_t raw, uint32_t epoch);
void pref_note_epoch(uint32_t epoch);
uint32_t pol_rb_take(uint64_t *out, uint32_t max);
void pref_start(void);
void exec_epoch(uint32_t ep_fin);
void exec_second(FILE *exec_csv);
uint64_t exec_resident_pages(void);
struct exec_stats {
    uint64_t demote_att, demote_ok, demote_fail;
    uint64_t cold_found, cold_scanned, cold_passes;
    uint64_t majflt_cum, pswpin_cum, pswpout_cum;
    uint64_t storm_epochs;
    uint64_t pref_pages, pref_regions;
    uint64_t rb_flips;
    uint64_t prom_cands;        // regions enqueued for model-driven promotion
};
extern struct exec_stats XSTATS;
#endif
