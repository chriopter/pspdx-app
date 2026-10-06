"""The advance of every character in a PGF font, read the way intraFont
reads them, and with them the width intraFontMeasureText gives a line: what
gui/font.c's font_width returns on the console, without a console."""
import struct


def bits(data, at, n):
    v = 0
    for i in range(n):
        v |= ((data[(at + i) // 8] >> ((at + i) % 8)) & 1) << i
    return v


def table(data, pos, count, bpe):
    size = ((count * bpe + 31) // 32) * 4
    return [bits(data, pos * 8 + i * bpe, bpe) for i in range(count)], pos + size


class Font:
    def __init__(self, path):
        with open(path, "rb") as file:
            d = file.read()
        header_len, = struct.unpack_from("<H", d, 2)
        if d[4:8] != b"PGF0":
            raise ValueError(path + ": not a PGF")
        revision, _, charmap_len, charptr_len, charmap_bpe, charptr_bpe = struct.unpack_from("<6I", d, 8)
        charmap_min, = struct.unpack_from("<H", d, 182)
        t1, t2, t3, advance_len = d[258:262]
        shadowmap_len, shadowmap_bpe = struct.unpack_from("<2I", d, 364)
        pos = header_len + (t1 + t2 + t3) * 8
        advance = struct.unpack_from("<%di" % (advance_len * 2), d, pos)
        pos += advance_len * 8
        _, pos = table(d, pos, shadowmap_len, shadowmap_bpe)
        if revision == 3:
            self.ranges = [struct.unpack_from("<2H", d, pos + 4 * i) for i in range(7)]
            pos += 28
        else:
            self.ranges = [(charmap_min, charmap_len)]
        self.charmap, pos = table(d, pos, charmap_len, charmap_bpe)
        charptr, pos = table(d, pos, charptr_len, charptr_bpe)
        self.advance = []
        for ptr in charptr:
            b = (pos + ptr * 4) * 8 + 14 + 7 * 4
            flags = bits(d, b, 6)
            b += 6 + 7 + 9 + 24 + sum(0 if flags & f else 56 for f in (4, 8, 16))
            self.advance.append(int(advance[bits(d, b, 8) * 2] / 16))

    def glyph(self, ucs):
        at = 0
        for first, count in self.ranges:
            if first <= ucs < first + count:
                g = self.charmap[at + ucs - first]
                return g if g < len(self.advance) else None
            at += count
        return None

    def width(self, text, size=1.0):
        """Pixels, at intraFont's size; a character the font lacks is 0 wide."""
        return sum(self.advance[g] * size * 0.25 for g in map(self.glyph, map(ord, text)) if g is not None)
