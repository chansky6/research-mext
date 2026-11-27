#!/usr/bin/env python3
# classify_tw.py FAULTS IMAP MAPS TRACE SKIP OPS
# Each demand fault (spd -F) is put in one class by the page it hit:
#   req      page holds an item requested in the measured window
#   cold     page holds only items not requested in the window
#   noitem   inside memcached's item regions but no mapped item (free chunk, new alloc, slab meta)
#   other    outside the item regions (hash table, stacks, libc, ...)
import sys, struct, collections
from array import array
import bisect

PG = 4096
HDR = 56
faults_p, imap_p, maps_p, trace_p, skip, ops = sys.argv[1:7]
skip, ops = int(skip), int(ops)

f = array('Q'); f.frombytes(open(faults_p, 'rb').read())
f = [a & ~(PG - 1) for a in f if not a >> 63]

raw = open(imap_p, 'rb').read()
n = len(raw) // 16
addr = array('Q'); meta = array('I')
for i in range(n):
    a, nb, ks = struct.unpack_from('<QII', raw, i * 16)
    addr.append(a); meta.append(nb + ks)

tr = open(trace_p, 'rb')
tr.seek(skip * 12)
win = tr.read(ops * 12)
want = set()
for i in range(0, len(win), 12):
    want.add(struct.unpack_from('<I', win, i)[0])

page_req, page_any = set(), set()
for k in range(n):
    a = addr[k]
    if not a:
        continue
    p = a & ~(PG - 1)
    end = a + HDR + meta[k]
    while p < end:
        page_any.add(p)
        if k in want:
            page_req.add(p)
        p += PG

# item regions: anonymous mappings that contain at least one item
regs = []
for line in open(maps_p):
    s, e = (int(x, 16) for x in line.split()[0].split('-'))
    regs.append((s, e, line.split()[-1] if len(line.split()) > 5 else '[anon]'))
pa = sorted(page_any)
item_regs = []
for s, e, _ in regs:
    i = bisect.bisect_left(pa, s)
    if i < len(pa) and pa[i] < e:
        item_regs.append((s, e))

def region(p):
    for s, e, name in regs:
        if s <= p < e:
            return name, s
    return '?', 0

cls = collections.Counter(); seen = collections.Counter(); first = collections.Counter()
other = collections.Counter()
for p in f:
    if p in page_req:
        c = 'req'
    elif p in page_any:
        c = 'cold'
    elif any(s <= p < e for s, e in item_regs):
        c = 'noitem'
    else:
        c = 'other'
        name, s = region(p)
        other[(name, hex(s))] += 1
    cls[c] += 1
    if not seen[p]:
        first[c] += 1
    seen[p] += 1

nf = len(f)
print('faults %d  distinct pages %d  window keys %d  items %d' % (nf, len(seen), len(want), n))
print('item pages %d (requested-in-window %d)' % (len(page_any), len(page_req)))
for c in ('req', 'cold', 'noitem', 'other'):
    print('%-7s %8d %5.1f%%  first-touch %8d  repeat %8d' % (c, cls[c], 100 * cls[c] / max(1, nf), first[c], cls[c] - first[c]))
for k, v in other.most_common(6):
    print('  other', k, v)
