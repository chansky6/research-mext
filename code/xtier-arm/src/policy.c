/* SPDX-License-Identifier: GPL-2.0 */
/*
 * policy.c — page state, features, INT8 MLP, filters, top-K gate.
 * Ported from xTier's userspace-policy twin (page_profiler_user.c us_* section),
 * which itself mirrors pebs_mlp_kern.c semantics. Direction note: on DRAM+SSD
 * the score ("predicted benefit") ranks DEMOTION candidates (lowest first)
 * rather than promotion; the math is unchanged.
 */
#include "xtierd.h"
#include <time.h>
#include <math.h>

#ifndef XTIER_MODEL_HDR
#define XTIER_MODEL_HDR "mlp_q8.h"
#endif
#include XTIER_MODEL_HDR

// ---- verbatim fixed-point helpers (pebs_mlp_kern.c) ----
static inline uint8_t clamp_u8(int64_t x) { if (x < 0) return 0; if (x > 255) return 255; return (uint8_t)x; }
static inline uint8_t clamp_relu_u8(int64_t x, int32_t zp) {
    if (x < (int64_t)zp) return (uint8_t)zp;
    if (x > 255) return 255;
    return (uint8_t)x;
}
static inline int64_t mul_shift_rnd(int64_t acc, int32_t M, int32_t sh) {
    int64_t t = (M != 0) ? (acc * (int64_t)M) : acc;
    if (sh < 0) sh = 0; if (sh > 63) sh = 63;
    if (sh > 0) { int64_t add = (t >= 0) ? ((int64_t)1 << (sh - 1)) : -((int64_t)1 << (sh - 1)); t += add; t >>= sh; }
    return t;
}
static inline uint8_t pack_delta(int32_t d) { if (d > 127) d = 127; if (d < -128) d = -128; return (uint8_t)(d + 128); }
static inline uint8_t log_scale(uint32_t val) {
    if (val == 0) return 32; if (val <= 5) return (uint8_t)(32 + val * 4);
    if (val <= 20) return (uint8_t)(52 + (val - 5) / 2);
    if (val <= 100) return (uint8_t)(60 + (val - 20) / 10);
    { uint32_t r = 70u + (val - 100u) / 50u; return (uint8_t)(r > 255u ? 255u : r); }
}
// xTier's rank-preserving quantizer for the alternate scorers (pebs_mlp_kern.c).
static inline uint8_t q255_log2(uint32_t v) {
    if (v == 0) return 0;
    uint32_t hi = 0;
    for (int b = 31; b >= 1; b--) { if (hi == 0 && (v >> b)) hi = (uint32_t)b; }
    uint32_t mant = (hi >= 4) ? ((v >> (hi - 4)) & 0xF) : ((v << (4 - hi)) & 0xF);
    uint32_t q = hi * 16u + mant;
    return (uint8_t)(q > 255u ? 255u : q);
}

static inline uint16_t off_bucket_calc(uint64_t page) { return (uint16_t)(((page >> PAGE_SHIFT) & 511u) & (OFF_BUCKETS - 1u)); }
static void get_scale_and_shift(float real_multiplier, int32_t *m, int32_t *s) {
    if (real_multiplier == 0.0f) { *m = 0; *s = 0; return; }
    int exp; float sig = frexpf(real_multiplier, &exp); *m = (int32_t)(sig * 2147483648.0f); *s = 31 - exp;
}

