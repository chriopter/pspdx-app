#!/bin/sh
# The sound under the card: SND0.AT3, the file the XMB plays out of the EBOOT
# while the cursor rests on it, and the catalog plays too. ATRAC3 in a RIFF,
# 44.1 kHz stereo, eighteen seconds at most.
#
#   sh make-snd0.sh <audio> <SND0.AT3> [start seconds]
#
# ffmpeg turns whatever it is given into the one thing atracdenc accepts, a
# 44.1 kHz 16-bit stereo WAV, cut to DURATION seconds (18 by default) with a
# fade at either end so the loop does not click. atracdenc encodes it; the
# .at3 name asks it for the RIFF container, which is what sceAtrac reads.
#
# In a RIFF the encoder knows two rates, and --bitrate is not one of its
# knobs there: 132 kbps (LP2, what Sony's own SND0 files are, 16.5 KB a
# second) or BITRATE=66 (LP4, half the bytes and audibly so).
#
# atracdenc is the only free ATRAC3 encoder and no distribution packages it:
# on Arch it is atracdenc-git in the AUR, anywhere else it builds from
# https://github.com/dcherednik/atracdenc with cmake and libsndfile. ATRACDENC
# in the environment names the binary, or a wrapper around one in a
# container; it is run from the directory holding the WAV with relative
# names, so a wrapper that mounts its working directory works too.
set -e
src="$1"
out="$2"
start="${3:-0}"
dur="${DURATION:-18}"
case "${BITRATE:-132}" in
	132) codec=atrac3 ;;
	66) codec=atrac3_lp4 ;;
	*) echo "BITRATE is 132 or 66; ATRAC3 in a RIFF has no other" >&2; exit 2 ;;
esac
[ -n "$src" ] && [ -n "$out" ] || { echo "usage: sh make-snd0.sh <audio> <SND0.AT3> [start seconds]" >&2; exit 2; }
command -v ffmpeg >/dev/null || { echo "ffmpeg is not on PATH" >&2; exit 1; }

enc="${ATRACDENC:-atracdenc}"
if ! command -v "$enc" >/dev/null; then
	echo "no ATRAC3 encoder: set ATRACDENC or put atracdenc on PATH." >&2
	echo "  Arch: an AUR helper installs atracdenc-git" >&2
	echo "  else: https://github.com/dcherednik/atracdenc, cmake and libsndfile" >&2
	exit 1
fi
# The encoder is run from another directory, so a relative path to it has to
# be made absolute first.
case "$enc" in
	*/*) enc="$(cd "$(dirname "$enc")" && pwd)/$(basename "$enc")" ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# -t before -i stops decoding at the cut instead of decoding a whole album
# and throwing it away. The fade-out is done by reversing, fading in and
# reversing back, so it sits at the real end whether or not the input was
# shorter than the cut.
ffmpeg -v error -y -ss "$start" -t "$dur" -i "$src" -vn \
	-af "afade=t=in:d=0.2,areverse,afade=t=in:d=1,areverse" \
	-ac 2 -ar 44100 -c:a pcm_s16le "$tmp/snd0.wav"

# atracdenc chooses the container by the output's extension, so the encode
# goes to a .at3 name and is moved afterwards, whatever the caller asked for.
(cd "$tmp" && "$enc" -e "$codec" -i snd0.wav -o snd0.at3 >/dev/null)
[ "$(head -c 4 "$tmp/snd0.at3")" = "RIFF" ] || { echo "the encoder did not write a RIFF" >&2; exit 1; }

# The firmware refuses the file as the encoder leaves it. atracdenc writes
# the fact chunk, the count of samples, rounded up to whole frames, and
# sceAtrac turns that count plus the decoder's lead-in (the chunk's second
# word and 69 samples more) into a count of frames that has to be strictly
# less than the frames the data holds. So the count is written down to the
# largest that passes: the WAV's own, or a frame or two under the data's
# end, which a looped clip with a fade never reaches.
python3 - "$tmp/snd0.wav" "$tmp/snd0.at3" <<'PY'
import struct, sys
wav, at3 = sys.argv[1], sys.argv[2]

def chunks(b):
    pos = 12
    while pos + 8 <= len(b):
        tag, n = bytes(b[pos:pos + 4]), struct.unpack('<I', b[pos + 4:pos + 8])[0]
        yield tag, pos + 8, n
        pos += 8 + n + (n & 1)

w = open(wav, 'rb').read()
true = next(n // 4 for tag, at, n in chunks(w) if tag == b'data')
a = bytearray(open(at3, 'rb').read())
fmt = next((at, n) for tag, at, n in chunks(a) if tag == b'fmt ')
block = struct.unpack('<H', a[fmt[0] + 12:fmt[0] + 14])[0]
data = next(n for tag, at, n in chunks(a) if tag == b'data')
fact = next((at, n) for tag, at, n in chunks(a) if tag == b'fact')
lead = struct.unpack('<I', a[fact[0] + 4:fact[0] + 8])[0] if fact[1] >= 8 else 0
frames = data // block
count = true
while ((count + lead + 69 - 1) >> 10) * block >= data and count > 1024:
    count -= 1024
struct.pack_into('<I', a, fact[0], count)
open(at3, 'wb').write(a)
print("fact: %d of %d samples, %d frames of %d" % (count, true, (count + lead + 68) >> 10, frames))
PY
mv "$tmp/snd0.at3" "$out"
echo "$out: $(stat -c %s "$out") bytes"
