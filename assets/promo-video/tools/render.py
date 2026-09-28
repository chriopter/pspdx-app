#!/usr/bin/env python3
"""Render the PSPDX promo: PSP footage + the room + kinetic overlays -> MP4.

  render.py [--storyboard storyboard.json] [--footage DIR] [--work DIR]
            [--out out/pspdx-promo.mp4] [--audio out/soundtrack.wav]
            [--preview] [--from S] [--to S] [--still T[,T...]] [--jobs N]

--footage DIR holds <take>.mkv + <take>.marks.json per take (footage.py makes
them, from a capture or from the placeholder stills). --preview renders
960x540 at 30 fps. --still writes PNGs of single moments instead of a video,
for judging a composition without a render. Everything is drawn in a
1920x1080 design space; the storyboard's numbers are in that space.

Needs: python3 with PIL, pycairo and PyGObject (Pango, PangoCairo, Rsvg),
ffmpeg. Exit status: 0 rendered, 1 failure.
"""
import argparse
import json
import math
import multiprocessing as mp
import os
import random
import shutil
import subprocess
import sys

import cairo
import gi
gi.require_version('Pango', '1.0')
gi.require_version('PangoCairo', '1.0')
gi.require_version('Rsvg', '2.0')
from gi.repository import Pango, PangoCairo, Rsvg  # noqa: E402
from PIL import Image, ImageChops, ImageDraw, ImageFilter  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PW, PH = 480, 272                      # PSP screen
DW, DH = 1920, 1080                    # design space

# ------------------------------------------------------------------ helpers

def clamp(x, a=0.0, b=1.0):
    return a if x < a else b if x > b else x


def lerp(a, b, t):
    return a + (b - a) * t


EASE = {
    'linear': lambda p: p,
    'in': lambda p: p * p * p,
    'out': lambda p: 1 - (1 - p) ** 3,
    'inout': lambda p: 4 * p * p * p if p < .5 else 1 - (-2 * p + 2) ** 3 / 2,
    'expo_out': lambda p: 1.0 if p >= 1 else 1 - 2 ** (-10 * p),
    'expo_in': lambda p: 0.0 if p <= 0 else 2 ** (10 * p - 10),
    'expo_inout': lambda p: 0.0 if p <= 0 else 1.0 if p >= 1 else
    (2 ** (20 * p - 10) / 2 if p < .5 else (2 - 2 ** (-20 * p + 10)) / 2),
    'back_out': lambda p: 1 + 2.70158 * (p - 1) ** 3 + 1.70158 * (p - 1) ** 2,
}


def hexrgb(h, a=1.0):
    h = h.lstrip('#')
    return (int(h[0:2], 16) / 255, int(h[2:4], 16) / 255, int(h[4:6], 16) / 255, a)


def keyed(keys, t, fields):
    """Interpolate keyframes [{t, field.., ease}] at time t."""
    def v(k, f):
        return hexrgb(k[f]) if isinstance(k[f], str) else k[f]
    if t <= keys[0]['t']:
        return {f: v(keys[0], f) for f in fields}
    for a, b in zip(keys, keys[1:]):
        if t < b['t']:
            p = EASE[b.get('ease', 'inout')]((t - a['t']) / max(b['t'] - a['t'], 1e-6))
            out = {}
            for f in fields:
                va, vb = a[f], b[f]
                if isinstance(va, str):
                    ca, cb = hexrgb(va), hexrgb(vb)
                    out[f] = tuple(lerp(x, y, p) for x, y in zip(ca, cb))
                elif f == 's':
                    out[f] = math.exp(lerp(math.log(va), math.log(vb), p))
                else:
                    out[f] = lerp(va, vb, p)
            return out
    return {f: v(keys[-1], f) for f in fields}


def cairo_to_pil(surface, mode='RGBA'):
    w, h = surface.get_width(), surface.get_height()
    surface.flush()
    data = bytes(surface.get_data())
    if mode == 'RGB':
        return Image.frombuffer('RGB', (w, h), data, 'raw', 'BGRX', surface.get_stride(), 1)
    if mode == 'PREMUL':                   # premultiplied colour, for additive glow
        return Image.frombuffer('RGB', (w, h), data, 'raw', 'BGRX', surface.get_stride(), 1)
    return Image.frombuffer('RGBA', (w, h), data, 'raw', 'BGRa', surface.get_stride(), 1)


def rounded(ctx, x, y, w, h, r):
    r = min(r, w / 2, h / 2)
    ctx.new_sub_path()
    ctx.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    ctx.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    ctx.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    ctx.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
    ctx.close_path()


# ------------------------------------------------------------------ text

WEIGHTS = {'Black': Pango.Weight.HEAVY, 'ExtraBold': Pango.Weight.ULTRABOLD,
           'Bold': Pango.Weight.BOLD, 'SemiBold': Pango.Weight.SEMIBOLD,
           'Medium': Pango.Weight.MEDIUM, 'Regular': Pango.Weight.NORMAL}


class Type:
    def __init__(self, tokens):
        fm = PangoCairo.FontMap.get_default()
        for f in tokens['font']['files']:
            fm.add_font_file(os.path.join(ROOT, f))
        self.display = tokens['font']['display']
        self.text = tokens['font']['text']
        self.cache = {}

    def layout(self, ctx, s, px, weight='Regular', display=False, tracking=0.0, width=None,
               align='left', spacing=0.0):
        lay = PangoCairo.create_layout(ctx)
        fo = cairo.FontOptions()
        fo.set_hint_style(cairo.HINT_STYLE_NONE)
        fo.set_hint_metrics(cairo.HINT_METRICS_OFF)
        fo.set_antialias(cairo.ANTIALIAS_GRAY)
        PangoCairo.context_set_font_options(lay.get_context(), fo)
        d = Pango.FontDescription()
        d.set_family(self.display if display else self.text)
        d.set_weight(WEIGHTS[weight])
        d.set_absolute_size(px * Pango.SCALE)
        lay.set_font_description(d)
        attrs = Pango.AttrList()
        if tracking:
            attrs.insert(Pango.attr_letter_spacing_new(int(tracking * px * Pango.SCALE)))
        lay.set_attributes(attrs)
        if width:
            lay.set_width(int(width * Pango.SCALE))
            lay.set_wrap(Pango.WrapMode.WORD)
        if spacing:
            lay.set_line_spacing(spacing)
        lay.set_alignment({'left': Pango.Alignment.LEFT, 'center': Pango.Alignment.CENTER,
                           'right': Pango.Alignment.RIGHT}[align])
        lay.set_text(s, -1)
        return lay

    @staticmethod
    def size(lay):
        ink, log = lay.get_pixel_extents()
        return log.width, log.height