// ---- heuristic filter (us_check_heuristic_filter, verbatim semantics) ----
static int check_heuristic_filter(struct feature_row *row, uint8_t cur_tier, uint32_t cd_epochs) {
    if (cd_epochs != 0 && row->last_mig_epoch != 0 && row->epoch > row->last_mig_epoch &&
        (row->epoch - row->last_mig_epoch) < cd_epochs) return -3;
    int persistent = 1;
    if (row->hits < HOT_THRESHOLD) persistent = 0;
    for (int i = 0; i < K_PERSISTENT - 1; i++) { if (row->hits_prev[i] < HOT_THRESHOLD) persistent = 0; }
    if (persistent) {
        if (cur_tier == 1) return -4;
        return 1;
    }
    if (row->epoch >= WARMUP_EPOCHS) {
        int cold = 1; if (row->hits > 0) cold = 0;
        for (int i = 0; i < HIST_STEPS && i < COLD_EPOCHS - 1; i++) { if (row->hits_prev[i] > 0) cold = 0; }
        if (cold) {
            if (cur_tier == 2) return -5;
            return 2;
        }
        uint64_t sum = row->hits, sum_sq = (uint64_t)row->hits * row->hits; int N = 1;
        for (int i = 0; i < HIST_STEPS; i++) { sum += row->hits_prev[i]; sum_sq += (uint64_t)row->hits_prev[i] * (uint64_t)row->hits_prev[i]; N++; }
        uint64_t variance_scaled = (sum_sq * N - sum * sum) * 100; variance_scaled /= (N * N);
        if (variance_scaled < VAR_THRESHOLD_INT) {
            if (cur_tier == 1 && row->hits <= 1) return -1;
            return -2;
        }
    }
    return -1;
}

static struct dem_cand *dem_pool;
static uint32_t dem_n;

// ---- weights (us_build_weights) ----
static struct row0 w_l0[L0_OUT]; static struct mul w_l0m[L0_OUT];
static struct row1 w_l1[L1_OUT]; static struct mul w_l1m[L1_OUT];
static struct rowO w_lc;         static struct mul w_lcm;
static struct qparams qp;

uint64_t mlp_forward_(const uint8_t in[L0_IN]) {
    uint8_t l0o[L0_OUT], l1o[L1_OUT];
    for (int i = 0; i < L0_OUT; i++) {
        int64_t acc = w_l0[i].b;
        for (int j = 0; j < L0_IN; j++) acc += (int64_t)((int32_t)in[j] - qp.in_zp0) * w_l0[i].w[j];
        l0o[i] = clamp_relu_u8(mul_shift_rnd(acc, w_l0m[i].M, w_l0m[i].shift) + qp.out_zp0, qp.out_zp0);
    }
    for (int i = 0; i < L1_OUT; i++) {
        int64_t acc = w_l1[i].b;
        for (int j = 0; j < L0_OUT; j++) acc += (int64_t)((int32_t)l0o[j] - qp.out_zp0) * w_l1[i].w[j];
        l1o[i] = clamp_relu_u8(mul_shift_rnd(acc, w_l1m[i].M, w_l1m[i].shift) + qp.out_zp1, qp.out_zp1);
    }
    int64_t accC = w_lc.b;
    for (int i = 0; i < L1_OUT; i++) accC += (int64_t)((int32_t)l1o[i] - qp.out_zp1) * w_lc.w[i];
    return clamp_u8(mul_shift_rnd(accC, w_lcm.M, w_lcm.shift) + qp.out_zpC);
}

// ---- state table (us_state) ----
static struct pstate *state;
static uint32_t state_slots;
static uint32_t probe_max = 32;
static uint64_t state_evictions;
static uint32_t cur_epoch = 1;

static inline uint64_t hash64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x;
}
static struct pstate *state_get(uint64_t page, int create) {
    uint32_t idx = (uint32_t)(hash64(page) & (state_slots - 1));
    uint32_t first = idx;
    for (uint32_t p = 0; p < probe_max; p++) {
        struct pstate *e = &state[idx];
        if (e->page == page) return e;
        if (e->page == 0) {
            if (!create) return NULL;
            memset(e, 0, sizeof(*e));
            e->page = page;
            e->tier = 1;   // first sample = resident (access just faulted it in)
            return e;
        }
        idx = (idx + 1) & (state_slots - 1);
    }
    if (!create) return NULL;
    state_evictions++;
    struct pstate *e = &state[first];
    memset(e, 0, sizeof(*e));
    e->page = page; e->tier = 1;
    return e;
}
uint64_t pol_state_evictions(void) { return state_evictions; }

