#!/usr/bin/env python3
"""page_idle 受控实验:标记两页 idle,一页不再触碰,一页持续触碰,5s 后读位验证。"""
import ctypes
import os
import struct
import time

PAGE = 4096

def pfn_of(pid, va):
    fd = os.open("/proc/%d/pagemap" % pid, os.O_RDONLY)
    e = os.pread(fd, 8, va // PAGE * 8)
    os.close(fd)
    v = struct.unpack("<Q", e)[0]
    if v & (1 << 63):
        return v & ((1 << 55) - 1)
    return None

bm = open("/sys/kernel/mm/page_idle/bitmap", "rb+")

def set_idle(pfn):
    bm.seek(pfn // 64 * 8)
    v = struct.unpack("<Q", bm.read(8))[0] | (1 << (pfn % 64))
    bm.seek(pfn // 64 * 8)
    bm.write(struct.pack("<Q", v))

def get_idle(pfn):
    bm.seek(pfn // 64 * 8)
    return (struct.unpack("<Q", bm.read(8))[0] >> (pfn % 64)) & 1

libc = ctypes.CDLL(None)
libc.mmap.restype = ctypes.c_void_p
sz = 2 * PAGE
addr = libc.mmap(None, sz, 3, 0x02 | 0x20, -1, 0)  # PROT_RW | MAP_PRIVATE|MAP_ANON
if not addr:
    raise OSError("mmap failed")

ctypes.memset(ctypes.c_void_p(addr), 1, 1)          # 触碰 quiet 页
ctypes.memset(ctypes.c_void_p(addr + PAGE), 1, 1)   # 触碰 busy 页

pid = os.getpid()
p_quiet = pfn_of(pid, addr)
p_busy = pfn_of(pid, addr + PAGE)
print("PFN quiet=%s busy=%s" % (p_quiet, p_busy))
if not (p_quiet and p_busy):
    raise SystemExit("PFN not visible")

set_idle(p_quiet)
set_idle(p_busy)
print("marked idle; touching busy page for 5s, quiet untouched")
t0 = time.time()
n = 0
while time.time() - t0 < 5:
    ctypes.memset(ctypes.c_void_p(addr + PAGE), n & 0xFF, 1)  # 写 busy 页(置 A 位)
    n += 1
    time.sleep(0.2)

print("after 5s: quiet=%d (expect 1) busy=%d (expect 0)"
      % (get_idle(p_quiet), get_idle(p_busy)))
