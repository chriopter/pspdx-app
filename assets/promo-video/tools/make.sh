#!/usr/bin/env bash
# End to end: footage -> soundtrack -> video.
#   tools/make.sh placeholder [--preview]      the stills in placeholder/
#   tools/make.sh real CAPTURE_PREFIX [--preview]   a psp_capture.py take
# Output: out/pspdx-promo[-placeholder].mp4 next to this skill.
# Exits non-zero on the first failing step.
set -Eeuo pipefail
cd "$(dirname "$0")/.."
WORK="${TMPDIR:-/tmp}/pspdx-promo-work"
mode="${1:?placeholder|real}"; shift
case "$mode" in
  placeholder)
    FOOT="$WORK/footage-placeholder"
    python3 tools/footage.py placeholder storyboard.json "$FOOT"
    OUT=out/pspdx-promo-placeholder.mp4 ;;
  real)
    cap="${1:?capture prefix, e.g. captures/hero}"; shift
    FOOT=footage
    mkdir -p "$FOOT"
    python3 tools/footage.py capture "$cap" "$FOOT/hero"
    OUT=out/pspdx-promo.mp4 ;;
  *) echo "placeholder|real" >&2; exit 2 ;;
esac
mkdir -p out
python3 tools/audio.py --out out/soundtrack.wav --work "$WORK"
python3 tools/render.py --footage "$FOOT" --work "$WORK" --audio out/soundtrack.wav --out "$OUT" "$@"
