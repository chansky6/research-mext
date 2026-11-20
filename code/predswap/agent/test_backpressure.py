#!/usr/bin/env python3
"""Linux: a full daemon feedback queue must not block HELLO or the WILLNEED worker."""
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time


def fill_queue(path):
    queued = 0
    # Use a fresh sender for each datagram. A single sender can exhaust its own
    # SO_SNDBUF before this receiver's net.unix.max_dgram_qlen is reached.
    while True:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as filler:
            try:
                filler.sendto(bytes(32), socket.MSG_DONTWAIT, path)
                queued += 1
            except BlockingIOError:
                return queued


def check(root):
    (root / 'agents').mkdir()
    lib = root / 'agent.so'
    subprocess.run(['cc', '-O2', '-fPIC', '-shared', '-pthread', f'-DPS_DIR="{root}"',
                    str(Path(__file__).with_name('predswap_agent.c')), '-o', lib], check=True)
    daemon_path = str(root / 'agent.sock')
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as daemon:
        daemon.bind(daemon_path)
        queued = fill_queue(daemon_path)
        probe = ('import ctypes,mmap,sys; m=mmap.mmap(-1,4096); '
                 'print(ctypes.addressof(ctypes.c_char.from_buffer(m)),flush=True); sys.stdin.read(1)')
        with subprocess.Popen([sys.executable, '-c', probe], env={**os.environ, 'LD_PRELOAD': str(lib)},
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE) as child:
            try:
                assert select.select([child.stdout], [], [], 3)[0], 'HELLO stalled target startup'
                addr = int(child.stdout.readline())
                target = str(root / 'agents' / f'a-{child.pid}.sock')
                req = struct.pack('<IIQQ', 2, 1, addr, 4096)
                daemon.settimeout(3)
                for _ in range(queued):
                    daemon.recv(32)
                daemon.sendto(req, target)
                frame = daemon.recv(64)
                if frame == struct.pack('<II', 1, child.pid):
                    frame = daemon.recv(64)  # HELLO and ACK share the daemon socket.
                assert len(frame) == 32, ('unexpected feedback frame', frame)
                ack = struct.unpack('<IIQQiI', frame)
                assert ack == (3, child.pid, 4096, 4096, 0, 0), ack
                fill_queue(daemon_path)
                daemon.setblocking(False)
                depth = int(Path('/proc/sys/net/unix/max_dgram_qlen').read_text())
                deadline = time.monotonic() + 3
                # Exceed the REQ queue depth: without a draining worker, sendto
                # cannot complete this burst. Leave the feedback queue untouched.
                for _ in range(depth + 4):
                    while True:
                        if time.monotonic() >= deadline:
                            raise TimeoutError('REQ worker stalled while feedback queue was full')
                        try:
                            daemon.sendto(req, socket.MSG_DONTWAIT, target)
                            break
                        except BlockingIOError:
                            time.sleep(0.001)
            finally:
                child.kill()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(dir='/tmp', prefix='ps-ack-') as tmp:
        check(Path(tmp))