# ------------------------------------------------------------------ glyphs

class Glyphs:
    def __init__(self, folder):
        self.folder = folder
        self.handles = {}

    def draw(self, ctx, name, x, y, size, rgba):
        if name not in self.handles:
            path = os.path.join(self.folder, name + '.svg')
            self.handles[name] = Rsvg.Handle.new_from_file(path) if os.path.exists(path) else None
        h = self.handles[name]
        if not h:
            return
        ctx.save()
        ctx.push_group()
        vp = Rsvg.Rectangle()
        vp.x, vp.y, vp.width, vp.height = x, y, size, size
        h.render_document(ctx, vp)
        pat = ctx.pop_group()
        ctx.set_source_rgba(*rgba)
        ctx.mask(pat)
        ctx.restore()


# ------------------------------------------------------------------ footage

class Footage:
    def __init__(self, work, takes):
        self.files, self.count, self.marks, self.placeholder = {}, {}, {}, {}
        for name, info in takes.items():
            self.files[name] = info['rgb']
            self.count[name] = os.path.getsize(info['rgb']) // (PW * PH * 3)
            self.marks[name] = info['marks']
            self.placeholder[name] = info['placeholder']
        self.handles = {}

    def frame(self, take, t):
        if take not in self.handles:
            self.handles[take] = open(self.files[take], 'rb')
        i = int(clamp(round(t * 60), 0, self.count[take] - 1))
        f = self.handles[take]
        f.seek(i * PW * PH * 3)
        return Image.frombytes('RGB', (PW, PH), f.read(PW * PH * 3))


def decode_takes(footage_dir, work, names):
    out = {}
    for name in names:
        mkv = os.path.join(footage_dir, name + '.mkv')
        mj = os.path.join(footage_dir, name + '.marks.json')
        rgb = os.path.join(work, name + '.rgb')
        if not os.path.exists(mkv):
            sys.exit(f'missing footage {mkv}: run footage.py first')
        if not os.path.exists(rgb) or os.path.getmtime(rgb) < os.path.getmtime(mkv):
            print(f'decoding {mkv}', flush=True)
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', mkv, '-vf', 'fps=60,scale=480:272:flags=neighbor',
                            '-f', 'rawvideo', '-pix_fmt', 'rgb24', rgb], check=True)
        meta = json.load(open(mj))
        out[name] = {'rgb': rgb, 'marks': meta['marks'], 'placeholder': meta.get('placeholder', False)}
    return out


# ------------------------------------------------------------------ the scene

