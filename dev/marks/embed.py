#!/usr/bin/env python3
"""Packs app/assets/marks/*.png into one atlas and writes app/gui/marks_data.h.

Every mark is two pictures: the glyph, white with the anti-aliasing in its
alpha, and its shadow, the same shape blurred and black. Both go into a
single power-of-two sheet, so the shell draws a mark as two textured sprites
out of one bound texture rather than as a pile of rectangles.

Only the alpha is emitted. The colour of a cell is decided by which list it
is in -- white for a glyph, black for a shadow -- and gui/marks.c paints it
while it unpacks the sheet into a texture. Storing four bytes a pixel to
say "white" sixteen thousand times would quadruple this file for nothing.

The header is committed, which is why this reads PNG itself rather than
leaning on Pillow: regenerating the set must need no more than a stock
Python, and the Docker build needs neither.

    sh dev/marks/render.sh          # svg -> png, needs rsvg-convert
    python3 dev/marks/embed.py      # png -> app/gui/marks_data.h
"""

import os
import struct
import sys
import zlib

# The order here is the order of enum mark in gui/marks.h. Nothing keeps the
# two in step but this comment, so if a mark is added it goes in both.
ORDER = [
    'cross', 'circle', 'triangle', 'square',
    'tick', 'update', 'basket', 'installed', 'play', 'download', 'info',
    'store', 'umd',
    'games', 'demos', 'apps',
    'plus', 'page', 'list', 'globe',
    'start', 'select', 'l', 'r', 'home', 'gear',
]

# Every glyph is drawn with a pixel of margin around it so the anti-aliasing
# has somewhere to land. The shell lays out with the size inside that margin,
# which is the size the mark looks.
MARGIN = 1

# Candidate sheets, smallest first. The GE wants both sides a power of two.
SHEETS = [(64, 64), (128, 64), (128, 128), (256, 128), (256, 256)]

GUTTER = 1      # a transparent pixel between cells, so no sprite samples
                # its neighbour if the filter ever slips half a texel


def read_png(path):
    """The alpha plane of an 8-bit RGBA PNG. Only what render.sh writes has
    to be read, so the exotic colour types and interlace are not handled --
    they are refused loudly instead of decoded wrongly."""
    with open(path, 'rb') as fh:
        data = fh.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(path + ': not a PNG')
    pos = 8
    idat = b''
    w = h = None
    while pos < len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b'IHDR':
            w, h, depth, colour, comp, filt, inter = struct.unpack('>IIBBBBB', body)
            if (depth, colour, inter) != (8, 6, 0):
                raise ValueError(path + ': want 8-bit RGBA, not interlaced')
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray()
    prev = bytearray(stride)
    at = 0
    for _ in range(h):
        ftype = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
            elif ftype != 0:
                raise ValueError(path + ': unknown filter %d' % ftype)
        out += line[3::4]
        prev = line
    return w, h, bytes(out)


def pack(cells, sheet_w, sheet_h):
    """Shelves, tallest cell first: rows as high as the first cell that
    opened them, filled left to right. For two dozen cells of four or five
    sizes it wastes a few hundred pixels and is twenty lines long, which is
    the trade this wants."""
    order = sorted(range(len(cells)), key=lambda i: -cells[i][1])
    at = [None] * len(cells)
    x = y = shelf_h = 0
    for i in order:
        w, h = cells[i]
        if x + w > sheet_w:
            x = 0
            y += shelf_h + GUTTER
            shelf_h = 0
        if y + h > sheet_h:
            return None
        at[i] = (x, y)
        x += w + GUTTER
        if h > shelf_h:
            shelf_h = h
    return at


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    app = os.path.join(os.path.dirname(os.path.dirname(here)), 'app')
    src = os.path.join(app, 'assets', 'marks')
    dst = os.path.join(app, 'gui', 'marks_data.h')

    # Glyph then shadow for each mark, in one list, so the packer sees them
    # all at once and the tall shadows open the shelves.
    pics = []
    for name in ORDER:
        for suffix in ('', '_shadow'):
            w, h, alpha = read_png(os.path.join(src, name + suffix + '.png'))
            pics.append((name + suffix, w, h, alpha))

    cells = [(p[1], p[2]) for p in pics]
    for sheet_w, sheet_h in SHEETS:
        at = pack(cells, sheet_w, sheet_h)
        if at:
            break
    else:
        raise SystemExit('marks: nothing in SHEETS is big enough')

    sheet = bytearray(sheet_w * sheet_h)
    for (name, w, h, alpha), (x, y) in zip(pics, at):
        for row in range(h):
            start = (y + row) * sheet_w + x
            sheet[start:start + w] = alpha[row * w:(row + 1) * w]

    used = sum(w * h for _, w, h, _ in pics)

    lines = []
    # No wildcard in the line: a "/*" inside a comment is a warning, and this
    # build is kept warning-free.
    lines.append('/* Generated by dev/marks/embed.py from the PNGs in'
                 ' assets/marks -- do not edit. */')
    lines.append('')
    lines.append('#ifndef PSPDX_MARKS_DATA_H')
    lines.append('#define PSPDX_MARKS_DATA_H')
    lines.append('')
    lines.append('/* The sheet: one byte of coverage a pixel, %d of %d used.'
                 ' */' % (used, sheet_w * sheet_h))
    lines.append('#define MARK_SHEET_W %d' % sheet_w)
    lines.append('#define MARK_SHEET_H %d' % sheet_h)
    lines.append('')
    lines.append('/* Where a mark is on the sheet. w and h are what the mark')
    lines.append('   measures to the shell, one pixel inside the cell it is')
    lines.append('   drawn in; gx, gy, gw, gh are the glyph cell and sx, sy,')
    lines.append('   sw, sh the blurred black one it stands on. */')
    lines.append('struct mark_glyph {')
    lines.append('    const char *name;')
    lines.append('    unsigned char w, h;')
    lines.append('    unsigned char gx, gy, gw, gh;')
    lines.append('    unsigned char sx, sy, sw, sh;')
    lines.append('};')
    lines.append('')
    lines.append('static const struct mark_glyph mark_glyphs[] = {')
    for i, name in enumerate(ORDER):
        g, s = pics[i * 2], pics[i * 2 + 1]
        (gx, gy), (sx, sy) = at[i * 2], at[i * 2 + 1]
        lines.append('    { "%s", %d, %d,  %d, %d, %d, %d,  %d, %d, %d, %d },'
                     % (name, g[1] - MARGIN * 2, g[2] - MARGIN * 2,
                        gx, gy, g[1], g[2], sx, sy, s[1], s[2]))
    lines.append('};')
    lines.append('')
    lines.append('static const unsigned char mark_sheet[%d] = {' % len(sheet))
    for y in range(sheet_h):
        row = sheet[y * sheet_w:(y + 1) * sheet_w]
        # Sixteen to a line: the sheet is the bulk of this file and a run of
        # zeroes should cost as little of it as it can.
        for i in range(0, sheet_w, 16):
            lines.append('    ' + ''.join('%d,' % v for v in row[i:i + 16]))
    lines.append('};')
    lines.append('')
    lines.append('#endif')
    lines.append('')

    with open(dst, 'w') as fh:
        fh.write('\n'.join(lines))
    sys.stderr.write('%s: %d marks on a %dx%d sheet, %d%% used\n'
                     % (dst, len(ORDER), sheet_w, sheet_h,
                        used * 100 // (sheet_w * sheet_h)))


if __name__ == '__main__':
    main()
