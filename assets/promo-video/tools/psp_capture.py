#!/usr/bin/env python3
"""Record the PSP's screen over USB while playing a scripted take on its pad.

One TCP connection to remotejoy (via usbhostfs_pc, 127.0.0.1:10004) does both
jobs, because usbhostfs_pc hands the async channel to one client at a time:

  PC -> PSP   JoyEvent {magic, type, value}   buttons, and TYPE_SCREEN_CMD
  PSP -> PC   JoyScrHeader {magic, mode, size, ref} + `size` bytes of pixels

`ref` is the PSP's own vblank counter (sceDisplayGetVcount, 59.94 Hz), so the
footage is timed by the console, not by USB jitter. Button marks are timed on
the host and mapped onto the vblank clock afterwards (footage.py).

Usage:
  psp_capture.py TAKE.take OUTDIR/NAME [--port 10004] [--full] [--half]
                 [--drop N] [--lead 1.0] [--tail 1.5] [--max 260]

Writes OUTDIR/NAME.raw (headers + pixels as received), NAME.frames.jsonl
(one line per frame: offset, size, mode, ref, host time) and NAME.marks.json
(host time of every `mark` in the take, plus the take itself).

Take language, one step per line, '#' starts a comment:
  mark NAME            note the time under NAME (clips in storyboard.json)
  wait SECS
  tap BTN [HOLD]       press and release (HOLD default 0.08 s)
  hold BTN SECS        keep BTN down for SECS
  taps BTN N [GAP]     N taps, GAP seconds apart (default 0.18)
  sh COMMAND...        start a host command and go on without waiting
                       (e.g. pspsh -e "ldstart ..." to launch the app on cue)
Buttons: up down left right cross circle square triangle start select
         ltrigger rtrigger

Exit status: 0 recorded, 1 connection or protocol failure, 2 bad take file.
Standard library only. Never sends anything but pad and screen commands.
"""
import argparse
import subprocess
import json
import os
import socket
import struct
import sys
import threading
import time

MAGIC = 0x909ACCEF
TYPE_DOWN, TYPE_UP, TYPE_SCREEN = 1, 2, 5
SCREEN_ACTIVE, SCREEN_HSIZE, SCREEN_FULLCOLOR = 1, 2, 4
BTN = {
    'select': 0x1, 'start': 0x8, 'up': 0x10, 'right': 0x20, 'down': 0x40,
    'left': 0x80, 'ltrigger': 0x100, 'rtrigger': 0x200, 'triangle': 0x1000,
    'circle': 0x2000, 'cross': 0x4000, 'square': 0x8000,
}
HEADER = struct.Struct('<IiiI')          # magic, mode, size, ref (vcount)
MAX_FRAME = 480 * 272 * 4


def parse_take(path):
    steps = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            words = line.split('#', 1)[0].split()
            if not words:
                continue
            verb, args = words[0], words[1:]
            try:
                if verb == 'sh' and args:
                    steps.append(('sh', line.split('#', 1)[0].strip()[2:].strip()))
                elif verb == 'mark' and len(args) == 1:
                    steps.append(('mark', args[0]))
                elif verb == 'wait' and len(args) == 1:
                    steps.append(('wait', float(args[0])))
                elif verb == 'tap' and len(args) in (1, 2) and args[0] in BTN:
                    steps.append(('tap', args[0], float(args[1]) if len(args) > 1 else 0.08))
                elif verb == 'hold' and len(args) == 2 and args[0] in BTN:
                    steps.append(('hold', args[0], float(args[1])))
                elif verb == 'taps' and len(args) in (2, 3) and args[0] in BTN:
                    steps.append(('taps', args[0], int(args[1]),
                                  float(args[2]) if len(args) > 2 else 0.18))
                else:
                    raise ValueError
            except ValueError:
                sys.exit(f'{path}:{n}: cannot read "{line.strip()}"')
    return steps


def take_seconds(steps):
    t = 0.0
    for s in steps:
        if s[0] == 'wait':
            t += s[1]
        elif s[0] == 'tap':
            t += s[2] + 0.12
        elif s[0] == 'hold':
            t += s[2] + 0.12
        elif s[0] == 'taps':
            t += s[2] * (0.08 + s[3])
    return t


class Link:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.s.settimeout(None)
        self.lock = threading.Lock()

    def send(self, typ, value):
        with self.lock:
            self.s.sendall(struct.pack('<IiI', MAGIC, typ, value & 0xFFFFFFFF))

    def down(self, b):
        self.send(TYPE_DOWN, BTN[b])

    def up(self, b):
        self.send(TYPE_UP, BTN[b])

    def release_all(self):
        self.send(TYPE_UP, 0xFFFFFFFF)