class Scene:
    def __init__(self, sb, tokens, footage, scale, fps):
        self.sb, self.tk, self.fx = sb, tokens, footage
        self.S = scale
        self.W, self.H = int(DW * scale), int(DH * scale)
        self.fps = fps
        self.type = Type(tokens)
        self.glyphs = Glyphs(os.path.normpath(os.path.join(ROOT, tokens['glyphs'])))
        self.col = {k: hexrgb(v) for k, v in tokens['color'].items()}
        rnd = random.Random(7)
        self.stars = [(rnd.random(), rnd.random() * 0.62, rnd.random(), rnd.uniform(.6, 1.8)) for _ in range(140)]
        self.streaks = [(rnd.random(), rnd.random(), rnd.uniform(.25, 1), rnd.uniform(.4, 1.6)) for _ in range(46)]
        self.vignette = self._vignette()
        self.masks = {}

    # -- camera, world
    def camera(self, t):
        return keyed(self.sb['camera'], t, ['cx', 'cy', 's', 'device'])

    def world(self, t):
        return keyed(self.sb['world'], t, ['speed', 'glow', 'tint', 'horizon'])

    def grid_phase(self, t):
        # integral of speed over time, sampled coarsely (deterministic)
        keys = self.sb['world']
        acc, step, x = 0.0, 1 / 30, 0.0
        while x < t:
            acc += keyed(keys, x, ['speed'])['speed'] * min(step, t - x)
            x += step
        return acc

    def _vignette(self):
        w, h = self.W // 8, self.H // 8
        im = Image.new('L', (w, h))
        px = im.load()
        for y in range(h):
            for x in range(w):
                dx, dy = (x / w - .5) * 1.6, (y / h - .5) * 1.9
                px[x, y] = int(255 * clamp(1.08 - .55 * (dx * dx + dy * dy) ** 1.1, 0.35, 1))
        return im.resize((self.W, self.H), Image.BILINEAR).convert('RGB')

    def draw_world(self, ctx, t, cam, w):
        S = self.S
        tint = w['tint']
        g = cairo.LinearGradient(0, 0, 0, DH)
        g.add_color_stop_rgb(0, *self.col['night_top'][:3])
        g.add_color_stop_rgb(1, *self.col['night_bottom'][:3])
        ctx.set_source(g)
        ctx.paint()
        par = (cam['cx'] - DW / 2) * -0.06
        hy = DH * w['horizon'] + (cam['cy'] - DH / 2) * -0.05
        vx = DW / 2 + par
        # horizon light
        rg = cairo.RadialGradient(vx, hy, 0, vx, hy, DW * .62)
        rg.add_color_stop_rgba(0, tint[0], tint[1], tint[2], .42 * w['glow'])
        rg.add_color_stop_rgba(.35, tint[0], tint[1], tint[2], .12 * w['glow'])
        rg.add_color_stop_rgba(1, tint[0], tint[1], tint[2], 0)
        ctx.save()
        ctx.scale(1, .42)
        ctx.translate(0, hy / .42 - hy)
        ctx.set_source(rg)
        ctx.paint()
        ctx.restore()
        # stars above the horizon
        for sx, sy, ph, r in self.stars:
            a = (.25 + .35 * math.sin(t * 1.3 + ph * 20)) * clamp(1 - sy / .62)
            if a <= 0.02:
                continue
            ctx.set_source_rgba(.75, .85, 1, a * w['glow'])
            ctx.arc(((sx * DW + t * 6 + par * .5) % DW), sy * hy, r, 0, 2 * math.pi)
            ctx.fill()
        # the floor: a perspective grid on slow water
        phase = self.grid_phase(t)
        f, camh = 900.0, 150.0
        ctx.set_line_width(1.3)
        dz = 1.0
        off = phase % dz
        for k in range(1, 34):
            z = k * dz * 0.55 - off * 0.55 + 0.35
            if z <= 0.3:
                continue
            y0 = hy + camh / z * (f / 900) * 1.2
            if y0 > DH + 40:
                continue
            a = clamp(0.55 / z, 0, .55) * clamp((z - .3) * 3) * w['glow']
            ctx.set_source_rgba(tint[0] * .75, tint[1] * .75, tint[2], a)
            ctx.move_to(-20, y0)
            for i in range(1, 25):
                x = -20 + i * (DW + 40) / 24
                ctx.line_to(x, y0 + math.sin(x * .006 + t * 1.1 + z) * 3.2 / z)
            ctx.stroke()
        for i in range(-26, 27):
            X = i * 0.9
            ctx.move_to(vx + X * f / 60, hy + camh / 60 * 1.2)
            for z in (30, 12, 6, 3, 1.6, .9, .5, .3):
                ctx.line_to(vx + X * f / z * (1 / 15), hy + camh / z * 1.2)
            a = .28 * w['glow'] * clamp(1 - abs(i) / 27)
            lg = cairo.LinearGradient(0, hy, 0, DH)
            lg.add_color_stop_rgba(0, tint[0], tint[1], tint[2], 0)
            lg.add_color_stop_rgba(.35, tint[0] * .8, tint[1] * .8, tint[2], a)
            lg.add_color_stop_rgba(1, tint[0] * .8, tint[1] * .8, tint[2], a * .7)
            ctx.set_source(lg)
            ctx.stroke()
        # fog at the horizon
        fg = cairo.LinearGradient(0, hy - 40, 0, hy + 120)
        fg.add_color_stop_rgba(0, *self.col['night_bottom'][:3], 0)
        fg.add_color_stop_rgba(.4, tint[0] * .25, tint[1] * .25, tint[2] * .4, .55 * w['glow'])
        fg.add_color_stop_rgba(1, *self.col['night_bottom'][:3], 0)
        ctx.set_source(fg)
        ctx.rectangle(0, hy - 40, DW, 160)
        ctx.fill()

    def draw_streaks(self, ctx, t, amount, cam):
        if amount <= 0:
            return
        for sx, sy, a, ln in self.streaks:
            x = sx * DW
            if abs(x - cam['cx']) < PW * cam['s'] * .5 + 30:
                continue
            L = 140 + 520 * ln * amount
            y = DH + L - ((sy * (DH + L) + t * 2600 * (0.6 + a)) % (DH + 2 * L))
            g = cairo.LinearGradient(0, y, 0, y + L)
            c = self.col['accent']
            g.add_color_stop_rgba(0, c[0], c[1], c[2], .55 * a * amount)
            g.add_color_stop_rgba(1, c[0], c[1], c[2], 0)
            ctx.set_source(g)
            ctx.rectangle(x, y, 2.2, L)
            ctx.fill()

    # -- the console
    def draw_device(self, ctx, cam, alpha):
        if alpha <= 0.01:
            return
        s, cx, cy = cam['s'], cam['cx'], cam['cy']
        tint, acc = self.col['tint'], self.col['accent']
        ctx.save()
        ctx.translate(cx, cy)
        ctx.scale(s, s)
        bw, bh = 862, 376                     # PSP-1000: 170 x 74 mm around a 95 x 54 mm screen
        # glow under the body
        for i in range(7, 0, -1):
            rounded(ctx, -bw / 2 - i * 5, -bh / 2 - i * 5, bw + i * 10, bh + i * 10, 172 + i * 5)
            ctx.set_source_rgba(tint[0], tint[1], tint[2], .018 * alpha)
            ctx.fill()
        rounded(ctx, -bw / 2, -bh / 2, bw, bh, 172)
        g = cairo.LinearGradient(0, -bh / 2, 0, bh / 2)
        g.add_color_stop_rgba(0, .085, .1, .14, alpha)
        g.add_color_stop_rgba(.5, .03, .035, .05, alpha)
        g.add_color_stop_rgba(1, .015, .018, .03, alpha)
        ctx.set_source(g)
        ctx.fill_preserve()
        rim = cairo.LinearGradient(-bw / 2, -bh / 2, bw / 2, bh / 2)
        rim.add_color_stop_rgba(0, acc[0], acc[1], acc[2], .85 * alpha)
        rim.add_color_stop_rgba(.45, tint[0], tint[1], tint[2], .18 * alpha)
        rim.add_color_stop_rgba(1, acc[0], acc[1], acc[2], .55 * alpha)
        ctx.set_source(rim)
        ctx.set_line_width(2.2 / s * 1.4)
        ctx.stroke()
        # gloss
        gl = cairo.LinearGradient(0, -bh / 2, 0, -bh / 2 + 120)
        gl.add_color_stop_rgba(0, 1, 1, 1, .07 * alpha)
        gl.add_color_stop_rgba(1, 1, 1, 1, 0)
        rounded(ctx, -bw / 2 + 6, -bh / 2 + 4, bw - 12, 150, 160)
        ctx.set_source(gl)
        ctx.fill()
        # screen well
        rounded(ctx, -PW / 2 - 16, -PH / 2 - 14, PW + 32, PH + 28, 10)
        ctx.set_source_rgba(0, 0, 0, .9 * alpha)
        ctx.fill_preserve()
        ctx.set_source_rgba(1, 1, 1, .07 * alpha)
        ctx.set_line_width(1.0)
        ctx.stroke()
        line = (1, 1, 1, .13 * alpha)
        # d-pad
        ctx.save()
        ctx.translate(-340, -26)
        for ang in range(4):
            ctx.save()
            ctx.rotate(ang * math.pi / 2)
            rounded(ctx, -13, -52, 26, 34, 6)
            ctx.restore()
        ctx.set_source_rgba(*line)
        ctx.set_line_width(1.4)
        ctx.stroke()
        ctx.arc(0, 0, 62, 0, 2 * math.pi)
        ctx.set_source_rgba(1, 1, 1, .045 * alpha)
        ctx.stroke()
        ctx.restore()
        # analog nub
        ctx.arc(-352, 104, 24, 0, 2 * math.pi)
        ctx.set_source_rgba(1, 1, 1, .06 * alpha)
        ctx.fill_preserve()
        ctx.set_source_rgba(*line)
        ctx.stroke()
        # face buttons, symbols in their colours
        faces = [('triangle', 0, -44, (.35, .85, .6)), ('circle', 44, 0, (1, .4, .45)),
                 ('cross', 0, 44, (.45, .65, 1)), ('square', -44, 0, (1, .55, .85))]
        for name, dx, dy, c in faces:
            ctx.arc(340 + dx, -26 + dy, 19, 0, 2 * math.pi)
            ctx.set_source_rgba(1, 1, 1, .05 * alpha)
            ctx.fill_preserve()
            ctx.set_source_rgba(*line)
            ctx.set_line_width(1.2)
            ctx.stroke()
            self.glyphs.draw(ctx, name, 340 + dx - 8, -26 + dy - 8, 16, (c[0], c[1], c[2], .55 * alpha))
        # the row under the screen
        for i, wdt in enumerate([34, 22, 22, 22, 22, 30, 30]):
            x = -230 + i * 62 if i else -250
            rounded(ctx, x, PH / 2 + 30, wdt, 9, 4.5)
            ctx.set_source_rgba(1, 1, 1, .07 * alpha)
            ctx.fill()
        # shoulders
        for sgn in (-1, 1):
            ctx.save()
            ctx.scale(sgn, 1)
            ctx.arc(bw / 2 - 172, -bh / 2 + 172, 176, -math.pi / 2 - .1, -math.pi / 2 + .62)
            ctx.set_source_rgba(acc[0], acc[1], acc[2], .35 * alpha)
            ctx.set_line_width(3)
            ctx.stroke()
            ctx.restore()
        ctx.restore()

    # -- the screen
    def screen_image(self, t):
        for sh in self.sb['shots']:
            a, b = sh['t']
            if a <= t < b:
                if not sh.get('take'):
                    return None, sh, t - a
                marks = self.fx.marks[sh['take']]
                if sh['mark'] not in marks:
                    raise SystemExit(f'take {sh["take"]} has no mark {sh["mark"]}')
                src = marks[sh['mark']] + sh.get('from', 0.0) + (t - a) * sh.get('speed', 1.0)
                return self.fx.frame(sh['take'], src), sh, t - a
        return None, None, 0

    def mask(self, w, h, r):
        key = (w, h, r)
        if key not in self.masks:
            m = Image.new('L', (w * 2, h * 2), 0)
            ImageDraw.Draw(m).rounded_rectangle((0, 0, w * 2 - 1, h * 2 - 1), r * 2, fill=255)
            self.masks[key] = m.resize((w, h), Image.LANCZOS)
            if len(self.masks) > 64:
                self.masks.pop(next(iter(self.masks)))
        return self.masks[key]

    def place_screen(self, frame, img, sh, u, cam):
        S = self.S
        s = cam['s'] * S
        w, h = int(round(PW * s)), int(round(PH * s))
        x0 = int(round(cam['cx'] * S - w / 2))
        y0 = int(round(cam['cy'] * S - h / 2))
        if img is None:
            img = Image.new('RGB', (PW, PH), (2, 3, 6))
        on = (sh or {}).get('power_on', 0)
        if on and u < on:
            p = u / on
            white = Image.new('RGB', (PW, PH), (235, 245, 255))
            img = Image.blend(Image.new('RGB', (PW, PH), (0, 0, 0)), white, p / .3) if p < .3 \
                else Image.blend(white, img, EASE['out']((p - .3) / .7))
        # bloom: the screen lights the room
        small = Image.new('RGB', (64, 40), (0, 0, 0))
        small.paste(img.resize((32, 18), Image.BILINEAR), (16, 11))
        small = small.filter(ImageFilter.GaussianBlur(5))
        bw, bh = int(w * 2.0), int(h * 2.22)
        bloom = small.resize((bw, bh), Image.BILINEAR)
        bloom = Image.eval(bloom, lambda v: int(v * .62))
        bx, by = x0 - (bw - w) // 2, y0 - (bh - h) // 2
        region = (max(bx, 0), max(by, 0), min(bx + bw, self.W), min(by + bh, self.H))
        if region[2] > region[0] and region[3] > region[1]:
            crop = bloom.crop((region[0] - bx, region[1] - by, region[2] - bx, region[3] - by))
            base = frame.crop(region)
            frame.paste(ImageChops.screen(base, crop), region[:2])
        # the picture, crisp: integer nearest-neighbour first, then smooth to size
        k = max(1, int(s))
        big = img.resize((PW * k, PH * k), Image.NEAREST) if k > 1 else img
        pic = big.resize((w, h), Image.LANCZOS if s < k else Image.BICUBIC) if (w, h) != big.size else big
        r = max(1, int(3 * s)) if cam['s'] < 3.9 else 0
        m = self.mask(w, h, r) if r else None
        frame.paste(pic, (x0, y0), m)
        return (x0, y0, w, h)

    def glare(self, frame, rect, p):
        x0, y0, w, h = rect
        box = (max(x0, 0), max(y0, 0), min(x0 + w, self.W), min(y0 + h, self.H))
        if box[2] <= box[0] or box[3] <= box[1]:
            return
        gl = cairo.ImageSurface(cairo.FORMAT_ARGB32, box[2] - box[0], box[3] - box[1])
        gc = cairo.Context(gl)
        gc.translate(x0 - box[0], y0 - box[1])
        xg = lerp(-w * .6, w * 1.4, EASE['inout'](p))
        g = cairo.LinearGradient(xg - 240 * self.S, 0, xg + 240 * self.S, h * .5)
        g.add_color_stop_rgba(0, 1, 1, 1, 0)
        g.add_color_stop_rgba(.5, 1, 1, 1, .18)
        g.add_color_stop_rgba(1, 1, 1, 1, 0)
        gc.set_source(g)
        gc.rectangle(0, 0, w, h)
        gc.fill()
        base = frame.crop(box).convert('RGBA')
        frame.paste(Image.alpha_composite(base, cairo_to_pil(gl)).convert('RGB'), box[:2])

    # -- overlays
    def active(self, t):
        return [o for o in self.sb['overlays'] if o['t'][0] <= t < o['t'][1]]

    def env(self, o, t):
        a, b = o['t']
        m = self.tk['motion']
        enter = o.get('enter', m['enter'])
        exit_ = o.get('exit', m['exit'])
        pin = clamp((t - a) / enter)
        pout = clamp((t - (b - exit_)) / exit_)
        return pin, pout

    def card_rect(self, o, t, ctx):
        """Layout of a side card; returns (x, y, w, h, parts) in design space."""
        tk = self.tk
        c, ty = tk['card'], tk['type']
        pad = c['pad']
        parts = []
        inner = c['max_w'] - 2 * pad
        yy = 0
        icon = o.get('icon')
        if o.get('label'):
            lay = self.type.layout(ctx, o['label'].upper(), ty['label_px'], 'Medium', False, ty['label_tracking'])
            lw, lh = Type.size(lay)
            parts.append(('label', lay, yy, lw + (ty['label_px'] + 12 if icon else 0)))
            yy += lh + 14
        if o.get('stat'):
            spx = o.get('stat_px', ty['stat_px'])
            probe = o['stat'].replace('{n}', f"{o.get('count', 0):,}")
            lay = self.type.layout(ctx, probe, spx, 'Black', True, -0.035)
            sw, sh = Type.size(lay)
            if sw > inner:
                spx = int(spx * inner / sw)
                lay = self.type.layout(ctx, probe, spx, 'Black', True, -0.035)
                sw, sh = Type.size(lay)
            parts.append(('stat', lay, yy - spx * .1, sw))
            yy += sh - spx * .16
        if o.get('title'):
            px = o.get('title_px', ty['title_px'])
            lay = self.type.layout(ctx, o['title'], px, ty['title_weight'], True, -0.02, inner, spacing=0.98)
            tw, th = Type.size(lay)
            parts.append(('title', lay, yy, tw))
            yy += th + 12
        if o.get('sub'):
            lay = self.type.layout(ctx, o['sub'], ty['sub_px'], 'Regular', False, 0, inner - 10, spacing=1.12)
            sw_, sh_ = Type.size(lay)
            parts.append(('sub', lay, yy + 4, sw_))
            yy += sh_ + 4
        w = clamp(max(p[3] for p in parts) + 2 * pad + 10, c['min_w'], c['max_w'])
        h = yy + 2 * pad
        side = o.get('side', 'left')
        x = c['margin'] if side == 'left' else DW - c['margin'] - w
        y = o.get('y', DH / 2 - h / 2)
        if o.get('valign') == 'bottom':
            y = o['y'] - h
        return x, y, w, h, parts

    def slide(self, o, t):
        pin, pout = self.env(o, t)
        side = o.get('side', 'left')
        sgn = -1 if side == 'left' else 1
        dist = self.tk['motion']['slide_px'] + 260
        dx = sgn * dist * (1 - EASE['expo_out'](pin)) + sgn * dist * .8 * EASE['expo_in'](pout)
        alpha = clamp(pin * 3) * (1 - EASE['in'](pout))
        return dx, alpha, pin, pout

    def glass(self, frame, o, t, measure_ctx):
        x, y, w, h, _ = self.card_rect(o, t, measure_ctx)
        dx, alpha, _, _ = self.slide(o, t)
        if alpha <= 0.01:
            return
        S, c = self.S, self.tk['card']
        X, Y, Wd, Ht = int((x + dx) * S), int(y * S), int(w * S), int(h * S)
        box = (max(X, 0), max(Y, 0), min(X + Wd, self.W), min(Y + Ht, self.H))
        if box[2] <= box[0] or box[3] <= box[1]:
            return
        pad = int(c['blur'] * 2 * S)
        big = (max(box[0] - pad, 0), max(box[1] - pad, 0), min(box[2] + pad, self.W), min(box[3] + pad, self.H))
        region = frame.crop(big).filter(ImageFilter.GaussianBlur(c['blur'] * S))
        region = region.crop((box[0] - big[0], box[1] - big[1], box[2] - big[0], box[3] - big[1]))
        sc = self.col['surface']
        tinted = Image.blend(region, Image.new('RGB', region.size, tuple(int(v * 255) for v in sc[:3])), c['fill_alpha'])
        m = self.mask(Wd, Ht, int(c['radius'] * S)).crop((box[0] - X, box[1] - Y, box[2] - X, box[3] - Y))
        if alpha < 1:
            m = m.point(lambda v: int(v * alpha))
        frame.paste(tinted, box[:2], m)

    def draw_card(self, ctx, glow, o, t):
        x, y, w, h, parts = self.card_rect(o, t, ctx)
        dx, alpha, pin, pout = self.slide(o, t)
        if alpha <= 0.01:
            return
        c, m, ty = self.tk['card'], self.tk['motion'], self.tk['type']
        acc, tint = self.col['accent'], self.col['tint']
        x += dx
        self.cards.append((x, y, w, h, o.get('side', 'left')))
        ctx.save()
        ctx.push_group()
        # border + glow
        rounded(ctx, x, y, w, h, c['radius'])
        bg = cairo.LinearGradient(x, y, x + w, y + h)
        bg.add_color_stop_rgba(0, acc[0], acc[1], acc[2], c['border_alpha'] * 1.6)
        bg.add_color_stop_rgba(.5, 1, 1, 1, c['border_alpha'] * .35)
        bg.add_color_stop_rgba(1, tint[0], tint[1], tint[2], c['border_alpha'])
        ctx.set_source(bg)
        ctx.set_line_width(1.6)
        ctx.stroke()
        # accent bar, grows in
        bar = EASE['expo_out'](clamp((pin - .15) / .6))
        side = o.get('side', 'left')
        bx = x + 18 if side == 'left' else x + w - 22
        ctx.set_source_rgba(acc[0], acc[1], acc[2], .95)
        rounded(ctx, bx, y + c['pad'], 4, (h - 2 * c['pad']) * bar, 2)
        ctx.fill()
        glow.set_source_rgba(acc[0], acc[1], acc[2], .8 * alpha)
        rounded(glow, bx + dx * 0, y + c['pad'], 4, (h - 2 * c['pad']) * bar, 2)
        glow.fill()
        # the lines, each rising through its own mask, staggered
        tx = x + c['pad'] + 8
        dur = o['t'][1] - o['t'][0]
        for i, (kind, lay, ly, lw) in enumerate(parts):
            d = (t - o['t'][0]) - (.1 + i * m['stagger'])
            p = EASE['expo_out'](clamp(d / .5))
            if p <= 0:
                continue
            lw_, lh_ = Type.size(lay)
            yy = y + c['pad'] + ly
            ctx.save()
            ctx.rectangle(x, yy - 6, w, lh_ + 18)
            ctx.clip()
            off = (1 - p) * (lh_ + 12)
            col = {'label': acc, 'stat': (1, 1, 1, 1), 'title': (1, 1, 1, 1), 'sub': self.col['text_dim']}[kind]
            lx = tx
            if kind == 'label' and o.get('icon'):
                self.glyphs.draw(ctx, o['icon'], tx, yy + off + 1, ty['label_px'] + 2, acc)
                lx = tx + ty['label_px'] + 12
            if kind == 'stat':
                num = o.get('count')
                if num:
                    v = num * EASE['expo_out'](clamp((t - o['t'][0] - .1) / 1.25))
                    lay.set_text(o['stat'].replace('{n}', f'{int(round(v)):,}'), -1)
                g = cairo.LinearGradient(lx, yy, lx + lw_, yy + lh_)
                g.add_color_stop_rgba(0, 1, 1, 1, 1)
                g.add_color_stop_rgba(1, acc[0], acc[1], acc[2], 1)
                ctx.set_source(g)
            else:
                ctx.set_source_rgba(*col[:3], col[3] if len(col) > 3 else 1)
            ctx.move_to(lx, yy + off)
            PangoCairo.show_layout(ctx, lay)
            if kind in ('stat', 'title'):
                glow.save()
                glow.set_source_rgba(tint[0], tint[1], tint[2], .55 * alpha * p)
                glow.move_to(lx, yy + off)
                PangoCairo.show_layout(glow, lay)
                glow.restore()
            ctx.restore()
        ctx.pop_group_to_source()
        ctx.paint_with_alpha(alpha)
        ctx.restore()

    def draw_headline(self, ctx, glow, o, t):
        """Kinetic words: each rises through a mask line, staggered; exits upward."""
        ty = self.tk['type']
        px = o.get('px', ty['headline_px'])
        lines = o['text'].split('|')
        accent = set(o.get('accent', []))
        a, b = o['t']
        align = o.get('align', 'center')
        acc, tint = self.col['accent'], self.col['tint']
        lh = px * 1.06
        y0 = o.get('y', DH / 2 - lh * len(lines) / 2)
        stagger = o.get('stagger', .075)
        if o.get('scrim'):
            p = EASE['out'](clamp((t - a) / .4)) * (1 - clamp((t - (b - .3)) / .3))
            cxs = o.get('x', DW / 2) if align == 'center' else DW / 2
            cys = y0 + lh * len(lines) / 2
            ctx.save()
            ctx.translate(cxs, cys)
            ctx.scale(1, .45)
            rg = cairo.RadialGradient(0, 0, 0, 0, 0, DW * .42)
            rg.add_color_stop_rgba(0, 0.01, 0.02, 0.05, .88 * p)
            rg.add_color_stop_rgba(.55, 0.01, 0.02, 0.05, .6 * p)
            rg.add_color_stop_rgba(1, 0.01, 0.02, 0.05, 0)
            ctx.set_source(rg)
            ctx.paint()
            ctx.restore()
        k = 0
        for li, line in enumerate(lines):
            words = line.split(' ')
            lays = [self.type.layout(ctx, wd, px, o.get('weight', ty['headline_weight']), True,
                                     ty['headline_tracking']) for wd in words]
            space = px * .26
            widths = [Type.size(l)[0] for l in lays]
            total = sum(widths) + space * (len(words) - 1)
            x = {'center': o.get('x', DW / 2) - total / 2, 'left': o.get('x', 110),
                 'right': o.get('x', DW - 110) - total}[align]
            y = y0 + li * lh
            for wd, lay, wdt in zip(words, lays, widths):
                d = t - a - k * stagger
                p = EASE['expo_out'](clamp(d / .6))
                q = EASE['expo_in'](clamp((t - (b - .38) - k * .025) / .38))
                k += 1
                if p > 0 and q < 1:
                    ctx.save()
                    ctx.rectangle(x - px * .2, y - px * .05, wdt + px * .4, px * 1.25)
                    ctx.clip()
                    off = (1 - p) * px * 1.1 - q * px * 1.1
                    if wd.strip('.,!') in accent or wd in accent:
                        g = cairo.LinearGradient(x, y, x + wdt, y + px)
                        g.add_color_stop_rgb(0, *acc[:3])
                        g.add_color_stop_rgb(1, *tint[:3])
                        ctx.set_source(g)
                    else:
                        ctx.set_source_rgba(1, 1, 1, 1)
                    ctx.move_to(x, y + off)
                    PangoCairo.show_layout(ctx, lay)
                    ctx.restore()
                    glow.save()
                    glow.set_source_rgba(tint[0], tint[1], tint[2], .5 * p * (1 - q))
                    glow.move_to(x, y + off)
                    PangoCairo.show_layout(glow, lay)
                    glow.restore()
                x += wdt + space

    def draw_wordmark(self, ctx, glow, o, t, alpha=1.0):
        a, b = o['t']
        u = t - a
        px = o.get('px', 250)
        cx, cy = o.get('x', DW / 2), o.get('y', DH / 2)
        p = EASE['expo_out'](clamp(u / .9))
        q = EASE['expo_in'](clamp((t - (b - .3)) / .3)) if o.get('exit', True) else 0
        track = lerp(.9, .16, p)
        lay = self.type.layout(ctx, o.get('text', 'PSPDX'), px, 'Black', True, track)
        w, h = Type.size(lay)
        w -= track * px                    # trailing letter space
        sc = lerp(1.18, 1.0, p) * (1 + .08 * q)
        al = clamp(u / .25) * (1 - q) * alpha
        if o.get('scrim'):
            ctx.save()
            ctx.translate(cx, cy + 20)
            ctx.scale(1, .42)
            rg = cairo.RadialGradient(0, 0, 0, 0, 0, DW * .5)
            rg.add_color_stop_rgba(0, 0.01, 0.02, 0.05, .93 * al)
            rg.add_color_stop_rgba(.6, 0.01, 0.02, 0.05, .7 * al)
            rg.add_color_stop_rgba(1, 0.01, 0.02, 0.05, 0)
            ctx.set_source(rg)
            ctx.paint()
            ctx.restore()
        for c_, a_ in ((ctx, al), (glow, al * .9)):
            c_.save()
            c_.translate(cx, cy)
            c_.scale(sc, sc)
            if c_ is glow:
                tint = self.col['tint']
                c_.set_source_rgba(tint[0], tint[1], tint[2], a_)
            else:
                c_.set_source_rgba(1, 1, 1, a_)
            c_.move_to(-w / 2, -h / 2)
            PangoCairo.show_layout(c_, lay)
            c_.restore()
        if o.get('sub'):
            sp = EASE['expo_out'](clamp((u - .35) / .7))
            lay2 = self.type.layout(ctx, o['sub'].upper(), 26, 'Medium', False, .42)
            w2, h2 = Type.size(lay2)
            acc = self.col['accent']
            ctx.set_source_rgba(acc[0], acc[1], acc[2], sp * (1 - q) * alpha)
            ctx.move_to(cx - (w2 - .42 * 26) / 2, cy + h * .42 + 18 + (1 - sp) * 20)
            PangoCairo.show_layout(ctx, lay2)

    def draw_chip(self, ctx, glow, o, t, rect):
        a, b = o['t']
        p = EASE['back_out'](clamp((t - a) / .42))
        q = EASE['expo_in'](clamp((t - (b - .25)) / .25))
        if p <= 0 or q >= 1:
            return
        lay = self.type.layout(ctx, o['text'], o.get('px', 34), 'SemiBold', True, 0.0)
        w, h = Type.size(lay)
        icon = o.get('icon')
        ip = h * .9 if icon else 0
        pw, ph = w + ip + (26 if icon else 0) + 56, h + 30
        if 'psp' in o and rect:
            x0, y0, rw, rh = rect
            cx = (x0 + o['psp'][0] / PW * rw) / self.S
            cy = (y0 + o['psp'][1] / PH * rh) / self.S
        else:
            cx, cy = o.get('x', DW / 2), o.get('y', DH - 140)
        ctx.save()
        ctx.translate(cx, cy)
        sc = max(p, 0.001) * (1 - .1 * q)
        ctx.scale(sc, sc)
        al = (1 - q)
        rounded(ctx, -pw / 2, -ph / 2, pw, ph, ph / 2)
        sf = self.col['surface']
        ctx.set_source_rgba(sf[0], sf[1], sf[2], .88 * al)
        ctx.fill_preserve()
        acc = self.col['accent']
        ctx.set_source_rgba(acc[0], acc[1], acc[2], .9 * al)
        ctx.set_line_width(2)
        ctx.stroke()
        x = -pw / 2 + 28
        if icon:
            self.glyphs.draw(ctx, icon, x, -ip / 2, ip, (1, 1, 1, al))
            x += ip + 16
        ctx.set_source_rgba(1, 1, 1, al)
        ctx.move_to(x, -h / 2)
        PangoCairo.show_layout(ctx, lay)
        ctx.restore()
        glow.save()
        glow.translate(cx, cy)
        glow.scale(sc, sc)
        rounded(glow, -pw / 2, -ph / 2, pw, ph, ph / 2)
        glow.set_source_rgba(acc[0], acc[1], acc[2], .55 * al)
        glow.set_line_width(6)
        glow.stroke()
        glow.restore()

    def draw_callout(self, ctx, glow, o, t, rect):
        if not rect:
            return
        a, b = o['t']
        p = EASE['expo_out'](clamp((t - a) / .7))
        q = clamp((t - (b - .25)) / .25)
        x0, y0, rw, rh = rect
        px = (x0 + o['psp'][0] / PW * rw) / self.S
        py = (y0 + o['psp'][1] / PH * rh) / self.S
        if 'to' in o:
            tx, ty_ = o['to']
        elif self.cards:
            cx_, cy_, cw_, ch_, side = self.cards[0]
            tx, ty_ = (cx_ + cw_ if side == 'left' else cx_), cy_ + ch_ / 2
        else:
            return
        acc = self.col['accent']
        al = 1 - q
        ex, ey = lerp(tx, px, p), lerp(ty_, py, p)
        for c_, wdt, aa in ((glow, 5, .7), (ctx, 2, 1)):
            c_.set_source_rgba(acc[0], acc[1], acc[2], aa * al)
            c_.set_line_width(wdt)
            c_.move_to(tx, ty_)
            c_.line_to(ex, ey)
            c_.stroke()
        if p > .9:
            r = 9 + 22 * ((t - a) * 1.4 % 1)
            ctx.set_source_rgba(acc[0], acc[1], acc[2], (1 - ((t - a) * 1.4 % 1)) * al)
            ctx.set_line_width(2)
            ctx.arc(px, py, r, 0, 2 * math.pi)
            ctx.stroke()
            ctx.set_source_rgba(1, 1, 1, al)
            ctx.arc(px, py, 7, 0, 2 * math.pi)
            ctx.fill()
            glow.set_source_rgba(acc[0], acc[1], acc[2], al)
            glow.arc(px, py, 14, 0, 2 * math.pi)
            glow.fill()

    def draw_endcard(self, ctx, glow, o, t, cam):
        a, b = o['t']
        u = t - a
        p = EASE['out'](clamp(u / .45))
        ctx.save()
        ctx.push_group()
        self.draw_world(ctx, t, {'cx': DW / 2, 'cy': DH / 2, 's': 1}, keyed(self.sb['world'], t, ['speed', 'glow', 'tint', 'horizon']))
        ctx.pop_group_to_source()
        ctx.paint_with_alpha(p)
        ctx.restore()
        wm = dict(o, t=[a + .15, b], text='PSPDX', px=210, y=DH / 2 - 90, exit=False, sub='PSP Download Index')
        self.draw_wordmark(ctx, glow, wm, t)
        lines = o.get('lines', [])
        for i, (txt, size, weight, colname) in enumerate(lines):
            sp = EASE['expo_out'](clamp((u - .7 - i * .12) / .7))
            lay = self.type.layout(ctx, txt, size, weight, False, .01)
            w, h = Type.size(lay)
            c = self.col[colname]
            ctx.set_source_rgba(c[0], c[1], c[2], sp)
            ctx.move_to(DW / 2 - w / 2, DH / 2 + 110 + i * (size + 22) + (1 - sp) * 24)
            PangoCairo.show_layout(ctx, lay)

    # -- one frame
    def render(self, t, measure=None):
        S, W, H = self.S, self.W, self.H
        cam = self.camera(t)
        w = self.world(t)
        bg = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
        c = cairo.Context(bg)
        c.scale(S, S)
        self.draw_world(c, t, cam, w)
        streak = sum(clamp((t - f['t'][0]) / .5) * clamp((f['t'][1] - t) / .5)
                     for f in self.sb.get('fx', []) if f['kind'] == 'streaks' and f['t'][0] <= t < f['t'][1])
        self.draw_streaks(c, t, streak, cam)
        self.draw_device(c, cam, cam['device'])
        frame = cairo_to_pil(bg, 'RGB').copy()
        img, sh, u = self.screen_image(t)
        rect = self.place_screen(frame, img, sh, u, cam) if sh is not None else None
        # glare across the glass
        for f in self.sb.get('fx', []):
            if f['kind'] == 'glare' and f['t'][0] <= t < f['t'][1] and rect:
                self.glare(frame, rect, (t - f['t'][0]) / (f['t'][1] - f['t'][0]))
        active = self.active(t)
        vec = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
        v = cairo.Context(vec)
        v.scale(S, S)
        gsurf = cairo.ImageSurface(cairo.FORMAT_ARGB32, W // 4, H // 4)
        gctx = cairo.Context(gsurf)
        gctx.scale(S / 4, S / 4)
        self.cards = []
        for o in active:
            if o['kind'] == 'card':
                self.glass(frame, o, t, v)
        for o in sorted(active, key=lambda o: o['kind'] == 'callout'):
            k = o['kind']
            if k == 'card':
                self.draw_card(v, gctx, o, t)
            elif k == 'headline':
                self.draw_headline(v, gctx, o, t)
            elif k == 'wordmark':
                self.draw_wordmark(v, gctx, o, t)
            elif k == 'chip':
                self.draw_chip(v, gctx, o, t, rect)
            elif k == 'callout':
                self.draw_callout(v, gctx, o, t, rect)
            elif k == 'endcard':
                self.draw_endcard(v, gctx, o, t, cam)
        if rect and sh and sh.get('take') and self.fx.placeholder.get(sh['take']) \
                and not os.environ.get('PROMO_UNTAGGED'):
            lay = self.type.layout(v, 'PLACEHOLDER · hardware still', 15, 'Medium', False, .12)
            v.set_source_rgba(1, .8, .3, .7)
            ty_ = (rect[1] + rect[3]) / S + 10
            v.move_to(max(rect[0] / S, 12) + 4, ty_ if ty_ < DH - 30 else DH - 34)
            PangoCairo.show_layout(v, lay)
        # glow: blurred at a quarter size, added
        gimg = cairo_to_pil(gsurf, 'PREMUL').filter(ImageFilter.GaussianBlur(7 * S))
        frame = ImageChops.add(frame, gimg.resize((W, H), Image.BILINEAR))
        frame = Image.alpha_composite(frame.convert('RGBA'), cairo_to_pil(vec)).convert('RGB')
        frame = ImageChops.multiply(frame, self.vignette)
        for f in self.sb.get('fx', []):
            if f['kind'] == 'flash' and f['t'][0] <= t < f['t'][1]:
                p = 1 - (t - f['t'][0]) / (f['t'][1] - f['t'][0])
                frame = Image.blend(frame, Image.new('RGB', (W, H), (225, 238, 255)), clamp(p * f.get('amount', .8)) ** 1.6)
            if f['kind'] == 'fade' and f['t'][0] <= t < f['t'][1]:
                p = (t - f['t'][0]) / (f['t'][1] - f['t'][0])
                if f.get('dir', 'out') == 'in':
                    p = 1 - p
                frame = Image.blend(frame, Image.new('RGB', (W, H), (0, 0, 0)), clamp(p))
        return frame


# ------------------------------------------------------------------ driver

G = {}


def worker_init(args):
    sb, tokens, takes, scale, fps = args
    G['scene'] = Scene(sb, tokens, Footage(None, takes), scale, fps)


def render_chunk(job):
    idx, frames, path = job
    sc = G['scene']
    cmd = ['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
           '-s', f'{sc.W}x{sc.H}', '-r', str(sc.fps), '-i', '-',
           '-vf', 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p',
           '-c:v', 'libx264', '-preset', 'slow', '-crf', '14', '-g', str(sc.fps * 2),
           '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709',
           '-movflags', '+faststart', path]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for n in frames:
        p.stdin.write(sc.render(n / sc.fps).tobytes())
    p.stdin.close()
    if p.wait():
        raise RuntimeError(f'ffmpeg failed on chunk {idx}')
    return idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--storyboard', default=os.path.join(ROOT, 'storyboard.json'))
    ap.add_argument('--tokens', default=os.path.join(ROOT, 'tokens.json'))
    ap.add_argument('--footage', default=os.path.join(ROOT, 'footage'))
    ap.add_argument('--work', default=os.path.join(os.environ.get('TMPDIR', '/tmp'), 'pspdx-promo-work'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'pspdx-promo.mp4'))
    ap.add_argument('--audio', default=None, help='WAV to mux in (audio.py makes it)')
    ap.add_argument('--preview', action='store_true', help='960x540, 30 fps')
    ap.add_argument('--from', dest='t0', type=float, default=0.0)
    ap.add_argument('--to', dest='t1', type=float, default=None)
    ap.add_argument('--still', default=None, help='comma-separated seconds; writes PNGs next to --out')
    ap.add_argument('--untagged', action='store_true',
                    help='no PLACEHOLDER tag on stills (they are real hardware screenshots)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    a = ap.parse_args()
    if a.untagged:
        os.environ["PROMO_UNTAGGED"] = "1"

    sb = json.load(open(a.storyboard))
    tokens = json.load(open(a.tokens))
    os.makedirs(a.work, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    names = sorted({s['take'] for s in sb['shots'] if s.get('take')})
    takes = decode_takes(a.footage, a.work, names)
    scale = .5 if a.preview else 1.0
    fps = 30 if a.preview else sb['fps']

    if a.still:
        sc = Scene(sb, tokens, Footage(None, takes), scale, fps)
        base = os.path.splitext(a.out)[0]
        for ts in a.still.split(','):
            path = f'{base}-{float(ts):06.2f}.png'
            sc.render(float(ts)).save(path)
            print(path)
        return

    t1 = a.t1 if a.t1 is not None else sb['duration']
    frames = list(range(int(round(a.t0 * fps)), int(round(t1 * fps))))
    n = max(1, min(a.jobs * 2, len(frames) // 30 or 1))
    size = math.ceil(len(frames) / n)
    chunkdir = os.path.join(a.work, 'chunks')
    shutil.rmtree(chunkdir, ignore_errors=True)
    os.makedirs(chunkdir)
    jobs = [(i, frames[i * size:(i + 1) * size], os.path.join(chunkdir, f'c{i:03d}.mp4'))
            for i in range(n) if frames[i * size:(i + 1) * size]]
    print(f'{len(frames)} frames at {fps} fps, {len(jobs)} chunks on {a.jobs} processes', flush=True)
    with mp.get_context('fork').Pool(a.jobs, worker_init, ((sb, tokens, takes, scale, fps),)) as pool:
        for done, i in enumerate(pool.imap_unordered(render_chunk, jobs), 1):
            print(f'  chunk {i:3d} done ({done}/{len(jobs)})', flush=True)
    lst = os.path.join(chunkdir, 'list.txt')
    with open(lst, 'w') as f:
        for _, _, p in jobs:
            f.write(f"file '{p}'\n")
    cmd = ['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', lst]
    if a.audio:
        cmd += ['-ss', str(a.t0), '-t', str(t1 - a.t0), '-i', a.audio, '-map', '0:v', '-map', '1:a',
                '-c:a', 'aac', '-b:a', '256k', '-shortest']
    cmd += ['-c:v', 'copy', '-movflags', '+faststart', a.out]
    subprocess.run(cmd, check=True)
    print(a.out)


if __name__ == '__main__':
    main()
