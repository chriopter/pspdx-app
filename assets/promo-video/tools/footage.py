#!/usr/bin/env python3
"""Turn a capture, or the placeholder stills, into footage the renderer reads.

  footage.py capture CAPTURE_PREFIX OUT_DIR/NAME
      CAPTURE_PREFIX.raw/.frames.jsonl/.marks.json from psp_capture.py
      -> NAME.mkv (480x272, 60 fps constant, lossless FFV1)
      -> NAME.marks.json {"marks": {mark: seconds into NAME.mkv}, ...}
  footage.py placeholder STORYBOARD OUT_DIR
      the storyboard's "placeholder" section: per take, per mark, a list of
      [still, seconds] -> the same two files per take, flagged placeholder

Frames are timed by the PSP's vblank counter (59.94 Hz); every output frame
at 60 fps shows the newest captured frame at or before it, so dropped or late
frames repeat the last one instead of shifting time. Marks are host times;
the host clock is tied to the vblank clock by the frame that arrived
soonest after its vblank (the least-delayed one), plus one frame of
pad-to-screen latency.

Exit status: 0 written, 1 bad input. Needs PIL and ffmpeg.
"""
import json
import os
import subprocess
import sys

from PIL import Image

PW, PH = 480, 272
VBLANK = 59.94
RAWMODE = {3: ('RGBA', 4), 0: ('RGB;16', 2), 1: ('RGB;15', 2), 2: ('RGB;4B', 2)}


def decode(buf, mode, size):
    """One remotejoy frame -> 480x272 RGB image (half-size frames are scaled up)."""
    raw, bpp = RAWMODE[mode]
    n = size // bpp
    w, h = (PW, PH) if n == PW * PH else (PW // 2, PH // 2)
    if raw == 'RGBA':
        im = Image.frombytes('RGBA', (w, h), bytes(buf)).convert('RGB')
    else:
        im = Image.frombytes('RGB', (w, h), bytes(buf), 'raw', raw)
    return im if w == PW else im.resize((PW, PH), Image.NEAREST)


def encoder(path):
    return subprocess.Popen(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                             '-s', f'{PW}x{PH}', '-r', '60', '-i', '-', '-c:v', 'ffv1', '-level', '3',
                             '-pix_fmt', 'bgr0', path], stdin=subprocess.PIPE)


def from_capture(prefix, out):
    frames = [json.loads(line) for line in open(prefix + '.frames.jsonl')]
    meta = json.load(open(prefix + '.marks.json'))
    if not frames:
        sys.exit('no frames in the capture')
    ref0 = frames[0]['ref']
    # host = ref/VBLANK + c; c from the least-delayed frame
    c = min(f['host'] - (f['ref'] - ref0) / VBLANK for f in frames)
    latency = 1 / VBLANK
    times = [(f['ref'] - ref0) / VBLANK for f in frames]
    end = times[-1]
    raw = open(prefix + '.raw', 'rb')
    p = encoder(out + '.mkv')
    i, last, n = 0, None, int(end * 60) + 1
    for k in range(n):
        t = k / 60
        moved = False
        while i + 1 < len(frames) and times[i + 1] <= t:
            i += 1
            moved = True
        if last is None or moved or k == 0:
            f = frames[i]
            raw.seek(f['off'] + 16)
            last = decode(raw.read(f['size']), f['mode'], f['size']).tobytes()
        p.stdin.write(last)
    p.stdin.close()
    p.wait()
    marks = {name: round(h - c + latency, 4) for name, h in meta['marks'].items() if not name.startswith('_')}
    intervals = [b - a for a, b in zip(times, times[1:])]
    fps = len(frames) / max(end, 1e-6)
    json.dump({'marks': marks, 'placeholder': False, 'source': os.path.abspath(prefix),
               'captured_fps': round(fps, 2), 'longest_gap_s': round(max(intervals or [0]), 3),
               'seconds': round(end, 2)}, open(out + '.marks.json', 'w'), indent=1)
    print(f'{out}.mkv: {end:.1f} s, captured {fps:.1f} fps, longest gap {max(intervals or [0]) * 1000:.0f} ms')
    for name, s in sorted(marks.items(), key=lambda kv: kv[1]):
        print(f'  {s:7.2f}  {name}')


def placeholder(storyboard, outdir):
    sb = json.load(open(storyboard))
    ph = sb['placeholder']
    root = os.path.dirname(os.path.abspath(storyboard))
    stills = os.path.join(root, ph['stills'])
    os.makedirs(outdir, exist_ok=True)
    for take, clips in ph['takes'].items():
        p = encoder(os.path.join(outdir, take + '.mkv'))
        marks, t = {}, 0.0
        prev = None
        for mark, seq in clips.items():
            marks[mark] = round(t, 4)
            for item in seq:
                name, secs = item[0], item[1]
                fade = item[2] if len(item) > 2 else ph.get('crossfade', 0.12)
                im = Image.open(os.path.join(stills, name)).convert('RGB').resize((PW, PH))
                n = max(1, round(secs * 60))
                for k in range(n):
                    if prev is not None and fade > 0 and k < fade * 60:
                        p.stdin.write(Image.blend(prev, im, (k + 1) / (fade * 60 + 1)).tobytes())
                    else:
                        p.stdin.write(im.tobytes())
                prev = im
                t += n / 60
        p.stdin.close()
        p.wait()
        json.dump({'marks': marks, 'placeholder': True, 'seconds': round(t, 2)},
                  open(os.path.join(outdir, take + '.marks.json'), 'w'), indent=1)
        print(f'{take}: {t:.1f} s of placeholder footage, marks {", ".join(marks)}')


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'capture':
        from_capture(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == 'placeholder':
        placeholder(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
