#!/bin/sh
# The film on the card: ICON1.PMF, the file the XMB plays out of the EBOOT
# and the catalog plays too. 144x80, a few seconds, no sound.
#
#   sh make-icon1.sh <video> <ICON1.PMF> [start seconds]
#
# ffmpeg scales and centre-crops the source to 144x80, which is what an XMB
# icon is and whose sides are sixteenths, the unit the PSMF header counts in.
# Thirty frames a second, H.264 Main profile at level 2.1, as Sony's own
# ICON1s are: the PSP-1000 (6.61) fails a Baseline stream on its very first
# picture with 0x80628002, and plays the same encode in Main. One reference
# picture, no B-frames, and explicit HRD/picture timing keep it compatible
# with the retail decoder across keyframes.
# The MP4 that comes out is then wrapped by dev/tools/mp4-to-psmf.c, the same
# code the client wraps its own clips with: a PSMF header and an MPEG-2
# program stream in 2048-byte packs laid out the way Sony's composer lays
# it out -- every GOP in a pack of its own, opened by a system header and a
# private-stream-2 index of the GOP's access unit sizes, the video in PES
# 0xE0 behind it. sceMpeg on a retail PSP refuses a stream without the
# index; the client's reader refuses it too. ffprobe reads the result on
# the desk, so it can be checked before it goes into a PBP.
#
# A .pmf or .PMF works as the source too, and is decoded and encoded again
# like any other video: remuxing alone would carry an old clip's
# incompatible AVC settings over unchanged. Prefer the original footage
# where there is one; a 144x80 film encoded twice loses a little.
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
# overhang, never letterbox. -bf 0 means every sample shows in the order it
# is stored, which is how the wrapper reads it.
ffmpeg -nostdin -v error -y -ss "$start" -i "$src" -t "$dur" -an \
	-vf "scale=144:80:force_original_aspect_ratio=increase:flags=lanczos,crop=144:80,fps=$fps,format=yuv420p" \
	-c:v libx264 -profile:v main -level:v 2.1 -preset veryslow -tune film \
	-crf 23 -maxrate 300k -bufsize 300k -refs 1 \
	-x264-params "aud=1:nal-hrd=vbr:pic-struct=1" -g "$fps" -keyint_min "$fps" -sc_threshold 0 -bf 0 \
	-movflags +faststart "$tmp/icon1.mp4"

"$tmp/mp4-to-psmf" "$tmp/icon1.mp4" "$out"

if command -v ffprobe >/dev/null; then
	echo "ffprobe sees: $(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,width,height,r_frame_rate -of csv=p=0 "$out")"
fi
