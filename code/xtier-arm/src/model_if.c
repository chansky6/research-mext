/* SPDX-License-Identifier: GPL-2.0 */
/* model_if.c — backend registry for the promotion scorer seam. */
#include "model_if.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- backends -----------------------------------------------------------

// stub: no model opinion -> promotion channel silent, daemon is pure
// scan-demotion. Baseline for A/B and the safe default when no model is
// trained yet.
static int be_none(const struct feature_row *row, int32_t *out) {
    (void)row; *out = 0; return 0;
}

// reference backend: xTier's INT8 MLP (pebs_mlp_kern.c port, policy.c).
// Requires a compiled-in model header (Makefile MODEL_HDR).
static int be_mlp(const struct feature_row *row, int32_t *out) {
    uint8_t in[L0_IN];
    pol_build_l0_input(row, in);
    *out = (int32_t)(uint8_t)mlp_forward_(in) - G.out_zpC_cache;
    return 0;
}

// rank-preserving quantizer, same as policy.c (pebs_mlp_kern.c)
static inline uint8_t q255_log2_(uint32_t v) {
    if (v == 0) return 0;
    uint32_t hi = 0;
    for (int b = 31; b >= 1; b--) { if (hi == 0 && (v >> b)) hi = (uint32_t)b; }
    uint32_t mant = (hi >= 4) ? ((v >> (hi - 4)) & 0xF) : ((v << (4 - hi)) & 0xF);
    uint32_t q = hi * 16u + mant;
    return (uint8_t)(q > 255u ? 255u : q);
}

// ablation scorers (xTier pebs_mlp_kern.c alternates), same semantics as
// the pre-refactor XTIER_SCORER= values
static int be_freq(const struct feature_row *row, int32_t *out) {
    uint32_t sum = row->hits;
    for (int h = 0; h < HIST_STEPS; h++) sum += row->hits_prev[h];
    *out = (int32_t)q255_log2_(sum) - G.out_zpC_cache;
    return 0;
}
static int be_recency(const struct feature_row *row, int32_t *out) {
    *out = (int32_t)(255u - q255_log2_((uint32_t)row->recency_epochs)) - G.out_zpC_cache;
    return 0;
}
static int be_ewma(const struct feature_row *row, int32_t *out) {
    uint32_t ew = (uint32_t)row->hits << 5;
    for (int h = 0; h < HIST_STEPS; h++) ew += (uint32_t)row->hits_prev[h] << (4 - (h > 4 ? 4 : h));
    *out = (int32_t)q255_log2_(ew) - G.out_zpC_cache;
    return 0;
}
static uint32_t rng_state_ = 0x9E3779B9u;
static int be_random(const struct feature_row *row, int32_t *out) {
    (void)row;
    uint32_t x = rng_state_; x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state_ = x;
    *out = (int32_t)(x & 0xFFu) - G.out_zpC_cache;
    return 0;
}

static const struct { const char *name; model_score_fn fn; } backends[] = {
    { "none",    be_none },
    { "mlp",     be_mlp },
    { "freq",    be_freq },
    { "recency", be_recency },
    { "ewma",    be_ewma },
    { "random",  be_random },
};

static model_score_fn cur_fn = be_none;
static const char *cur_name = "none";

int model_if_init(void) {
    const char *m = getenv("XTIER_MODEL");
    if (!m || !*m) m = getenv("XTIER_SCORER");        // legacy alias
    if (!m || !*m) m = "mlp";                          // historical default
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (!strcmp(backends[i].name, m)) {
            cur_fn = backends[i].fn;
            cur_name = backends[i].name;
            printf("[*] model backend: %s (promotion scorer)\n", cur_name);
            return 0;
        }
    }
    fprintf(stderr, "[!] XTIER_MODEL='%s' unknown; have:", m);
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++)
        fprintf(stderr, " %s", backends[i].name);
    fprintf(stderr, "\n");
    return -1;
}

const char *model_if_name(void) { return cur_name; }

int model_if_score(const struct feature_row *row, int32_t *score_out) {
    return cur_fn(row, score_out);
}
