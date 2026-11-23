#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# One tiering experiment: workload + xtierd, CSVs under $RES/$TAG.
#
#   bench/run.sh <tag> <gb> <theta> <mops_per_thread> <threads> <budget_mb> [mode] [seconds]
# defaults: mode=run, seconds=120, epoch_ms=1000, interval=4096
set -u
TAG=${1:?tag}; GB=${2:?gb}; THETA=${3:?theta}; MOPS=${4:?mops}; NTHR=${5:?threads}
BUDGET_MB=${6:?budget_mb}; MODE=${7:-run}; SECS=${8:-120}
EPOCH_MS=${EPOCH_MS:-1000}
INTERVAL=${INTERVAL:-4096}
ROOT=${XTIER_ARM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
RES=${RES:-/var/tmp/xtier-results}/$TAG
mkdir -p "$RES"

source "$ROOT/env.sh"
xtier_arm_sysprep >/dev/null

cd "$ROOT"
numactl --cpunodebind=0 --membind=0 ./bench/zipf_chase "$GB" "$THETA" "$MOPS" "$NTHR" 1 \
    > "$RES/workload.log" 2>&1 &
WPID=$!
echo "workload pid=$WPID tag=$TAG mode=$MODE budget=${BUDGET_MB}MB"

XTIER_FEATURES_CSV="$RES/features.csv" \
XTIER_STATS_CSV="$RES/stats.csv" \
XTIER_EXEC_CSV="$RES/exec.csv" \
XTIER_DRAM_BUDGET_MB="$BUDGET_MB" \
XTIER_MIG_BUDGET="${XTIER_MIG_BUDGET:-2048}" \
timeout "$SECS" ./src/xtierd "$WPID" "$INTERVAL" "$EPOCH_MS" "$MODE" 2>&1 | tee "$RES/xtierd.log" | tail -5

kill $WPID 2>/dev/null; wait 2>/dev/null
echo "=== summary $TAG ==="
tail -3 "$RES/exec.csv" 2>/dev/null
grep -c . "$RES/features.csv" 2>/dev/null | xargs -I{} echo "features rows: {}"
