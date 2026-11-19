#!/usr/bin/env python3
"""fake daemon for T2:监听 agent HELLO,sleep 窗口内对最大匿名区后半发预取 REQ。"""
import socket, struct, time, os, sys

os.makedirs("/tmp/predswap/agents", exist_ok=True)
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
if os.path.exists("/tmp/predswap/agent.sock"):
    os.unlink("/tmp/predswap/agent.sock")
s.bind("/tmp/predswap/agent.sock")
s.settimeout(2)

def biggest_anon(pid):
    best = (0, None, None)
    try:
        for l in open("/proc/%d/maps" % pid):
            rng, rest = l.split(None, 1)
            a, b = rng.split("-")
            flags = rest.split()[0]
            if flags.startswith("rw") and "/" not in rest and "[" not in rest:
                sz = int(b, 16) - int(a, 16)
                if sz > best[0]:
                    best = (sz, int(a, 16), int(b, 16))
    except FileNotFoundError:
        pass
    return best

hello_pid = None
sent = False
deadline = time.time() + 45
while time.time() < deadline:
    try:
        d, a = s.recvfrom(8200)
    except socket.timeout:
        if sent:
            break
        continue
    if len(d) == 8 and d[:4] == struct.pack("<I", 1):
        hello_pid = struct.unpack("<II", d)[1]
        print("HELLO pid=%d" % hello_pid, flush=True)
        time.sleep(3.2)  # 等 synth warm + 进入 sleep(5) 阶段(此时 swap 已完成)
        # Phase 0 教训:顶限额时 WILLNEED 静默无效(充值失败)——先抬限额留 H 余量
        try:
            with open("/sys/fs/cgroup/memory/p1t/memory.limit_in_bytes", "w") as g:
                g.write("280M")
            print("limit raised to 280M", flush=True)
        except OSError as e:
            print("limit raise failed:", e, flush=True)
        sz, b0, b1 = biggest_anon(hello_pid)
        if b1 is None:
            print("no anon region found", flush=True)
            continue
        half = b0 + (b1 - b0) // 2
        buf = struct.pack("<II", 2, 1) + struct.pack("<QQ", half, b1 - half)
        s.sendto(buf, "/tmp/predswap/agents/a-%d.sock" % hello_pid)
        print("REQ sent: region 0x%x..0x%x (%dMB)" % (half, b1, (b1-half)>>20), flush=True)
        sent = True
    elif len(d) == 32 and d[:4] == struct.pack("<I", 3):
        magic, pid, req, adv, ret, pad = struct.unpack("<IIQQiI", d)
        print("ACK pid=%d req=%d adv=%d ret=%d" % (pid, req, adv, ret), flush=True)
        break
s.close()
