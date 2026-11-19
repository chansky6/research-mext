#!/bin/sh
# G6:负载运行中 kill -9 predswapd 十次,负载零错误、正常结束
set -u
P=/opt/predswap
CG=/sys/fs/cgroup/memory/g6
DUR=${1:-90}
rmdir $CG 2>/dev/null; mkdir -p $CG
echo 100 > $CG/memory.swappiness
echo 400M > $CG/memory.limit_in_bytes

$P/predswapd run --cgroup $CG --budget-mb 400 --headroom-mb 64 \
  --scan-period 5 --cooldown 20 --major-guard-ps 3000 > /tmp/g6_daemon.out 2>&1 &
DPID=$!
sh -c "echo \$\$ > $CG/tasks; exec $P/synth tiers 512 $DUR 7 20000" > /tmp/g6_synth.out 2>/tmp/g6_synth.err &
SPID=$!
RC_FILE=/tmp/g6_rc
# 每 8s 杀一次 daemon 并重启,共 10 次
i=0
while [ $i -lt 10 ]; do
  sleep 8
  if ! kill -0 $SPID 2>/dev/null; then echo "synth exited early" >&2; break; fi
  kill -9 $DPID 2>/dev/null
  sleep 1
  $P/predswapd run --cgroup $CG --budget-mb 400 --headroom-mb 64 \
    --scan-period 5 --cooldown 20 --major-guard-ps 3000 >> /tmp/g6_daemon.out 2>&1 &
  DPID=$!
  i=$((i+1))
  echo "kill/restart #$i" >&2
done
wait $SPID
RC=$?
echo "verify: $(grep verify /tmp/g6_synth.out)"
echo "synth rc=$RC kills=$i"
cat /tmp/g6_synth.out
head -1 /tmp/g6_synth.err
kill -TERM $DPID 2>/dev/null
sleep 1
if [ $RC -eq 0 ]; then echo "G6: PASS"; else echo "G6: FAIL (rc=$RC)"; fi
rmdir $CG 2>/dev/null
