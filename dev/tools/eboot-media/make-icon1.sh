#!/bin/sh
# The film on the card: ICON1.PMF, the file the XMB plays out of the EBOOT
# and the catalog plays too. 144x80, a few seconds, no sound.
#
#   sh make-icon1.sh <video> <ICON1.PMF> [start seconds]
#
# ffmpeg scales and centre-crops the source to 144x80, which is what an XMB
# icon is and whose sides are sixteenths, the unit the PSMF header counts in.
# Thirty frames a second, H.264 Constrained Baseline, since that is what the
# hardware decodes; a keyframe every second so a restart never waits long.
# The MP4 that comes out is then wrapped by dev/tools/mp4-to-psmf.c, the same
# code the client wraps its own clips with: a PSMF header and an MPEG-2
# program stream in 2048-byte packs laid out the way Sony's composer lays
# it out -- every GOP in a pack of its own, opened by a system header and a
# private-stream-2 index of the GOP's access unit sizes, the video in PES
# 0xE0 behind it. sceMpeg on a retail PSP refuses a stream without the
# index; the client's reader refuses it too. ffprobe reads the result on
# the desk, so it can be checked before it goes into a PBP.
#
# A .pmf or .PMF as the source is remuxed as it is: the pictures are copied
# into the new layout, nothing is re-encoded, and DURATION, FPS and the
# start are ignored. How an ICON1 made before the layout is brought up to it.
#
# Six seconds by default. DURATION and FPS in the environment change that;
# FPS=15 about halves the file for a calm clip.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/../../../app"
src="$1"
out="$2"
start="${3:-0}"
dur="${DURATION:-6}"
fps="${FPS:-30}"
[ -n "$src" ] && [ -n "$out" ] || { echo "usage: sh make-icon1.sh <video> <ICON1.PMF> [start seconds]" >&2; exit 2; }
command -v ffmpeg >/dev/null || { echo "ffmpeg is not on PATH" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Built every time: it is three small files and a second of cc, and a stale
# binary lying around would be one more thing to keep in step.
cc -O2 -I"$APP" "$APP/video/mp4.c" "$APP/video/psmf.c" "$HERE/../mp4-to-psmf.c" -o "$tmp/mp4-to-psmf"

# force_original_aspect_ratio=increase then crop: fill the frame, cut the
# overhang, never letterbox. -bf 0 and the baseline profile mean every
# sample shows in the order it is stored, which is how the wrapper reads it.
case "$src" in
*.pmf|*.PMF|*.psmf)
	ffmpeg -nostdin -v error -y -i "$src" -an -c:v copy -movflags +faststart "$tmp/icon1.mp4" ;;
*)
	ffmpeg -nostdin -v error -y -ss "$start" -i "$src" -t "$dur" -an \
		-vf "scale=144:80:force_original_aspect_ratio=increase:flags=lanczos,crop=144:80,fps=$fps,format=yuv420p" \
		-c:v libx264 -profile:v baseline -preset veryslow -tune film \
		-crf 23 -maxrate 300k -bufsize 300k -g "$fps" -keyint_min "$fps" -sc_threshold 0 -bf 0 \
		-movflags +faststart "$tmp/icon1.mp4" ;;
esac

"$tmp/mp4-to-psmf" "$tmp/icon1.mp4" "$out"

if command -v ffprobe >/dev/null; then
	echo "ffprobe sees: $(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,width,height,r_frame_rate -of csv=p=0 "$out")"
fi
