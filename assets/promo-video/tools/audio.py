#!/usr/bin/env python3
"""The soundtrack: PSPDX's own tune and interface sounds, plus a launch-film
pulse (kick, off-beat hats, riser, impacts, whooshes) synthesised here.

  audio.py [--storyboard storyboard.json] [--app ../../app] [--work DIR]
           [--out out/soundtrack.wav]

Steps: compile score.c against the app's audio code (app/ is only read),
render the tune with the cues and struck notes the storyboard implies, then
mix the pulse under it with a side-chain pump and normalise with ffmpeg
loudnorm to the storyboard's master_lufs. Everything is generated; no
third-party samples. Exit status: 0 written, 1 failure.
"""
import argparse
import array
import json
import math
import os
import random
import subprocess
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RATE = 48000
NOTES = [64, 68, 71, 73, 76, 78, 80, 83]          # E major add9, over the tune's E


def build_score(app, work):
    stub = os.path.join(work, 'stub', 'util')
    os.makedirs(stub, exist_ok=True)
    with open(os.path.join(stub, 'version.h'), 'w') as f:
        f.write('#define PSPDX_VERSION "promo"\n')
    exe = os.path.join(work, 'score')
    src = [os.path.join(app, 'audio', n) for n in ('synth.c', 'music.c', 'cues.c')]
    subprocess.run(['cc', '-O2', '-I' + os.path.join(work, 'stub'), '-I' + app, *src,
                    os.path.join(HERE, 'score.c'), '-lm', '-o', exe], check=True)
    return exe


def events(sb):
    au = sb['audio']
    ev = [(0.0, 'level', au.get('tune_level', 0.9))]
    names = {'move': 0, 'open': 1, 'done': 2, 'fail': 3}
    for t, name in au.get('ui', []):
        ev.append((t, 'cue', names[name], 0))
    for a, b in au.get('scroll_ticks', []):
        t, row = a, 0
        while t < b:
            ev.append((t, 'cue', 0, row))
            row += 1
            rate = 6 + 22 * ((t - a) / (b - a)) ** 1.5
            t += 1 / rate
    if au.get('glass_on_cards'):
        k = 0
        for o in sb['overlays']:
            if o['kind'] in ('card', 'headline', 'wordmark'):
                pan = -.6 if o.get('side') == 'left' else .6 if o.get('side') == 'right' else 0
                ev.append((o['t'][0] + .04, 'strike', NOTES[k % len(NOTES)], .5, 1, pan))
                k += 1
    for t in au.get('impact', []):
        ev.append((t, 'strike', 28, .9, 4, 0))       # the tune's own sub, on E1
        ev.append((t, 'strike', 76, .35, 6, 0))      # and a bell
    ev.sort(key=lambda e: e[0])
    return ev


# ------------------------------------------------------------------ the pulse

def kick():
    n = int(.42 * RATE)
    out, ph = [], 0.0
    for i in range(n):
        t = i / RATE
        f = 44 + 120 * math.exp(-t / .035)
        ph += 2 * math.pi * f / RATE
        a = math.exp(-t / .16) * (1 - math.exp(-t / .002))
        out.append(math.sin(ph) * a * .95 + (random.random() - .5) * .25 * math.exp(-t / .004))
    return out


def hat():
    n = int(.07 * RATE)
    prev, out = 0.0, []
    for i in range(n):
        w = random.random() * 2 - 1
        out.append((w - prev) * .5 * math.exp(-i / (.018 * RATE)))
        prev = w
    return out


def noise_sweep(dur, f0, f1, peak_at, rng):
    n = int(dur * RATE)
    y, out = 0.0, []
    for i in range(n):
        p = i / n
        f = f0 * (f1 / f0) ** p
        a = 1 - math.exp(-2 * math.pi * f / RATE)
        y += a * ((rng.random() * 2 - 1) - y)
        e = (p / peak_at) ** 2 if p < peak_at else ((1 - p) / (1 - peak_at)) ** 1.5
        out.append(y * e)
    return out


def riser(dur):
    rng = random.Random(3)
    base = noise_sweep(dur, 300, 6000, .97, rng)
    ph = 0.0
    for i in range(len(base)):
        p = i / len(base)
        ph += 2 * math.pi * (180 * (8 ** p)) / RATE
        base[i] = base[i] * .8 + math.sin(ph) * .18 * p * p
    return base


