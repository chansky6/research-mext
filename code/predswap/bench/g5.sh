#!/bin/sh
# G5:predswapd CPU/内存开销采样(daemon 自身,不含 bridge 的 BPF 内核部分)
set -u
P=/opt/predswap
CG=/sys/fs/cgroup/memory/g5
DUR=${1:-30}
rmdir $CG 2>/dev/null; mkdir -p $CG
echo 100 > $CG/memory.swappiness
echo 400M > $CG/memory.limit_in_bytes
ulimit -l unlimited
$P/predswapd run --cgroup $CG --budget-mb 400 --headroom-mb 64 \
  --scan-period 5 --cooldown 20 --major-guard-ps 3000 > /tmp/g5_daemon.out 2>&1 &
D=$!
CLK=$(getconf CLK_TCK)
read_daemon_stat() {
  awk '{print $14+$15, $24}' /proc/$D/stat 2>/dev/null
}
S0=$(read_daemon_stat)
RSS0=$(grep VmRSS /proc/$D/status 2>/dev/null | awk '{print $2}')
sh -c "echo \$\$ > $CG/tasks; exec $P/synth tiers 512 $DUR 7 20000" > /dev/null 2>&1
S1=$(read_daemon_stat)
RSS1=$(grep VmRSS /proc/$D/status 2>/dev/null | awk '{print $2}')
kill -TERM $D; wait $D 2>/dev/null
T0=$(echo $S0 | cut -d' ' -f1); T1=$(echo $S1 | cut -d' ' -f1)
STARTS0=$(echo $S0 | cut -d' ' -f2); STARTS1=$(echo $S1 | cut -d' ' -f2)
# 线程数按启动后新增线程近似:忽略,只报 CPU
CPU_CORES=$(awk -v t0=$T0 -v t1=$T1 -v d=$DUR -v c=$CLK 'BEGIN{printf "%.3f", (t1-t0)/c/d}')
echo "G5: daemon cpu=${CPU_CORES} core(s) avg over ${DUR}s; rss ${RSS0}kB -> ${RSS1}kB (托管 512MB ≈ $(awk -v r=$RSS1 'BEGIN{printf "%.2f", r/512000}')%)"
grep -E "^scan: idle.scan" /tmp/g5_daemon.out | tail -3
rmdir $CG 2>/dev/null
