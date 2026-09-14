#!/usr/bin/env python3
# Writes a stick trace for the entropy screen: { u8 lx, u8 ly, u16 buttons }
# per frame, as dev/rig --sweep replays it. The scripted hand makes clear,
# sustained turns at unpredictable headings and times, with small analog
# jitter, while keeping the source away from the walls. It leaves enough
# turns for the direction and timing accounting to fill the bar, then START.
#
#   python3 dev/sweep-trace.py dev/testdata/sweep.trace
#
# A replayed trace is public input, so this is a development aid and the
# client refuses it a seed; it only has to get the screen over with quickly.
import math
import secrets
import struct
import sys

STEP_X = 0.0065 / 60.0
STEP_Z = 0.0040 / 28.0
START = 0x0008
TURNS = 64
MARGIN = 0.10


rnd = secrets.SystemRandom()
x = z = 0.5
last_heading = None
frames = []
for _ in range(TURNS):
    while True:
        # Most holds stay quick, while regular long ones spread turns across
        # the timing classes instead of teaching a single run length.
        hold = rnd.randint(15, 40) if rnd.randrange(6) == 0 else rnd.randint(15, 25)
        heading = rnd.randrange(8) if last_heading is None else (
            last_heading + rnd.choice((-3, -2, -1, 1, 2, 3))
        ) % 8
        angle = heading * math.pi / 4 + rnd.uniform(-0.12, 0.12)
        magnitude = rnd.randint(112, 127)
        dx = int(round(magnitude * math.cos(angle)))
        dy = int(round(magnitude * math.sin(angle)))
        run = []
        nx, nz = x, z
        for _ in range(hold):
            jx = max(-128, min(127, dx + rnd.randint(-2, 2)))
            jy = max(-128, min(127, dy + rnd.randint(-2, 2)))
            nx += jx * STEP_X
            nz -= jy * STEP_Z
            run.append((jx + 128, jy + 128, 0))
        if MARGIN < nx < 1.0 - MARGIN and MARGIN < nz < 1.0 - MARGIN:
            break
    last_heading = heading
    frames.extend(run)
    x, z = nx, nz
for _ in range(6):
    frames.append((128, 128, 0))
for _ in range(4):
    frames.append((128, 128, START))

out = sys.argv[1] if len(sys.argv) > 1 else "sweep.trace"
with open(out, "wb") as f:
    for lx, ly, b in frames:
        f.write(struct.pack("<BBH", lx, ly, b))
print(f"{out}: {len(frames)} frames, {TURNS} scripted turns, {len(frames) / 60:.1f} s at 60 fps")
