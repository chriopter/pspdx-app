#!/usr/bin/env python3
"""A stand-in for remotejoy + usbhostfs_pc, for dry-running psp_capture.py
and footage.py on the desk with no PSP attached.

  fake_remotejoy.py STILLS_DIR [--port 10044] [--fps 30] [--mode 0|3]

Listens on 127.0.0.1:PORT like usbhostfs_pc's async channel. After a
TYPE_SCREEN_CMD with ACTIVE it streams JoyScrHeader + pixels, the vblank
counter advancing like a 59.94 Hz display; every button press shows the next
still (sorted by name). Mode 0 sends PSP RGB565 (R in the low bits), 3 sends
RGBA8888, the two formats remotejoy.prx sends. Serves one client, then exits.
Exit status: 0 served, 1 bad arguments.
"""
import argparse
import os
import socket
import struct
import threading
import time

from PIL import Image

MAGIC = 0x909ACCEF


def pack565(im):
    out = bytearray()
    for r, g, b in im.getdata():
        v = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)
        out += struct.pack('<H', v)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stills')
    ap.add_argument('--port', type=int, default=10044)
    ap.add_argument('--fps', type=float, default=30)
    ap.add_argument('--mode', type=int, default=0, choices=(0, 3))
    a = ap.parse_args()
    names = sorted(n for n in os.listdir(a.stills) if n.endswith('.png'))
    frames = []
    for n in names[:12]:
        im = Image.open(os.path.join(a.stills, n)).convert('RGB').resize((480, 272))
        frames.append(pack565(im) if a.mode == 0 else im.convert('RGBA').tobytes())
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', a.port))
    srv.listen(1)
    print(f'fake remotejoy on 127.0.0.1:{a.port}, {len(frames)} stills', flush=True)
    conn, _ = srv.accept()
    state = {'on': False, 'shown': 0, 'quit': False}

    def rx():
        buf = b''
        while True:
            try:
                d = conn.recv(4096)
            except OSError:
                break
            if not d:
                break
            buf += d
            while len(buf) >= 12:
                magic, typ, val = struct.unpack_from('<IiI', buf)
                buf = buf[12:]
                if magic != MAGIC:
                    continue
                if typ == 5:
                    state['on'] = bool(val & 1)
                elif typ == 1:
                    state['shown'] = (state['shown'] + 1) % len(frames)
        state['quit'] = True

    threading.Thread(target=rx, daemon=True).start()
    t0 = time.monotonic()
    while not state['quit']:
        time.sleep(1 / a.fps)
        if state['on']:
            ref = int((time.monotonic() - t0) * 59.94)
            px = frames[state['shown']]
            try:
                conn.sendall(struct.pack('<IiiI', MAGIC, a.mode, len(px), ref) + px)
            except OSError:
                break
    conn.close()


if __name__ == '__main__':
    main()