// ---- meta table (us_meta) ----
struct meta_ent { uint64_t reg; struct meta_val mv; };
static struct meta_ent *meta_tab;
static uint32_t meta_slots;

static void meta_put(uint64_t reg_base, const struct meta_val *mv) {
    uint32_t idx = (uint32_t)(hash64(reg_base | 1) & (meta_slots - 1));
    for (uint32_t p = 0; p < probe_max; p++) {
        struct meta_ent *e = &meta_tab[idx];
        if (e->reg == reg_base || e->reg == 0) { e->reg = reg_base; e->mv = *mv; return; }
        idx = (idx + 1) & (meta_slots - 1);
    }
    meta_tab[(uint32_t)(hash64(reg_base | 1) & (meta_slots - 1))] =
        (struct meta_ent){ .reg = reg_base, .mv = *mv };
}
static const struct meta_val *meta_get(uint64_t page) {
    uint64_t reg = page & ~((uint64_t)((1ull << REG_SHIFT) - 1ull));
    uint32_t idx = (uint32_t)(hash64(reg | 1) & (meta_slots - 1));
    for (uint32_t p = 0; p < probe_max; p++) {
        struct meta_ent *e = &meta_tab[idx];
        if (e->reg == reg) return &e->mv;
        if (e->reg == 0) return NULL;
        idx = (idx + 1) & (meta_slots - 1);
    }
    return NULL;
}

// ---- /proc/pid/maps -> meta (populate_meta_map logic) ----
static int bucket_from_size(unsigned long sz) {
    if (sz < 4UL * 1024UL) return 0;
    if (sz < 64UL * 1024UL) return 1;
    if (sz < 2UL * 1024UL * 1024UL) return 2;
    if (sz < 1024UL * 1024UL * 1024UL) return 3;
    return 4;
}
static int page_type_from(const char *perms, const char *path) {
    if (path && strstr(path, "[stack]")) return 3;
    if (path && strstr(path, "[heap]"))  return 4;
    if (perms && strchr(perms, 'x'))     return 1;
    if (perms && strchr(perms, 'w'))     return 2;
    return 0;
}
void pol_load_meta(void) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/maps", (int)G.pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, fp) > 0) {
        unsigned long start = 0, end = 0; char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        char *pathname = strchr(line, '/');
        if (!pathname) { pathname = strstr(line, "["); if (!pathname) pathname = (char *)""; }
        struct meta_val mv = {
            .perm_r = (uint8_t)(strchr(perms, 'r') != NULL),
            .perm_w = (uint8_t)(strchr(perms, 'w') != NULL),
            .perm_x = (uint8_t)(strchr(perms, 'x') != NULL),
            .page_type = (uint8_t)page_type_from(perms, pathname),
            .vma_size_bucket = (uint8_t)bucket_from_size(end - start),
            .anon = (uint8_t)(pathname[0] != '/'),
            .shared = (uint8_t)(strchr(perms, 's') != NULL),
        };
        unsigned long reg_s = start >> REG_SHIFT, reg_e = (end - 1) >> REG_SHIFT;
        for (unsigned long rg = reg_s; rg <= reg_e; rg++)
            meta_put((uint64_t)(rg << REG_SHIFT), &mv);
    }
    free(line); fclose(fp);
}

