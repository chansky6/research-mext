#!/usr/bin/env python3
# classify_tw.py LOG IMAP MAPS TRACE SKIP OPS
# LOG is what spd -F writes (see spd.c): demand faults, hints (bit 63),
# prefetch requests (bit 62) and marks (~0).  Only the records after the last
# mark are used, i.e. the measured window when mc_spd.sh ran the daemon; without
# a mark the whole log is, settle period included.
#
# Each demand fault is put in one class by the page it hit:
#   req      page holds an item requested in the measured window
#   cold     page holds only items not requested in the window
#   noitem   inside memcached's item regions but no mapped item (free chunk, new alloc, slab meta)
#   other    outside the item regions (hash table, stacks, libc, ...)
# IMAP is taken before the window: an item a SET moved during the window is
# still looked up at its old place.
#
# With hints in the log, each fault is also put in one class by what the daemon
# was told about that page since the page's previous fault:
#   nohint      no hint
#   resident    hinted, no prefetch asked: the page was in DRAM then and left before its use
#   prefetched  hinted and a prefetch asked: it was dropped, or the page left again
# "gap" is how many hint records the log holds between that hint and the fault.
# For nohint faults, "hint within N" counts those whose page is hinted in the
# next N hint records: the request was known, its hint reached the daemon late.
import sys, struct, collections
from array import array
import bisect

PG = 4096
HDR = 56
MARK = (1 << 64) - 1
HINT, PREF = 1 << 63, 1 << 62
LATE_N = 4096
log_p, imap_p, maps_p, trace_p, skip, ops = sys.argv[1:7]
skip, ops = int(skip), int(ops)

log = array('Q'); log.frombytes(open(log_p, 'rb').read())
start = 0
marks = 0
for i, a in enumerate(log):
    if a == MARK:
        start = i + 1
        marks += 1

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
moved = set()        # keys a request of the window wrote or deleted
gets = gets_moved = 0
for i in range(0, len(win), 12):
    k, _, _, op = struct.unpack_from('<IIHH', win, i)
    want.add(k)
    if op:
        moved.add(k)
    else:
        gets += 1
        gets_moved += k in moved

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

def page_class(p):
    if p in page_req:
        return 'req'
    if p in page_any:
        return 'cold'
    if any(s <= p < e for s, e in item_regs):
        return 'noitem'
    return 'other'

CLS = ('req', 'cold', 'noitem', 'other')
HCLS = ('nohint', 'resident', 'prefetched')
cls = collections.Counter(); first = collections.Counter(); seen = set()
other = collections.Counter()
by = collections.Counter()           # (page class, hint class)
gaps = collections.defaultdict(list) # hint class -> gaps
hint_at = {}                         # page -> hint clock of its last hint since its last fault
pref = set()                         # pages with a prefetch asked since their last hint
hint_clock = collections.defaultdict(list)   # page -> clocks of all its hints
nohint = []                          # (page, clock, page class)
clock = nf = npref = 0
for a in memoryview(log)[start:]:
    if a == MARK:
        continue
    if a & HINT:
        p = a & ~HINT & ~(PG - 1)
        clock += 1
        hint_at[p] = clock
        hint_clock[p].append(clock)
        pref.discard(p)
        continue
    if a & PREF:
        npref += 1
        pref.add(a & ~PREF & ~(PG - 1))
        continue
    p = a & ~(PG - 1)
    nf += 1
    c = page_class(p)
    if c == 'other':
        name, s = region(p)
        other[(name, hex(s))] += 1
    cls[c] += 1
    if p not in seen:
        first[c] += 1
        seen.add(p)
    h = hint_at.pop(p, None)
    if h is None:
        hc = 'nohint'
        nohint.append((p, clock, c))
    else:
        hc = 'prefetched' if p in pref else 'resident'
        gaps[hc].append(clock - h)
    pref.discard(p)
    by[(c, hc)] += 1

print('log records %d  marks %d  window records %d%s' % (len(log), marks, len(log) - start,
      '' if marks else '  (no mark: whole log, settle period included)'))
print('faults %d  distinct pages %d  window keys %d  items %d' % (nf, len(seen), len(want), n))
print('item pages %d (requested-in-window %d)' % (len(page_any), len(page_req)))
print('window GETs %d, of them on a key written earlier in the window %d (%.1f%%)' %
      (gets, gets_moved, 100 * gets_moved / max(1, gets)))
for c in CLS:
    print('%-7s %8d %5.1f%%  first-touch %8d  repeat %8d' % (c, cls[c], 100 * cls[c] / max(1, nf), first[c], cls[c] - first[c]))
for k, v in other.most_common(6):
    print('  other', k, v)
if clock:
    print('hints %d  distinct hinted pages %d  prefetches asked %d' % (clock, len(hint_clock), npref))
    print('%-7s %10s %10s %10s' % (('',) + HCLS))
    for c in CLS:
        print('%-7s %10d %10d %10d' % ((c,) + tuple(by[(c, h)] for h in HCLS)))
    for h in ('resident', 'prefetched'):
        g = sorted(gaps[h])
        if g:
            print('gap %-10s p10 %d  p50 %d  p90 %d  max %d hint records' %
                  (h, g[len(g) // 10], g[len(g) // 2], g[len(g) * 9 // 10], g[-1]))
    late = collections.Counter(); ever = collections.Counter()
    for p, t, c in nohint:
        hs = hint_clock.get(p)
        if not hs:
            continue
        ever[c] += 1
        i = bisect.bisect_right(hs, t)
        if i < len(hs) and hs[i] - t <= LATE_N:
            late[c] += 1
    for c in CLS:
        if by[(c, 'nohint')]:
            print('nohint %-7s page hinted at some time %8d  hint within %d %8d' % (c, ever[c], LATE_N, late[c]))
