#!/usr/bin/env python3
"""离线复刻 predswapd replay:分类器合并 + M1/M2 复刻,定位 M2 预测稀少的根因。"""
import struct
import sys
from collections import defaultdict

PAGE = 4096
data = open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/p2_chain.psrec", "rb").read()
assert data.startswith(b"PSREC1\n")
p = 7
evs = []
pagemap_groups = 0
while p < len(data):
    tag = data[p]
    if tag == ord("E"):
        ts, va, tgid, tid, kind = struct.unpack_from("<QQIIB", data, p + 1)
        evs.append((ts, va, tgid, tid, kind))
        p += 33
    elif tag in (ord("P"), ord("D")):
        nseg = struct.unpack_from("<I", data, p + 21)[0]
        p += 25 + nseg * 16
    elif tag == ord("A"):
        p += 26
    else:
        break
print(f"events={len(evs)}")

# 分类器复刻:SWAPIN(+MISS?)...RETRY
RETRY_WIN = 50_000_000
pend = {}
accesses = []  # (ts, va, tgid, tid, major)
for ts, va, tgid, tid, kind in evs:
    key = (tid, va)
    if kind == 0:
        if key in pend and ts - pend[key][0] <= RETRY_WIN:
            # retry 对:关闭
            prev_ts, has_miss = pend.pop(key)
            accesses.append((prev_ts, va, tgid, tid, has_miss))
        else:
            pend[key] = (ts, False)
    else:
        if key in pend:
            pt, hm = pend[key]
            pend[key] = (pt, True)
# sweep 关闭
for (tid, va), (ts, hm) in pend.items():
    accesses.append((ts, va, 0, tid, hm))
accesses.sort(key=lambda a: a[0])
maj = sum(1 for a in accesses if a[4])
print(f"accesses={len(accesses)} major={maj}")

# ---- M1 复刻 ----
m1_hist = defaultdict(lambda: {"last": 0, "d": [0, 0], "armed": 0})
m1_preds = 0
m1_preds_set = set()
# ---- M2 复刻 ----
m2_last = {}
m2_succ = {}  # vpn -> {succ: count}
m2_cap = 262144
m2_clears = 0
m2_preds = 0
m2_miss_lookup = 0
# 影子账本复刻(2s 窗口 dedup per (va, member))
open_pred = {}  # (va, member) -> ts
W = 2_000_000_000
m1_dedup = m2_dedup = 0
for ts, va, tgid, tid, has_miss in accesses:
    vpn = va // PAGE
    # M1
    h = m1_hist[tid]
    if h["last"]:
        d = vpn - h["last"]
        h["d"][0], h["d"][1] = h["d"][1], d
        h["armed"] = min(h["armed"] + 1, 2)
    h["last"] = vpn
    if h["armed"] >= 2 and h["d"][0] == h["d"][1] and h["d"][0] != 0:
        s = h["d"][1]
        for k in range(1, 9):
            key = ("M1", vpn + k * s)
            if key not in open_pred or ts - open_pred[key] > W:
                open_pred[key] = ts
                m1_preds += 1
            else:
                m1_dedup += 1
    # M2 observe
    prev = m2_last.get(tid)
    if prev is not None and prev != vpn:
        e = m2_succ.setdefault(prev, {})
        e[vpn] = e.get(vpn, 0) + 1
        if len(m2_succ) > m2_cap:
            m2_succ.clear()
            m2_clears += 1
        # 只留 top2
        if len(e) > 2:
            for k in sorted(e, key=lambda k: e[k])[:-2]:
                del e[k]
    m2_last[tid] = vpn
    # M2 predict(8 步链)
    cur = vpn
    for _ in range(8):
        e = m2_succ.get(cur)
        if not e:
            m2_miss_lookup += 1
            break
        nxt = max(e, key=lambda k: e[k])
        if nxt == 0:
            break
        key = ("M2", nxt)
        if key not in open_pred or ts - open_pred[key] > W:
            open_pred[key] = ts
            m2_preds += 1
        else:
            m2_dedup += 1
        cur = nxt
    # 清理过期 open_pred(粗略)
    if len(open_pred) > 200000:
        open_pred = {k: t for k, t in open_pred.items() if ts - t <= W}

print(f"M1 preds={m1_preds} deduped={m1_dedup}")
print(f"M2 preds={m2_preds} deduped={m2_dedup} succ_miss={m2_miss_lookup} clears={m2_clears} succ_size={len(m2_succ)}")
# 事件流特征
uniq = len({a[1] for a in accesses})
print(f"unique accessed pages={uniq}")
# 前后事件间隔分布
gaps = [b[0] - a[0] for a, b in zip(accesses, accesses[1:])][:5000]
gaps.sort()
if gaps:
    print(f"event gap p50={gaps[len(gaps)//2]/1000:.0f}us p90={gaps[int(len(gaps)*0.9)]/1000:.0f}us")