def impact():
    n = int(1.6 * RATE)
    rng = random.Random(5)
    out, ph, y = [], 0.0, 0.0
    for i in range(n):
        t = i / RATE
        ph += 2 * math.pi * (38 + 40 * math.exp(-t / .08)) / RATE
        y += .08 * ((rng.random() * 2 - 1) - y)
        out.append(math.sin(ph) * math.exp(-t / .55) * .9 + y * math.exp(-t / .25) * 1.4)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--storyboard', default=os.path.join(ROOT, 'storyboard.json'))
    ap.add_argument('--app', default=os.path.normpath(os.path.join(ROOT, '..', '..', 'app')))
    ap.add_argument('--work', default=os.path.join(os.environ.get('TMPDIR', '/tmp'), 'pspdx-promo-work'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'soundtrack.wav'))
    a = ap.parse_args()
    random.seed(1)
    sb = json.load(open(a.storyboard))
    au, dur = sb['audio'], sb['duration']
    os.makedirs(a.work, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)

    exe = build_score(a.app, a.work)
    evfile = os.path.join(a.work, 'events.txt')
    with open(evfile, 'w') as f:
        for e in events(sb):
            f.write(' '.join(str(round(x, 4)) if isinstance(x, float) else str(x) for x in e) + '\n')
    tune = os.path.join(a.work, 'tune.wav')
    subprocess.run([exe, evfile, tune, str(dur + 1)], check=True)

    n = int(dur * RATE)
    L = array.array('f', bytes(4 * n))
    R = array.array('f', bytes(4 * n))
    duck = array.array('f', [1.0]) * n

    def add(sig, t, gain, pan=0.0):
        i0 = int(t * RATE)
        gl, gr = gain * math.sqrt((1 - pan) / 2) * 1.414, gain * math.sqrt((1 + pan) / 2) * 1.414
        for i, s in enumerate(sig):
            j = i0 + i
            if 0 <= j < n:
                L[j] += s * gl
                R[j] += s * gr

    beat = 60 / sb['bpm']
    k, h = kick(), hat()
    for a0, b0 in au.get('kick', []):
        t = a0
        while t < b0 - 1e-6:
            add(k, t, .55)
            i0 = int(t * RATE)
            for i in range(int(.3 * RATE)):
                if i0 + i < n:
                    duck[i0 + i] = min(duck[i0 + i], 1 - .45 * math.exp(-i / (.09 * RATE)))
            t += beat
    for a0, b0 in au.get('hats', []):
        t = a0 + beat / 2
        while t < b0:
            add(h, t, .16, .25)
            t += beat
    for a0, b0 in au.get('riser', []):
        add(riser(b0 - a0), a0, .35)
    imp = impact()
    for t in au.get('impact', []):
        add(imp, t, .6)
    if au.get('whoosh_on_overlays'):
        rng = random.Random(11)
        for o in sb['overlays']:
            if o['kind'] in ('card', 'headline', 'chip', 'wordmark', 'endcard'):
                pan = -.75 if o.get('side') == 'left' else .75 if o.get('side') == 'right' else 0
                w = noise_sweep(.5, 400, 5000, .55, rng)
                add(w, max(o["t"][0] - .12, 0), .08, pan)

    with wave.open(tune) as wv:
        raw = array.array('h', wv.readframes(n))
    out = array.array('h', bytes(4 * n))
    for i in range(n):
        tl = raw[2 * i] / 32768 * duck[i] if 2 * i + 1 < len(raw) else 0
        tr = raw[2 * i + 1] / 32768 * duck[i] if 2 * i + 1 < len(raw) else 0
        fade = min(1.0, (dur - i / RATE) / .8)
        out[2 * i] = int(max(-1, min(1, math.tanh((tl + L[i]) * 1.1) * fade)) * 32000)
        out[2 * i + 1] = int(max(-1, min(1, math.tanh((tr + R[i]) * 1.1) * fade)) * 32000)
    mix = os.path.join(a.work, 'mix.wav')
    with wave.open(mix, 'wb') as wv:
        wv.setnchannels(2)
        wv.setsampwidth(2)
        wv.setframerate(RATE)
        wv.writeframes(out.tobytes())
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', mix, '-af',
                    f'loudnorm=I={au.get("master_lufs", -14)}:TP=-1.0:LRA=9', '-ar', str(RATE), a.out], check=True)
    print(a.out)


if __name__ == '__main__':
    main()
