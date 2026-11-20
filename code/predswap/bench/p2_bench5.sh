#!/bin/sh
# P2 验证 v3:余量拓扑(硬限额 320M > 控制目标 256M > 热温集 ~290M)
#   cb   :冷块负载,B1 vs P(核心:成员在此工况应命中)
#   zipf :G2,3 reps 交错
# 用法: sh /tmp/p2_bench3.sh
set -u
P=/opt/predswap
CG=/sys/fs/cgroup/memory/p2t
LIMIT_MB=320   # 硬限额 = B + H
BUDGET_MB=320  # 控制器预算(目标 = 320-64 = 256M)
mkdir -p $CG
rm -f /tmp/p2_results5.txt

for V in B1 P; do
  rmdir $CG 2>/dev/null; mkdir -p $CG
  echo 100 > $CG/memory.swappiness
  echo ${LIMIT_MB}M > $CG/memory.limit_in_bytes
  if [ "$V" = P ]; then
    rm -f /tmp/p2_cb.psrec
    $P/predswapd run --cgroup $CG --budget-mb $BUDGET_MB --headroom-mb 64 \
      --scan-period 5 --cooldown 20 --major-guard-ps 3000 \
      --record /tmp/p2_cb.psrec > /tmp/p2_cb_daemon.out 2>&1 &
    D=$!
    sleep 1
  fi
  sh -c "echo \$\$ > $CG/tasks; LD_PRELOAD=$P/predswap_agent.so exec $P/synth cb 512 180 7 20000 2560 60000" \
    > /tmp/p2_cb_${V}.out 2>/dev/null
  LAST=$(grep "^cb " /tmp/p2_cb_${V}.out | tail -1)
  ST=$(grep "^steady" /tmp/p2_cb_${V}.out | tail -1)
  VB=$(grep verify /tmp/p2_cb_${V}.out | tail -1)
  CGMJ=$(grep "^pgmajfault" $CG/memory.stat | awk '{print $2}')
  echo "CASE cb $V: cgmaj=$CGMJ [$LAST] [$ST] [$VB]" >> /tmp/p2_results5.txt
  if [ "$V" = P ]; then
    $P/predswapd status >> /tmp/p2_results5.txt
    kill -TERM $D; sleep 1
    $P/predswapd replay /tmp/p2_cb.psrec | grep -E "shadow_acc|dispatch_off|dispatch records" >> /tmp/p2_results5.txt
  fi
done

r=1
while [ $r -le 3 ]; do
  for V in B1 P; do
    rmdir $CG 2>/dev/null; mkdir -p $CG
    echo 100 > $CG/memory.swappiness
    echo ${LIMIT_MB}M > $CG/memory.limit_in_bytes
    if [ "$V" = P ]; then
      $P/predswapd run --cgroup $CG --budget-mb $BUDGET_MB --headroom-mb 64 \
        --scan-period 5 --cooldown 20 --major-guard-ps 3000 \
        --record /tmp/p2_z$r.psrec > /tmp/p2_z${r}_daemon.out 2>&1 &
      D=$!
      sleep 1
    fi
    sh -c "echo \$\$ > $CG/tasks; LD_PRELOAD=$P/predswap_agent.so exec $P/synth zipf 512 60 7" \
      > /tmp/p2_z${r}_${V}.out 2>/dev/null
    CGMJ=$(grep "^pgmajfault" $CG/memory.stat | awk '{print $2}')
    OPS=$(grep "^zipf ops" /tmp/p2_z${r}_${V}.out | awk '{print $2}' | cut -d= -f2)
    echo "CASE zipf$r $V: cgmaj=$CGMJ ops=$OPS" >> /tmp/p2_results5.txt
    if [ "$V" = P ]; then
      $P/predswapd status | grep -E "^preds|^  M[0-3]|^agent:" >> /tmp/p2_results5.txt
      kill -TERM $D; sleep 1
    fi
  done
  r=$((r+1))
done

echo "=== P2 v3 (limit=${LIMIT_MB}M target=$((BUDGET_MB-64))M) ==="
cat /tmp/p2_results5.txt
rmdir $CG 2>/dev/null