void pol_init(void) {
    state_slots = 1u << G.state_bits;
    state = calloc(state_slots, sizeof(struct pstate));
    meta_slots = 1u << 18;
    meta_tab = calloc(meta_slots, sizeof(struct meta_ent));
    dem_pool = calloc(MAX_PAGES_PER_EPOCH, sizeof(struct dem_cand));
    if (!state || !meta_tab) { fprintf(stderr, "pol alloc failed\n"); exit(1); }

    qp = (struct qparams){ .in_zp0 = L0_IN_ZP[0], .out_zp0 = L0_OUT_ZP[0],
                           .out_zp1 = L1_OUT_ZP[0], .out_zpC = L2_OUT_ZP[0],
                           .cls_thr_q = CLS_THR_QINT8[0] };
    G.out_zpC_cache = qp.out_zpC;
    for (int i = 0; i < L0_OUT; i++) {
        w_l0[i].b = L0_B_INT32[i];
        for (int j = 0; j < L0_IN; j++) w_l0[i].w[j] = L0_W[i * L0_IN + j];
        get_scale_and_shift((L0_IN_SCALE[0] * L0_W_SCALE[i]) / L0_OUT_SCALE[0], &w_l0m[i].M, &w_l0m[i].shift);
    }
    for (int i = 0; i < L1_OUT; i++) {
        w_l1[i].b = L1_B_INT32[i];
        for (int j = 0; j < L0_OUT; j++) w_l1[i].w[j] = L1_W[i * L0_OUT + j];
        get_scale_and_shift((L1_IN_SCALE[0] * L1_W_SCALE[i]) / L1_OUT_SCALE[0], &w_l1m[i].M, &w_l1m[i].shift);
    }
    w_lc.b = L2_B_INT32[0];
    for (int j = 0; j < L1_OUT; j++) w_lc.w[j] = L2_W[j];
    get_scale_and_shift((L2_IN_SCALE[0] * L2_W_SCALE[0]) / L2_OUT_SCALE[0], &w_lcm.M, &w_lcm.shift);
    printf("[*] model loaded: zpC=%d\n", qp.out_zpC);
}

// ---- per-sample state update (us_sample_cb port) ----
uint64_t pol_epoch_pages[MAX_PAGES_PER_EPOCH];
uint32_t pol_epoch_cnt;

static uint32_t rng_state = 0x9E3779B9u;
static inline uint32_t prandom(void) { uint32_t x = rng_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rng_state = x; }

static uint64_t samples_rx, inferences, epoch_inf;

void pol_sample(void *ctx, uint64_t page, int is_store) {
    (void)ctx; (void)is_store;
    samples_rx++;
    const struct meta_val *mv = meta_get(page);
    if (mv && (mv->page_type == 3 || mv->page_type == 1)) return;  // stack pinned / exec skipped
    struct pstate *st = state_get(page, 1);
    // roll_state_if_needed, verbatim
    if (st->last_epoch_seen != cur_epoch) {
        if (st->cur_hits == 0 && st->last_epoch_seen != 0) {
            uint32_t gap = cur_epoch - st->last_epoch_seen;
            st->recency_epochs = (st->recency_epochs + gap < 0xFFFF) ? st->recency_epochs + gap : 0xFFFF;
        }
        for (int i = HIST_STEPS - 1; i > 0; --i) st->hits_prev[i] = st->hits_prev[i - 1];
        st->hits_prev[0] = (uint32_t)st->cur_hits;
        st->cur_hits = 0;
        st->last_epoch_seen = cur_epoch;
    }
    st->cur_hits++;
    st->owner_pid = (uint32_t)G.pid;
    // a sample on a swapped page means the access faulted it back in.
    // Fix 1 (fault-back pinning): anchor the cooldown NOW — the page just
    // proved it is accessed, so it must not be a demotion candidate for the
    // cooldown window (equivalent to the kernel putting a faulted page on
    // the active list). Without this, collapsed sampling re-demoted just-
    // faulted hot pages every cooldown expiry (exp6b thrash spiral).
    if (st->tier == 2) { st->tier = 1; st->last_mig_epoch = cur_epoch; pref_note_fault(page); }
    if (st->last_enq_epoch == cur_epoch) return;
    st->last_enq_epoch = cur_epoch;
    if (pol_epoch_cnt < MAX_PAGES_PER_EPOCH) pol_epoch_pages[pol_epoch_cnt++] = page;
}

