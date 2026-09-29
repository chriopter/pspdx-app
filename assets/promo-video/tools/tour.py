#!/usr/bin/env python3
"""The tour: a still product shot of the PSP, its screen replaced by footage
captured from the real console, and one calm line of text below saying what
is happening. Nothing else moves. See SKILL.md, "Principles".

  tour.py tour.json FOOTAGE.mkv OUT.mp4 [--preview]

tour.json names the plate (the product shot), where its screen is, the
kept stretches of the footage and the captions on the output timeline.
FOOTAGE.mkv is what footage.py makes of a psp_capture.py take (480x272).
Only ffmpeg does the work: overlay and drawtext, no frames through Python.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def esc(text):
    """drawtext's text= inside a filtergraph: escape \\ : ' and %."""
    return (text.replace('\\', '\\\\\\\\').replace(':', '\\:')
            .replace("'", "’").replace('%', '\\%'))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 3:
        sys.exit(__doc__)
    preview = '--preview' in sys.argv
    cfg_path, footage, out = args
    cfg = json.load(open(cfg_path))

    W, H = cfg['canvas']
    s = cfg['scale']
    cx, cy, cw, ch = cfg['plate_crop']        # the device, with a margin
    sx, sy, sw, sh = cfg['screen']            # the screen, in plate pixels
    pw, ph = round(cw * s), round(ch * s)
    px, py = (W - pw) // 2, cfg['plate_top']
    scr_x, scr_y = px + round((sx - cx) * s), py + round((sy - cy) * s)
    scr_w, scr_h = round(sw * s), round(sh * s)

    # The kept stretches of the take, played at the speed they were shot.
    parts, labels = [], []
    for i, (a, b) in enumerate(cfg['keep']):
        parts.append(f'[1:v]trim=start={a}:end={b},setpts=PTS-STARTPTS[k{i}]')
        labels.append(f'[k{i}]')
    total = sum(b - a for a, b in cfg['keep'])
    fade = cfg.get('fade', 0.5)

    g = parts
    g.append(f'{"".join(labels)}concat=n={len(labels)}:v=1:a=0,'
             f'scale={scr_w}:{scr_h}:flags=lanczos,format=rgb24[scr]')
    g.append(f'[0:v]crop={cw}:{ch}:{cx}:{cy},scale={pw}:{ph}:flags=lanczos,format=rgb24[plate]')
    g.append(f'color=c={cfg["background"]}:s={W}x{H}:r=60:d={total},format=rgb24[bg]')
    g.append(f'[bg][plate]overlay={px}:{py}[p]')
    g.append(f'[p][scr]overlay={scr_x}:{scr_y}:shortest=1[v0]')

    font = os.path.join(ROOT, cfg['font'])
    last = 'v0'
    for i, c in enumerate(cfg['captions']):
        t0, t1 = c['from'], c['to']
        alpha = (f"if(lt(t,{t0}+{fade}),(t-{t0})/{fade},"
                 f"if(gt(t,{t1}-{fade}),({t1}-t)/{fade},1))")
        size = c.get('size', cfg['size'])
        color = c.get('color', cfg['color'])
        g.append(f"[{last}]drawtext=fontfile='{font}':text='{esc(c['text'])}':"
                 f"fontsize={size}:fontcolor={color}:x=(w-text_w)/2:y={cfg['caption_y']}:"
                 f"alpha='{alpha}':enable='between(t,{t0},{t1})'[c{i}]")
        last = f'c{i}'
    if preview:
        g.append(f'[{last}]scale={W // 2}:{H // 2}[out]')
    else:
        g.append(f'[{last}]null[out]')

    plate = os.path.join(ROOT, cfg['plate'])
    cmd = ['ffmpeg', '-v', 'error', '-y', '-loop', '1', '-framerate', '60', '-i', plate,
           '-i', footage, '-filter_complex', ';'.join(g), '-map', '[out]',
           '-t', f'{total}', '-r', '60', '-c:v', 'libx264', '-crf', '18', '-preset', 'slow',
           '-pix_fmt', 'yuv420p', '-movflags', '+faststart', out]
    subprocess.run(cmd, check=True)
    print(out, f'{total:.1f} s')


if __name__ == '__main__':
    main()
