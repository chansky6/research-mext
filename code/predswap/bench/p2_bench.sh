#!/bin/sh
# P2 微基准 v2:
#   seq6k  (M1 主场):扫描 6K ops/s < IO 预算 7.6K/s,预取应显著降 major
#   seq20k (压差):20K/s 冷流超出预算,预期平手(预算诚实)
#   chain  (M2 主场):10K ops/s,M2 学到链后继
#   block  (M3 主场):sleep 30s 让控制器成组
#   zipf   (G2):随机负载 3 reps,P 不劣于 B1 超过 3%
# 用法: sh /tmp/p2_bench.sh
set -u
P=/opt/predswap
CG=/sys/fs/cgroup/memory/p2t
WS=512
BUDGET_MB=256
mkdir -p $CG
rm -f /tmp/p2_results.txt

run_case() {
  TAG=$1; ARGS=$2
  for V in B1 P; do
    rmdir $CG 2>/dev/null; mkdir -p $CG
    echo 100 > $CG/memory.swappiness
    echo ${BUDGET_MB}M > $CG/memory.limit_in_bytes
    if [ "$V" = P ]; then
      rm -f /tmp/p2_${TAG}.psrec
      $P/predswapd run --cgroup $CG --budget-mb $BUDGET_MB --headroom-mb 64 \
        --scan-period 5 --cooldown 20 --major-guard-ps 3000 \
        --record /tmp/p2_${TAG}.psrec > /tmp/p2_${TAG}_daemon.out 2>&1 &
      D=$!
      sleep 1
    fi
    sh -c "echo \$\$ > $CG/tasks; LD_PRELOAD=$P/predswap_agent.so exec $P/synth $ARGS" \
      > /tmp/p2_${TAG}_${V}.out 2>/dev/null
    LAST=$(grep -E "^(seq|chain|zipf|tiers) " /tmp/p2_${TAG}_${V}.out | tail -1)
    VB=$(grep verify /tmp/p2_${TAG}_${V}.out | tail -1)
    CGMJ=$(grep "^pgmajfault" $CG/memory.stat | awk '{print $2}')
    echo "CASE $TAG $V: cgmaj=$CGMJ [$LAST] [$VB]" >> /tmp/p2_results.txt
    if [ "$V" = P ]; then
      $P/predswapd status | grep -E "^  M[0-3]|^agent:" >> /tmp/p2_results.txt
      kill -TERM $D; sleep 1
      $P/predswapd replay /tmp/p2_${TAG}.psrec | grep -E "shadow_acc|dispatch records" >> /tmp/p2_results.txt
    fi
  done
}

run_case seq6k  "seq $WS 90 6000"
run_case seq20k "seq $WS 60 20000"
run_case chain  "chain $WS 90 11 10000"
run_case block  "block $WS 1 7 30"

# G2:zipf 3 reps 交错
r=1
while [ $r -le 3 ]; do
  for V in B1 P; do
    rmdir $CG 2>/dev/null; mkdir -p $CG
    echo 100 > $CG/memory.swappiness
    echo ${BUDGET_MB}M > $CG/memory.limit_in_bytes
    if [ "$V" = P ]; then
      $P/predswapd run --cgroup $CG --budget-mb $BUDGET_MB --headroom-mb 64 \
        --scan-period 5 --cooldown 20 --major-guard-ps 3000 \
        --record /tmp/p2_zipf$r.psrec > /tmp/p2_zipf${r}_daemon.out 2>&1 &
      D=$!
      sleep 1
    fi
    sh -c "echo \$\$ > $CG/tasks; LD_PRELOAD=$P/predswap_agent.so exec $P/synth zipf $WS 60 7" \
      > /tmp/p2_zipf${r}_${V}.out 2>/dev/null
    CGMJ=$(grep "^pgmajfault" $CG/memory.stat | awk '{print $2}')
    OPS=$(grep "^zipf ops" /tmp/p2_zipf${r}_${V}.out | awk '{print $3}')
    echo "CASE zipf$r $V: cgmaj=$CGMJ ops=$OPS" >> /tmp/p2_results.txt
    if [ "$V" = P ]; then
      kill -TERM $D; sleep 1
    fi
  done
  r=$((r+1))
done

echo "=== P2 v2 results (WS=${WS}M budget=${BUDGET_MB}M) ==="
cat /tmp/p2_results.txt
rmdir $CG 2>/dev/null