def reader(link, raw, index, stop, stats, screen_flags):
    """Frames off the socket into the raw file; resyncs on a bad magic."""
    buf = bytearray()
    sock = link.s
    sock.settimeout(0.5)
    offset = 0
    while not stop.is_set():
        try:
            chunk = sock.recv(1 << 20)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            stats['eof'] = True
            break
        buf += chunk
        while len(buf) >= HEADER.size:
            magic, mode, size, ref = HEADER.unpack_from(buf)
            if magic != MAGIC:
                at = buf.find(struct.pack('<I', MAGIC), 1)
                stats['resync'] += 1
                del buf[:at if at > 0 else len(buf) - 3]
                continue
            if mode < 0:
                # The PSP says the screen went off (remotejoy's own -1): ask again.
                del buf[:HEADER.size]
                link.send(TYPE_SCREEN, screen_flags)
                stats['reenable'] += 1
                continue
            if mode > 3 or size <= 0 or size > MAX_FRAME:
                del buf[:4]
                stats['resync'] += 1
                continue
            if len(buf) < HEADER.size + size:
                break
            now = time.monotonic()
            raw.write(buf[:HEADER.size + size])
            index.write(json.dumps({'off': offset, 'size': size, 'mode': mode,
                                    'ref': ref, 'host': round(now, 6)}) + '\n')
            offset += HEADER.size + size
            del buf[:HEADER.size + size]
            stats['frames'] += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('take')
    ap.add_argument('out', help='OUTDIR/NAME, no extension')
    ap.add_argument('--port', type=int, default=10004)
    ap.add_argument('--full', action='store_true', help='32-bit frames (522 KB each) instead of 16-bit')
    ap.add_argument('--half', action='store_true', help='240x136 frames (probe only)')
    ap.add_argument('--drop', type=int, default=0, help='skip N vblanks between frames (0-59)')
    ap.add_argument('--lead', type=float, default=1.0, help='seconds of screen before the take')
    ap.add_argument('--tail', type=float, default=1.5, help='seconds of screen after the take')
    ap.add_argument('--max', type=float, default=260.0,
                    help='refuse takes longer than this many seconds (bench WLAN timer)')
    a = ap.parse_args()

    steps = parse_take(a.take)
    length = take_seconds(steps) + a.lead + a.tail
    if length > a.max:
        print(f'take runs {length:.0f} s, over --max {a.max:.0f} s', file=sys.stderr)
        sys.exit(2)

    flags = SCREEN_ACTIVE | (SCREEN_FULLCOLOR if a.full else 0) | \
        (SCREEN_HSIZE if a.half else 0) | ((a.drop & 0xFF) << 24)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    try:
        link = Link(a.port)
    except OSError as e:
        print(f'no remotejoy on 127.0.0.1:{a.port}: {e}', file=sys.stderr)
        sys.exit(1)

    marks = {}
    stats = {'frames': 0, 'resync': 0, 'reenable': 0, 'eof': False}
    stop = threading.Event()
    with open(a.out + '.raw', 'wb') as raw, open(a.out + '.frames.jsonl', 'w') as index:
        t = threading.Thread(target=reader, args=(link, raw, index, stop, stats, flags), daemon=True)
        t.start()
        held = set()
        try:
            link.release_all()
            link.send(TYPE_SCREEN, flags)
            marks['_screen_on'] = time.monotonic()
            time.sleep(a.lead)
            for s in steps:
                if s[0] == 'mark':
                    marks[s[1]] = time.monotonic()
                    print(f'{marks[s[1]] - marks["_screen_on"]:7.2f}  mark {s[1]}  ({stats["frames"]} frames)', flush=True)
                elif s[0] == 'wait':
                    time.sleep(s[1])
                elif s[0] == 'sh':
                    subprocess.Popen(os.path.expanduser(s[1]), shell=True,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                elif s[0] == 'tap':
                    link.down(s[1]); held.add(s[1]); time.sleep(s[2])
                    link.up(s[1]); held.discard(s[1]); time.sleep(0.12)
                elif s[0] == 'hold':
                    link.down(s[1]); held.add(s[1]); time.sleep(s[2])
                    link.up(s[1]); held.discard(s[1]); time.sleep(0.12)
                elif s[0] == 'taps':
                    for _ in range(s[2]):
                        link.down(s[1]); time.sleep(0.08)
                        link.up(s[1]); time.sleep(s[3])
                if stats['eof']:
                    print('socket closed by the PSP side', file=sys.stderr)
                    break
            time.sleep(a.tail)
            marks['_end'] = time.monotonic()
        except KeyboardInterrupt:
            print('interrupted, releasing the pad', file=sys.stderr)
        finally:
            try:
                for b in held:
                    link.up(b)
                link.release_all()
                link.send(TYPE_SCREEN, 0)          # screen streaming off
            except OSError:
                pass
            time.sleep(0.3)
            stop.set()
            t.join(2)
            link.s.close()

    with open(a.out + '.marks.json', 'w') as f:
        json.dump({'marks': marks, 'take': os.path.abspath(a.take), 'flags': flags,
                   'stats': stats}, f, indent=1)
    secs = marks.get('_end', time.monotonic()) - marks['_screen_on']
    print(f'{stats["frames"]} frames in {secs:.1f} s = {stats["frames"] / max(secs, 1e-3):.1f} fps'
          f'  (resync {stats["resync"]}, re-enabled {stats["reenable"]})')
    sys.exit(0 if stats['frames'] else 1)


if __name__ == '__main__':
    main()
