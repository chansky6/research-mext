#!/usr/bin/env python3
"""ceil.py TRACE MAP NK BUDGET_MB WARM MEAS [WARMUP]
Ceilings for page-granularity replacement on a replayed trace, using the real
item addresses taken by `mctrace map` before any eviction.

Window [WARM, WARM+MEAS) is measured after replaying WARMUP requests (default
1000000) so neither policy starts cold in the window. Reported per budget:

  requests   requests that need at least one page not resident
  comp.      of those, requests touching a page never seen before (compulsory)
  excess     requests-comp.  the part a replacement or prefetch policy can act on

  LRU        exact LRU over item pages
  OPT        Belady over the same touches, full knowledge of the window's future

OPT is the hard ceiling: no predictor can do better. LRU-excess minus OPT-excess
is the entire prize for prediction at this budget.
"""
import sys, heapq
from array import array
from collections import OrderedDict

tr, mp, nk, budget_mb, warm, meas = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
warmup = int(sys.argv[7]) if len(sys.argv) > 7 else 1000000
PG = 4096
CAP = (budget_mb << 20) // PG
W0 = max(0, warm - warmup)

rec = array('I'); rec.frombytes(open(tr, 'rb').read(12 * (warm + meas)))
k = rec[0::3]
f = open(mp, 'rb')
addr = array('Q'); nb = array('I'); ks = array('I')
for _ in range(nk):
    addr.frombytes(f.read(8)); nb.frombytes(f.read(4)); ks.frombytes(f.read(4))

idx = {}
seq = array('i')
rstart = array('i', [0])
for i in range(W0, warm + meas):
    key = k[i]; a = addr[key]
    if a:
        sz = 56 + nb[key] + ks[key] + 2
        for p in range(a & ~(PG - 1), ((a + sz) & ~(PG - 1)) + 1, PG):
            j = idx.get(p)
            if j is None:
                j = len(idx); idx[p] = j
            seq.append(j)
    rstart.append(len(seq))
N = len(seq); P = len(idx); NR = warm + meas - W0
print('%d MiB: %d pages (%.0f MiB) in play, capacity %d pages, warmup %d requests, touches %d'
      % (budget_mb, P, P * PG / 2**20, CAP, warm + meas - W0 - meas, N), flush=True)

seen = bytearray(P)
for t in range(rstart[NR - meas]):
    seen[seq[t]] = 1

def report(tag, fault):
    req = comp = 0
    for r in range(NR - meas, NR):
        hit = False
        for t in range(rstart[r], rstart[r + 1]):
            if fault[t]:
                hit = True
                if not seen[seq[t]]:
                    comp += 1
                    break
        req += hit
    print('%-4s requests %7d (%.2f%%) compulsory %6d (%.2f%%) excess %6d (%.2f%%)'
          % (tag, req, 100.0 * req / meas, comp, 100.0 * comp / meas, req - comp, 100.0 * (req - comp) / meas), flush=True)
    return req - comp

lru = OrderedDict(); fl = bytearray(N)
for t in range(N):
    j = seq[t]
    if j in lru:
        lru.move_to_end(j)
    else:
        fl[t] = 1; lru[j] = 1
        if len(lru) > CAP:
            lru.popitem(last=False)
lru_ex = report('LRU', fl)
del lru, fl

nxt = array('i', [-1]) * N
last = {}
for t in range(N - 1, -1, -1):
    j = seq[t]; nxt[t] = last.get(j, -1); last[j] = t
del last
INF = N + 1
field = [-1] * P
heap = []; fl2 = bytearray(N); res = 0
for t in range(N):
    j = seq[t]
    n = nxt[t] if nxt[t] >= 0 else INF + t   # unseen again: farthest eviction candidate
    if field[j] < 0:
        fl2[t] = 1; res += 1
    field[j] = n
    heapq.heappush(heap, (-n, j))
    while res > CAP:
        neg, pj = heapq.heappop(heap)
        if field[pj] == -neg:
            field[pj] = -1; res -= 1
opt_ex = report('OPT', fl2)
print('prediction prize at %d MiB: %.2f%% of requests' % (budget_mb, 100.0 * (lru_ex - opt_ex) / meas))
