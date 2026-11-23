#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Two-phase XGBoost workload, ported from xTier bench/workloads/xgboost/phase_change.py.
# Scaled for a ~2.5GB DRAM budget: 12M x 30 float32 = 1.44GB matrix, DMatrix
# copy brings the phase working set to ~3GB. Dataset generation is CHUNKED
# (make_classification in row blocks) so the float64 transient never exceeds
# the cgroup limit; the original builds the whole array at once.
import argparse, ctypes, gc, os, time
import numpy as np
import xgboost as xgb
from sklearn.datasets import make_classification

def trim_libc():
    try:
        ctypes.CDLL("libc.so.6").malloc_trim(0)
    except OSError:
        pass

def build_dataset(n_samples, n_features, seed, label, chunk=1_000_000):
    print(f"[+] [{label}] generating {n_samples:,} x {n_features} (seed={seed}, chunked)")
    t0 = time.time()
    parts = []
    done = 0
    while done < n_samples:
        n = min(chunk, n_samples - done)
        Xc, yc = make_classification(
            n_samples=n, n_features=n_features,
            n_informative=max(2, int(n_features*0.6)),
            n_redundant=int(n_features*0.2), n_repeated=0,
            n_classes=2, flip_y=0.01, class_sep=1.0,
            random_state=seed + done,
        )
        parts.append((Xc.astype(np.float32), yc.astype(np.int32)))
        done += n
    X = np.vstack([p[0] for p in parts]); y = np.concatenate([p[1] for p in parts])
    del parts; gc.collect(); trim_libc()
    dmat = xgb.DMatrix(X, label=y, nthread=8)
    print(f"[+] [{label}] built in {time.time()-t0:.1f}s (matrix: {X.nbytes/(1024**3):.2f} GiB)")
    return X, y, dmat

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--samples", type=int, default=12_000_000)
    p.add_argument("--features", type=int, default=30)
    p.add_argument("--phase1-rounds", type=int, default=60)
    p.add_argument("--phase2-iters", type=int, default=300)
    args = p.parse_args()
    params = {
        "objective": "binary:logistic", "eval_metric": "logloss",
        "tree_method": "hist", "max_depth": 6, "learning_rate": 0.1,
        "subsample": 0.8, "colsample_bytree": 0.8, "nthread": 8,
    }
    print(f"[+] PID: {os.getpid()}")
    print(f"[PHASE_MARKER] start_of_run epoch_unix={time.time():.3f}")
    X1, y1, dtrain = build_dataset(args.samples, args.features, seed=42, label="phase1")
    print(f"[PHASE_MARKER] phase1_train_start epoch_unix={time.time():.3f}")
    t0 = time.time()
    bst = xgb.train(params, dtrain, num_boost_round=args.phase1_rounds, verbose_eval=20)
    print(f"[+] [phase1] training finished in {(time.time()-t0)/60:.2f} min")
    print(f"[PHASE_MARKER] phase1_train_end epoch_unix={time.time():.3f}")
    del X1, y1, dtrain; gc.collect(); trim_libc()   # sharp working-set shift
    print(f"[PHASE_MARKER] phase2_start epoch_unix={time.time():.3f}")
    X2, y2, dtest = build_dataset(args.samples, args.features, seed=7, label="phase2")
    t0 = time.time()
    sink = 0.0
    for i in range(args.phase2_iters):
        preds = bst.predict(dtest, output_margin=False)
        sink += float(preds[0])
    dt = time.time() - t0
    print(f"[+] [phase2] {args.phase2_iters} inference passes in {dt/60:.2f} min ({sink:.3f})")
    print(f"[PHASE_MARKER] phase2_end epoch_unix={time.time():.3f}")

if __name__ == "__main__":
    main()