// ---- top-K (topk_threshold_from_hist port) ----
static uint64_t hist[SCORE_HIST_BUCKETS];

static uint32_t topk_threshold_from_hist(void) {
    uint64_t total = 0;
    for (int i = 0; i < SCORE_HIST_BUCKETS; i++) total += hist[i];
    if (total == 0 || G.budget_pages == 0) return 0;
    uint64_t K = (uint64_t)((double)total * G.topk_admit_fraction);
    if (K < 1) K = 1;
    if (K > G.budget_pages) K = G.budget_pages;
    uint64_t acc = 0; int cutoff = 0;
    for (int b = SCORE_HIST_BUCKETS - 1; b >= 0; b--) {
        acc += hist[b];
        if (acc >= K) { cutoff = b; break; }
        cutoff = b;
    }
    int32_t dyn = cutoff - 1 - G.out_zpC_cache;
    if (dyn < 1) dyn = 1;
    if (dyn > TOPK_THRESH_MAX) dyn = TOPK_THRESH_MAX;
    return (uint32_t)dyn;
}

// ---- demotion candidate pool ----

static int cand_cmp(const void *a, const void *b) {
    const struct dem_cand *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? -1 : 1;
    return x->last_seen < y->last_seen ? -1 : 1;
}

static void emit_feature_row_csv(FILE *csv, const struct feature_row *r) {
    if (!csv) return;
    static int printed_header = 0;
    if (!printed_header) {
        fprintf(csv, "epoch,page,hits,lat_sum,hits_prev_1,lat_prev_1,delta_hits_1,hits_prev_2,lat_prev_2,delta_hits_2,hits_prev_3,lat_prev_3,delta_hits_3,hits_prev_4,lat_prev_4,delta_hits_4,hits_prev_5,lat_prev_5,delta_hits_5,hits_prev_6,lat_prev_6,delta_hits_6,recency_epochs,epochs_since_promotion,fp1_hex,fp2_hex,fp3_hex,fp4_hex,perm_r,perm_w,perm_x,page_type,vma_size_bucket,anon,shared,off_bucket\n");
        printed_header = 1;
    }
    fprintf(csv, "%u,%#llx,%u,%u", r->epoch, (unsigned long long)r->page, r->hits, r->lat_sum);
    for (int i = 0; i < HIST_STEPS; i++) fprintf(csv, ",%u,%u,%d", r->hits_prev[i], r->lat_prev[i], r->delta_hits[i]);
    fprintf(csv, ",%u,%u,", r->recency_epochs, r->epochs_since_promotion);
    for (int j = 0; j < FP_HIST; j++) fprintf(csv, "%016llx,", (unsigned long long)0);
    fprintf(csv, "%u,%u,%u,%u,%u,%u,%u,%u\n", r->perm_r, r->perm_w, r->perm_x,
            r->page_type, r->vma_size_bucket, r->anon, r->shared, r->off_bucket);
}


