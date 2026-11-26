#!/usr/bin/env python3
"""twprep.py: Twitter cache-trace text (stdin) -> mctrace records, plus statistics.

usage: zstd -dc clusterN.sort.zst | head -n LINES | twprep.py OUT.bin [VALUE_SCALE]

Input rows: timestamp,key,key_size,value_size,client_id,operation,ttl.
Output records: u32 key id (dense, by first appearance), u32 value size,
u16 key size, u16 op (0 get, 1 set, 2 delete).  A request that carries no
value size (a read, or a read that missed in production) gets the size its key
has elsewhere in the input, else the median.  TTLs are dropped; VALUE_SCALE
multiplies every value size.
"""
import struct
import sys
from array import array

OP = {'get': 0, 'gets': 0, 'set': 1, 'add': 1, 'replace': 1, 'cas': 1,
      'append': 1, 'prepend': 1, 'incr': 1, 'decr': 1, 'delete': 2}
ITEM = 56 + 10   # memcached item header + suffix, for the working-set estimate


def main():
    out = sys.argv[1]
    scale = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    ids = {}
    vsize = array('I')     # per key, 0 while unknown
    ksize = array('H')
    freq = array('I')
    rk = array('I')
    rv = array('I')
    ro = array('B')
    opn = {}
    tenth = []
    seen = 0
    new10 = 0
    next_mark = 10000000
    bad = 0
    t0 = t1 = None
    for line in sys.stdin.buffer:
        p = line.rstrip(b'\n').rsplit(b',', 5)
        h = p[0].split(b',', 1) if len(p) == 6 else ()
        n = len(rk) + 1
        if n >= next_mark:      # new-key share, by ten-million-request window
            tenth.append(new10 / (next_mark - (next_mark - 10000000)))
            new10 = 0
            next_mark += 10000000
        if len(h) != 2 or p[4] not in OP:
            bad += 1
            continue
        t = int(h[0])
        t1 = t
        if t0 is None:
            t0 = t
        k = ids.get(h[1])
        if k is None:
            k = ids[h[1]] = len(vsize)
            vsize.append(0)
            ksize.append(min(int(p[1]), 250))
            freq.append(0)
            new10 += 1
        v = int(p[2])
        if v:
            vsize[k] = v
        freq[k] += 1
        op = p[4]
        opn[op] = opn.get(op, 0) + 1
        rk.append(k)
        rv.append(v)
        ro.append(OP[op])
    n, nk = len(rk), len(ids)
    known = sorted(v for v in vsize if v)
    med = known[len(known) // 2]
    for k in range(nk):
        vsize[k] = max(1, int((vsize[k] or med) * scale))
    buf = bytearray(12 * n)
    pk = struct.Struct('<IIHH').pack_into
    for i in range(n):
        k = rk[i]
        pk(buf, 12 * i, k, max(1, int(rv[i] * scale)) if rv[i] else vsize[k], ksize[k], ro[i])
    with open(out, 'wb') as f:
        f.write(buf)

    print('requests %d keys %d span %d s (%.1f kqps) bad %d' % (n, nk, t1 - t0, n / max(t1 - t0, 1) / 1e3, bad))
    print('ops', ' '.join('%s=%.1f%%' % (k, 100 * v / n) for k, v in sorted(opn.items(), key=lambda kv: -kv[1])))
    sz = sorted(vsize)
    print('value size per key (x%g): p10 %d p50 %d p90 %d p99 %d max %d; no size in input %d (%.1f%%)' % (
        scale, sz[nk // 10], sz[nk // 2], sz[nk * 9 // 10], sz[nk * 99 // 100], sz[-1],
        nk - len(known), 100 * (nk - len(known)) / nk))
    wss = sum(ITEM + ksize[k] + vsize[k] for k in range(nk))
    print('working set %.0f MiB' % (wss / 2 ** 20))
    order = sorted(range(nk), key=freq.__getitem__, reverse=True)
    acc = b = 0
    marks = [0.01, 0.1, 0.5]
    cov = [0.5, 0.8, 0.9, 0.95, 0.99]
    mi = ci = 0
    for j, k in enumerate(order):
        acc += freq[k]
        b += ITEM + ksize[k] + vsize[k]
        if mi < len(marks) and j + 1 >= marks[mi] * nk:
            print('hottest %g%% of keys: %.1f%% of requests' % (100 * marks[mi], 100 * acc / n))
            mi += 1
        while ci < len(cov) and acc >= cov[ci] * n:
            print('%g%% of requests fit in %.0f MiB (%.1f%% of working set)' % (100 * cov[ci], b / 2 ** 20, 100 * b / wss))
            ci += 1
        if ci == len(cov) and mi == len(marks):
            break
    print('one-hit keys %.1f%%' % (100 * sum(1 for x in freq if x == 1) / nk))
    if tenth:
        print('new keys per 10M requests:', ' '.join('%.1fM' % (x / 1e6) for x in tenth))


main()
