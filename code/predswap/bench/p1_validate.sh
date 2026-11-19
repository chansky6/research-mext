#!/bin/sh
# P1 验证脚本 1:BPF 事件流正确性(对已知地址) + agent WILLNEED 生效性
set -u
P=/opt/predswap
CG=/sys/fs/cgroup/memory/p1t
LOG=/tmp/p1test.log
: > $LOG
log(){ echo "$@" | tee -a $LOG; }
ulimit -l unlimited

# ---------- T1:事件流 ----------
log "=== T1 event stream ==="
rmdir $CG 2>/dev/null
mkdir -p $CG
echo 100 > $CG/memory.swappiness
echo 120M > $CG/memory.limit_in_bytes

sh -c 'echo $$ > '$CG'/tasks; exec '$P'/synth block 256 1 1' > /tmp/t1_synth.out 2> /tmp/t1_synth.err &
SPID=$!
for i in $(seq 1 50); do grep -q "base=" /tmp/t1_synth.err && break; sleep 0.2; done
log "synth pid=$SPID $(head -1 /tmp/t1_synth.err)"

( echo "+$SPID"; sleep 15 ) | timeout 16 $P/ps_bridge $P/predswap.bpf.o > /tmp/t1_ev.bin 2>/tmp/t1_bridge.err &
BPID=$!
wait $SPID
log "--- synth output ---"; sed -n '1,6p' /tmp/t1_synth.out | tee -a $LOG
kill $BPID 2>/dev/null; wait $BPID 2>/dev/null
log "--- bridge stderr ---"; head -2 /tmp/t1_bridge.err | tee -a $LOG

python3 - <<'PY' 2>&1 | tee -a /tmp/p1test.log
import struct, re
data = open("/tmp/t1_ev.bin","rb").read()
n = len(data)//32
m = re.search(r"base=(0x[0-9a-f]+)", open("/tmp/t1_synth.err").read())
base = int(m.group(1),16) if m else 0
end = base + 256*1024*1024
swapin=miss=inrange=0
lo=hi=None
for i in range(n):
    ts,va,tgid,tid,kind = struct.unpack_from("<QQIIB", data, i*32)
    if kind==0: swapin+=1
    else: miss+=1
    lo = va if lo is None or va<lo else lo
    hi = va if hi is None or va>hi else hi
    if base <= va < end: inrange+=1
print("events=%d swapin=%d miss=%d" % (n, swapin, miss))
print("va range: 0x%x..0x%x  synth region: 0x%x..0x%x" % (lo or 0, hi or 0, base, end))
print("inrange=%d/%d (%.1f%%)" % (inrange, n, 100.0*inrange/max(n,1)))
PY

# ---------- T2:agent WILLNEED ----------
log ""
log "=== T2 agent WILLNEED ==="
mkdir -p /tmp/predswap/agents
rm -f /tmp/predswap/agent.sock /tmp/predswap/agents/*.sock
python3 /tmp/fake_daemon.py > /tmp/t2_fake.out 2>&1 &
FAKE=$!
sleep 0.3

sh -c 'echo $$ > '$CG'/tasks; LD_PRELOAD='$P'/predswap_agent.so exec '$P'/synth block 256 1 1' > /tmp/t2_synth.out 2>/tmp/t2_synth.err
log "--- synth(agent) ---"; sed -n '1,6p' /tmp/t2_synth.out | tee -a $LOG
head -1 /tmp/t2_synth.err | tee -a $LOG
wait $FAKE
log "--- fake daemon ---"; cat /tmp/t2_fake.out | tee -a $LOG
log "=== done ==="
rmdir $CG 2>/dev/null
