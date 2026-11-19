#!/bin/sh
# B1 vs B2 @ 50% 预算,3 reps 交错:sh /tmp/b1b2_50.sh [dur_s] [reps]
set -u
P=/opt/predswap
CG=/sys/fs/cgroup/memory/b2
DUR=${1:-60}
REPS=${2:-3}
WS_MB=512
BUDGET_MB=256   # 50%

mkdir -p $CG
rm -f /tmp/b1b2_results.txt

run_one() {
  TAG=$1 REP=$2
  rmdir $CG 2>/dev/null; mkdir -p $CG
  echo 100 > $CG/memory.swappiness
  echo ${BUDGET_MB}M > $CG/memory.limit_in_bytes
  if [ "$TAG" = B2 ]; then
    rm -f /tmp/b2.psrec
    $P/predswapd run --cgroup $CG --budget-mb $BUDGET_MB --headroom-mb 64 \
      --scan-period 5 --cooldown 20 --major-guard-ps 3000 \
      --record /tmp/b2.psrec > /tmp/b2_daemon.out 2>&1 &
    DPID=$!
  fi
  sh -c "echo \$\$ > $CG/tasks; exec $P/synth tiers $WS_MB $DUR 7 20000" > /tmp/r${REP}_synth.out 2>/tmp/r${REP}_synth.err
  MJ=$(sed -n 's/^steady *majflt=\([0-9]*\) minflt=\([0-9]*\) time=.*$/\1 \2/p' /tmp/r${REP}_synth.out | tail -1 | cut -d' ' -f1)
  OPS=$(sed -n 's/^tiers ops=\([0-9]*\).*/\1/p' /tmp/r${REP}_synth.out | tail -1)
  CGMJ=$(grep "^pgmajfault" $CG/memory.stat | awk '{print $2}')
  T=$(sed -n 's/^steady .*time=\(.*\)s/\1/p' /tmp/r${REP}_synth.out | tail -1)
  echo "RESULT rep=$REP $TAG majflt=$MJ cgmaj=$CGMJ ops=$OPS" >> /tmp/b1b2_results.txt
  if [ "$TAG" = B2 ]; then
    $P/predswapd status | grep -E "offload|pid=" >> /tmp/b1b2_results.txt
    kill -TERM $DPID; sleep 1
    grep -c "pageout=[1-9]" /tmp/b2_daemon.out | xargs -I{} echo "  ctrl_acted_ticks={}" >> /tmp/b1b2_results.txt
  fi
}

r=1
while [ $r -le $REPS ]; do
  run_one B1 $r
  run_one B2 $r
  r=$((r+1))
done
echo "=== summary ($WS_MB MB WS, ${BUDGET_MB}MB budget = 50%, dur=${DUR}s) ==="
cat /tmp/b1b2_results.txt
rmdir $CG 2>/dev/null
