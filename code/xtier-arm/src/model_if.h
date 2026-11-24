/* SPDX-License-Identifier: GPL-2.0 */
/*
 * model_if.h — pluggable promotion-scorer seam (xTier's MLP slot).
 *
 * Architecture (xTier-faithful, direction-flipped for DRAM+swap):
 *   demotion  = scan rules only (idle-bit cold scan, exec.c) — NO model.
 *   promotion = MODEL-driven: a swap-in fault marks a 2MB region as
 *               candidate; at the epoch boundary the model scores the
 *               faulting page's feature row; regions whose score clears
 *               the dynamic top-K threshold get batched swap-in
 *               (prefetch worker). This header is the model seam.
 *
 * Contract:
 *   input : struct feature_row — schema identical to the CSV emitted in
 *           collect mode (features.csv), so offline training and online
 *           scoring see the same features.
 *   output: centered int32 score, higher = hotter (promotion candidate).
 *           The promotion gate compares it to G.thresh_dyn (top-K).
 *   return: 0 = scored, -1 = error (page skipped).
 *
 * Plugging in your own model:
 *   1. implement `static int my_score(const struct feature_row *, int32_t *)`
 *   2. add `{ "myname", my_score }` to backends[] in model_if.c
 *   3. select at runtime: XTIER_MODEL=myname (env)
 * Shipped backends: none | mlp | freq | recency | ewma | random
 * ("none" = stub returning 0: promotion silent, pure scan-demotion daemon).
 */
#ifndef XTIER_MODEL_IF_H
#define XTIER_MODEL_IF_H

#include "xtierd.h"

typedef int (*model_score_fn)(const struct feature_row *row, int32_t *score_out);

int         model_if_init(void);   /* pick backend from XTIER_MODEL / XTIER_SCORER */
const char *model_if_name(void);
int         model_if_score(const struct feature_row *row, int32_t *score_out);

#endif
