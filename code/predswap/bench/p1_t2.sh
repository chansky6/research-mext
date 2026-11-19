#!/bin/sh
# T2:agent WILLNEED 生效性(带 H 余量,Phase 0 §2-6 教训)
set -u
P=/opt/predswap
CG=/sys/fs/cgroup/memory/p1t
rm -f /tmp/predswap/agent.sock /tmp/predswap/agents/*.sock 2>/dev/null
rmdir $CG 2>/dev/null
mkdir -p $CG
echo 100 > $CG/memory.swappiness
echo 120M > $CG/memory.limit_in_bytes

python3 /tmp/fake_daemon.py > /tmp/t2f.out 2>&1 &
F=$!
sleep 0.3
sh -c "echo \$\$ > $CG/tasks; LD_PRELOAD=$P/predswap_agent.so exec $P/synth block 256 1 1" 2>/tmp/t2s.err
wait $F
echo "--- fake daemon ---"
cat /tmp/t2f.out
echo "--- cgroup stat ---"
grep -E "^(pswpin|pswpout|pgmajfault) " $CG/memory.stat 2>/dev/null
rmdir $CG 2>/dev/null
