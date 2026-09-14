#!/bin/sh
# Rasterises app/assets/marks/src/*.svg into the PNGs the atlas is built from.
#
# Two files come out of each source. The glyph is the drawing at its own
# size, white, with the anti-aliasing in the alpha -- the SVG's width and
# height are the cell, and every cell carries one pixel of margin so the
# anti-aliasing has somewhere to go. The shadow is the same drawing three
# pixels larger on each side, black, and blurred: the XMB ships a separate
# blurred twin beside every foreground glyph it has, for the same reason we
# need one -- a white shape on a photograph loses its edge wherever the
# photograph is pale.
#
# The blur is done at four times the size and scaled back down, so the
# falloff is smooth rather than quantised to the sigma of a 13-pixel image.
#
#     sh dev/marks/render.sh          # then python3 dev/marks/embed.py
#
# Needs rsvg-convert and ImageMagick. Neither the build nor the generator
# needs them: the PNGs and the header are committed.

set -e

here=$(dirname "$0")
marks=$(cd "$here/../../app/assets/marks" && pwd)
src="$marks/src"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pad=3           # pixels of room around the shadow
sigma=1.1       # blur, in final pixels
scale=4         # rendered this much larger, blurred, then scaled back

for path in "$src"/*.svg; do
    name=$(basename "$path" .svg)
    case "$name" in *_shadow) continue ;; esac

    # The cell is whatever the source says it is.
    w=$(sed -n 's/.*<svg[^>]* width="\([0-9.]*\)".*/\1/p' "$path" | head -1)
    h=$(sed -n 's/.*<svg[^>]* height="\([0-9.]*\)".*/\1/p' "$path" | head -1)
    [ -n "$w" ] && [ -n "$h" ] || { echo "$name: no size in the svg" >&2; exit 1; }

    # White with the coverage in the alpha. The alpha is taken out and put
    # back rather than trusted as it comes: a resampled RGBA PNG can carry
    # black in the colour of its transparent pixels, and that black would
    # show up as a rim once the GE multiplies by the caller's colour.
    rsvg-convert -w "$w" -h "$h" "$path" -o "$tmp/g.png"
    magick "$tmp/g.png" -alpha extract "$tmp/ga.png"
    magick -size "${w}x${h}" xc:white "$tmp/ga.png" -alpha off \
           -compose copy_opacity -composite "PNG32:$marks/$name.png"

    # The shadow: the same shape, rendered large, spread, and brought back.
    sw=$((w + pad * 2))
    sh=$((h + pad * 2))
    big_w=$((w * scale))
    big_h=$((h * scale))
    big_sw=$((sw * scale))
    big_sh=$((sh * scale))
    big_sigma=$(awk "BEGIN { print $sigma * $scale }")
    # A mark may bring its own twin instead: a filled shape under an outline
    # is how the system draws its SELECT and START pills, and a blur of the
    # outline is not that. The twin's cell is the shadow cell, unblurred.
    if [ -f "$src/${name}_shadow.svg" ]; then
        rsvg-convert -w "$sw" -h "$sh" "$src/${name}_shadow.svg" -o "$tmp/s.png"
        magick "$tmp/s.png" -alpha extract "$tmp/sa.png"
    else
    rsvg-convert -w "$big_w" -h "$big_h" "$path" -o "$tmp/s.png"
    magick "$tmp/s.png" -alpha extract -background black -gravity center \
           -extent "${big_sw}x${big_sh}" -blur "0x$big_sigma" \
           -resize "${sw}x${sh}!" "$tmp/sa.png"
    fi
    magick -size "${sw}x${sh}" xc:black "$tmp/sa.png" -alpha off \
           -compose copy_opacity -composite "PNG32:$marks/${name}_shadow.png"

    echo "$name ${w}x${h}, shadow ${sw}x${sh}"
done