// ---- per-epoch decision pass (us_process_epoch, direction-flipped) ----
// Score every touched page; build the demotion candidate pool from
// low-scoring resident pages; top-K threshold from the score histogram.
void pol_process_epoch(uint32_t ep_fin, FILE *features_csv, FILE *stats_csv) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    dem_n = 0;
    uint32_t n_inf = 0;
    uint32_t n_keep = 0;

    for (uint32_t i = 0; i < pol_epoch_cnt; i++) {
        uint64_t page = pol_epoch_pages[i];
        struct pstate *st = state_get(page, 0);
        if (!st) continue;

        struct feature_row row = { .epoch = ep_fin, .page = page, .pid = st->owner_pid,
                                   .last_mig_epoch = st->last_mig_epoch,
                                   .recency_epochs = st->recency_epochs,
                                   .epochs_since_promotion = st->recency_epochs,
                                   .off_bucket = off_bucket_calc(page) };
        if (st->last_epoch_seen == ep_fin) {
            row.hits = (uint32_t)st->cur_hits; row.lat_sum = (uint32_t)st->cur_hits;
            for (int h = 0; h < HIST_STEPS; h++) { row.hits_prev[h] = st->hits_prev[h]; row.lat_prev[h] = st->hits_prev[h]; }
        } else if (ep_fin == st->last_epoch_seen + 1) {
            row.hits_prev[0] = (uint32_t)st->cur_hits; row.lat_prev[0] = (uint32_t)st->cur_hits;
            for (int h = 1; h < HIST_STEPS; h++) { row.hits_prev[h] = st->hits_prev[h - 1]; row.lat_prev[h] = st->hits_prev[h - 1]; }
        }
        row.delta_hits[0] = (int32_t)row.hits - (int32_t)row.hits_prev[0];
        for (int d = 1; d < HIST_STEPS; d++) row.delta_hits[d] = (int32_t)row.hits_prev[d - 1] - (int32_t)row.hits_prev[d];

        const struct meta_val *mv = meta_get(page);
        if (mv) { row.perm_r = mv->perm_r; row.perm_w = mv->perm_w; row.perm_x = mv->perm_x;
                  row.page_type = mv->page_type; row.vma_size_bucket = mv->vma_size_bucket;
                  row.anon = mv->anon; row.shared = mv->shared; }
        if (mv && mv->page_type == 3) { st->last_score = 9999; st->last_inf_epoch = ep_fin; continue; } // stack: never demote
        if (mv && mv->page_type == 1) continue;                                                     // exec: skip

        emit_feature_row_csv(features_csv, &row);

        int heuristic = check_heuristic_filter(&row, st->tier, G.cooldown_epochs);
        if (heuristic == -3) continue;   // cooldown
        if (heuristic == 2 && st->tier == 1) {
            // persistently cold resident page: demote candidate regardless of MLP
            if (dem_pool) dem_pool[dem_n++] = (struct dem_cand){ .page = page, .score = -500, .last_seen = st->last_epoch_seen };
            continue;
        }
        if (heuristic == 1) { st->last_score = 9999; st->last_inf_epoch = ep_fin; n_keep++; continue; } // persistently hot

        uint8_t input[L0_IN];
        memset(input, 64, sizeof(input));
        input[0] = (uint8_t)(ep_fin & 0xFF);
        input[1] = log_scale(row.hits);
        input[2] = log_scale(row.hits);
        for (int h = 0; h < HIST_STEPS; h++) {
            input[3 + h * 3 + 0] = log_scale(row.hits_prev[h]);
            input[3 + h * 3 + 1] = log_scale(row.hits_prev[h]);
            input[3 + h * 3 + 2] = pack_delta(row.delta_hits[h]);
        }
        input[21] = log_scale((uint32_t)row.recency_epochs);
        input[22] = log_scale((uint32_t)row.epochs_since_promotion);
        if (mv) {
            input[23] = mv->anon; input[24] = mv->shared;
            input[27] = mv->perm_r; input[28] = mv->perm_w; input[29] = mv->perm_x;
            input[30] = mv->page_type; input[31] = mv->vma_size_bucket;
        }
        uint8_t cls_u8 = (uint8_t)mlp_forward_(input);
        pref_note_score(page, (int32_t)cls_u8 - G.out_zpC_cache, ep_fin);   // region-level admission signal for prefetch
        // scorer ablation: same gates downstream, only the ranking changes
        if (G.scorer_id == XT_SCORER_FREQ) {
            uint32_t sum = row.hits;
            for (int h = 0; h < HIST_STEPS; h++) sum += row.hits_prev[h];
            cls_u8 = q255_log2(sum);
        } else if (G.scorer_id == XT_SCORER_RECENCY) {
            cls_u8 = (uint8_t)(255u - q255_log2((uint32_t)row.recency_epochs));
        } else if (G.scorer_id == XT_SCORER_EWMA) {
            uint32_t ew = row.hits << 5;
            for (int h = 0; h < HIST_STEPS; h++) ew += row.hits_prev[h] << (4 - (h > 4 ? 4 : h));
            cls_u8 = q255_log2(ew);
        } else if (G.scorer_id == XT_SCORER_RANDOM) {
            cls_u8 = (uint8_t)(prandom() & 0xFFu);
        }
        n_inf++;
        hist[cls_u8]++;
        int32_t raw = (int32_t)cls_u8 - G.out_zpC_cache;
        st->last_score = raw;
        st->last_inf_epoch = ep_fin;
        if (st->tier == 1) {
            // resident: demotion candidate unless the score clears the top-K gate
            uint32_t thresh = G.thresh_dyn > 0 ? G.thresh_dyn : 1;
            // Fix 2 (demotion eligibility gate): require evidence across epochs.
            // A single-epoch sighting under collapsed sampling is indistinguishable
            // hot vs cold (exp6b: sample density fell 5x, every page looked
            // "1 hit, cold", and the daemon re-demoted just-faulted hot pages).
            // The heuristic-cold path (COLD_EPOCHS of zero hits) and the idle-bit
            // cold scan keep their own stronger evidence and are unaffected.
            int multi_epoch = (row.hits >= 2) || row.hits_prev[0] > 0 || row.hits_prev[1] > 0;
            if (raw <= (int32_t)thresh && multi_epoch && dem_pool) {
                if (dem_n < MAX_PAGES_PER_EPOCH)
                    dem_pool[dem_n++] = (struct dem_cand){ .page = page, .score = raw, .last_seen = st->last_epoch_seen };
            } else {
                n_keep++;
            }
        }
    }
    inferences += n_inf;
    epoch_inf = n_inf;

    // top-K threshold for the next epoch
    if (G.budget_pages > 0 && ep_fin >= TOPK_WARMUP_EPOCHS) {
        uint32_t t = topk_threshold_from_hist();
        if (t > 0) G.thresh_dyn = t;
    }
    memset(hist, 0, sizeof(hist));

    qsort(dem_pool, dem_n, sizeof(struct dem_cand), cand_cmp);   // lowest score first

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t dur_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (t1.tv_nsec - t0.tv_nsec);
    printf("[EPOCH] ep=%u pages=%u inf=%u keep=%u dem_cands=%u thresh=%u proc_ms=%.2f\n",
           ep_fin, pol_epoch_cnt, n_inf, n_keep, dem_n, G.thresh_dyn, dur_ns / 1e6);
    if (stats_csv) {
        fprintf(stats_csv, "%u,%u,%u,%u,%u,%u,%.3f\n", ep_fin, pol_epoch_cnt, n_inf, n_keep,
                dem_n, G.thresh_dyn, dur_ns / 1e6);
        fflush(stats_csv);
    }
    pol_epoch_cnt = 0;
}

void pol_next_epoch(void) { cur_epoch++; pref_note_epoch(cur_epoch); }
uint32_t pol_cur_epoch(void) { return cur_epoch; }
uint64_t pol_samples_rx(void) { return samples_rx; }
uint64_t pol_inferences(void) { return inferences; }
struct pstate *pol_state_get(uint64_t page, int create) { return state_get(page, create); }
struct pstate *pol_state_peek(uint32_t idx, uint64_t *page_out) {
    struct pstate *e = &state[idx];
    if (e->page == 0) return NULL;
    if (page_out) *page_out = e->page;
    return e;
}
uint32_t pol_state_slots(void) { return state_slots; }
struct dem_cand *pol_dem_pool(void) { return dem_pool; }
uint32_t pol_dem_n(void) { return dem_n; }
const struct meta_val *pol_meta_get(uint64_t page) { return meta_get(page); }
