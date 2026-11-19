#!/bin/sh
# B1(原生 swap)vs B2(仅卸载控制器)对照:同负载同种子,zipf 512MB @ 400M 限额
# 用法: sh /tmp/b1b2.sh [dur_s]
set -u
P=/opt/predswap
CG=/sys/fs/cgroup/memory/b2
DUR=${1:-30}
mkdir -p $CG

run_one() {
  TAG=$1
  rmdir $CG 2>/dev/null; mkdir -p $CG
  echo 100 > $CG/memory.swappiness
  echo 400M > $CG/memory.limit_in_bytes
  if [ "$TAG" = B2 ]; then
    rm -f /tmp/b2.psrec
    $P/predswapd run --cgroup $CG --budget-mb 400 --headroom-mb 64 \
      --scan-period 5 --cooldown 20 --major-guard-ps 3000 \
      --record /tmp/b2.psrec > /tmp/b2_daemon.out 2>&1 &
    DPID=$!
  fi
  sh -c "echo \$\$ > $CG/tasks; exec $P/synth tiers 512 $DUR 7 20000" > /tmp/${TAG}_synth.out 2>/tmp/${TAG}_synth.err
  PO=$(awk '/^zipf ops/ {print $2}' /tmp/${TAG}_synth.out)
  MJ=$(awk '/^steady/ {print $3}' /tmp/${TAG}_synth.out | cut -d= -f2)
  ST=$(awk '/^steady/ {print $4}' /tmp/${TAG}_synth.out | cut -d= -f2)
  STAT=$(grep -E "^(pswpin|pswpout|pgmajfault) " $CG/memory.stat | tr '\n' ' ')
  echo "RESULT $TAG dur=$DUR ops=$PO $ST swapstat=[$STAT]"
  if [ "$TAG" = B2 ]; then
    sleep 1
    $P/predswapd status | head -4
    kill -TERM $DPID; sleep 1
    grep -v "^bridge" /tmp/b2_daemon.out | grep -c "^ctrl" | xargs echo "ctrl ticks acted:"
    grep "^ctrl" /tmp/b2_daemon.out | tail -3
  fi
}

run_one B1
run_one B2
rmdir $CG 2>/dev/null
