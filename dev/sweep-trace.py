#!/usr/bin/env python3
# Writes a stick trace for the entropy screen: { u8 lx, u8 ly, u16 buttons }
# per frame, as dev/rig --sweep replays it. The hand is /dev/urandom:
# a heading and a deflection drawn by lot, held for a frame or a few with the
# stick trembling, until the screen's own rule -- a bit for new ground
# reached under a new heading, eight headings -- has paid 128 and a few
# over for the client's float arithmetic, then X.
#
#   python3 dev/sweep-trace.py dev/testdata/sweep.trace
#
# A replayed trace is public input, so this is a development aid and the
# client refuses it a seed; it only has to get the screen over with quickly.
import math
import secrets
import struct
import sys

SIDE = 250
STEP_X = 0.0065 / 60.0
STEP_Z = 0.0040 / 28.0
BITS = 128
CROSS = 0x4000


def heading_of(dx, dy):
    ax, ay = abs(dx), abs(dy)
    if ay * 12 < ax * 5:
        return 0 if dx > 0 else 4
    if ax * 12 < ay * 5:
        return 2 if dy > 0 else 6
    if dx > 0:
        return 1 if dy > 0 else 7
    return 3 if dy > 0 else 5


rnd = secrets.SystemRandom()
fx = fz = 0.5
seen = set()
last = -1
bits = 0
frames = []
while bits < BITS + 4:
    angle = rnd.uniform(0, 2 * math.pi)
    mag = rnd.randint(70, 127)
    for _ in range(rnd.randint(1, 4)):
        dx = int(round(mag * math.cos(angle))) + rnd.randint(-5, 5)
        dy = int(round(mag * math.sin(angle))) + rnd.randint(-5, 5)
        dx = max(-128, min(127, dx))
        dy = max(-128, min(127, dy))
        frames.append((dx + 128, dy + 128, 0))
        if dx * dx + dy * dy <= 14 * 14:
            continue
        fx = min(1.0, max(0.0, fx + dx * STEP_X))
        fz = min(1.0, max(0.0, fz - dy * STEP_Z))
        field = int(fz * (SIDE - 1) + 0.5) * SIDE + int(fx * (SIDE - 1) + 0.5)
        if field in seen:
            continue
        seen.add(field)
        h = heading_of(dx, dy)
        if h != last:
            last = h
            bits += 1
for _ in range(6):
    frames.append((128, 128, 0))
for _ in range(4):
    frames.append((128, 128, CROSS))

out = sys.argv[1] if len(sys.argv) > 1 else "sweep.trace"
with open(out, "wb") as f:
    for lx, ly, b in frames:
        f.write(struct.pack("<BBH", lx, ly, b))
print(f"{out}: {len(frames)} frames, {bits} bits, {len(frames) / 60:.1f} s at 60 fps")
