#!/bin/sh
# Runs EBOOT.PBP under the PPSSPP flatpak, without needing a visible desktop,
# and leaves PSPDX.LOG and every screenshot the client took (shot.png,
# shot0.png, shot1.png, shot2.png) next to this script.
#
#   sh app/run-ppsspp.sh [seconds] [--sweep] [--keys FILE] [--slow [KB/s]]
#
# --keys FILE scripts input: one "<ms> <key>" per line, counted from the
# moment the catalog is up; keys are up, down, cross, circle, square,
# triangle, start, select and shot, the last of which leaves a settled
# screenshot as shot1.png.
#
# --slow [KB/s] paces the client's network down to a PSP-1004's, 180 KB/s by
# default, which is what its 802.11b radio and TCP stack were measured at.
#
# By default the emulator's stick gets a fixed test seed, so the client does
# what it does on a PSP after its first run: load the seed, skip the sweep,
# and be at the catalog a few seconds in. The seed is the string below and
# obviously not entropy; nothing from a rig run is fit to sign anything.
#
# --sweep replays testdata/sweep.trace through the entropy screen instead,
# for working on that screen. That trace is a hand drawn from /dev/urandom by
# tools/sweep-trace.py: headings by lot until the screen has its 128 bits,
# then X, so the screen is over in about six seconds. sweep-full.trace is a
# human at the stick and what the rate in logic/entropy.h was measured on.
# A replayed sweep never writes a seed -- the client refuses, since replayed
# input is not entropy either -- so the next
# default run seeds itself again.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
MS="$HOME/.var/app/org.ppsspp.PPSSPP/config/ppsspp"
# The client keeps everything a rig hands it or takes from it under one
# directory on the stick; nothing is left in the root any more.
DBG="$MS/PSP/PSPDX/DEBUG"
LOGS="$MS/PSP/PSPDX/LOGS"
SEED="$MS/PSP/PSPDX/CRYPTO/seed.bin"
SECS=25
SWEEP=0
KEYS=""
SLOW=""
while [ $# -gt 0 ]; do
	case "$1" in
		--sweep) SWEEP=1 ;;
		--keys) KEYS="$2"; shift ;;
		--slow) case "${2:-}" in ''|*[!0-9]*) SLOW=180 ;; *) SLOW="$2"; shift ;; esac ;;
		*) SECS="$1" ;;
	esac
	shift
done

mkdir -p "$MS/PSP/GAME/pspdx"
# EBOOT names another build to run instead of the one beside this script:
# a campaign copies its own aside, so a rebuild in app/ meanwhile -- by a
# hand, by another agent -- cannot swap the client under it.
cp "${EBOOT:-$HERE/EBOOT.PBP}" "$MS/PSP/GAME/pspdx/EBOOT.PBP"

# PPSSPP ships the PSP system fonts but does not mount flash0 for the guest,
# so put one where the client's fallback looks. Test rig only: on hardware the
# font comes out of the PSP's own firmware and nothing is copied.
FONTS="$(flatpak info --show-location org.ppsspp.PPSSPP 2>/dev/null)/files/share/ppsspp/assets/flash0/font"
if [ -f "$FONTS/ltn8.pgf" ]; then
	mkdir -p "$DBG/font"
	cp "$FONTS/ltn8.pgf" "$DBG/font/ltn8.pgf"
fi

# Clips the catalog repo holds but the published catalog does not link yet
# go straight into the client's cache, which it consults before any URL.
# That is how the player gets exercised ahead of a deploy.
for clip in "$HERE"/../catalog/apps/*/video.mp4; do
	[ -f "$clip" ] || continue
	id="$(basename "$(dirname "$clip")")"
	mkdir -p "$MS/PSP/PSPDX/cache"
	cp "$clip" "$MS/PSP/PSPDX/cache/$id.mp4"
done

mkdir -p "$DBG" "$LOGS" "$MS/PSP/PSPDX/CRYPTO"
if [ "$SWEEP" = 1 ]; then
	cp "$HERE/testdata/sweep.trace" "$DBG/PSPDX.TRACE"
	touch "$DBG/PSPDX.REPLAY"
	rm -f "$SEED"
else
	rm -f "$DBG/PSPDX.REPLAY"
	# 20 bytes, the pool size. Only written when missing: the client ratchets
	# the file forward on every run, and a rig that kept resetting it would
	# hide a bug in that.
	[ -f "$SEED" ] || printf 'PSPDX-TEST-SEED-0000' >"$SEED"
fi
rm -f "$LOGS/pspdx.log" "$DBG/PSPDX.BMP" "$DBG/PSPDX0.BMP" "$DBG/PSPDX1.BMP" \
      "$DBG/PSPDX2.BMP" "$DBG/PSPDX.BENCH" "$DBG/PSPDX.KEYS" "$DBG/PSPDX.SLOW"
[ -n "$KEYS" ] && cp "$KEYS" "$DBG/PSPDX.KEYS"
# The emulator borrows the host's network; a PSP-1004 has 802.11b and its own
# TCP stack, which together managed about 180 KB/s. --slow holds the client to
# that, so a film takes as long to arrive here as it does on the hardware.
[ -n "$SLOW" ] && printf '%s' "$SLOW" >"$DBG/PSPDX.SLOW"

# Native: the emulator renders the PSP's own 480x272 and only the window
# scales it, so what is on screen is what a PSP shows, pixel for pixel.
# The ini is rewritten by a running emulator on exit; a rig run that
# overlaps one loses this, which is harmless for a rig.
# The settings and the keyboard map under dev/ppsspp, so that every desk and
# every rig plays the same way.
INI="$MS/PSP/SYSTEM/ppsspp.ini"
if [ -f "$INI" ]; then
	grep -v '^#' "$HERE/../dev/ppsspp/settings" | while IFS='=' read -r key value; do
		key=$(printf '%s' "$key" | sed 's/ *$//'); value=$(printf '%s' "$value" | sed 's/^ *//')
		[ -n "$key" ] && sed -i "s/^$key = .*/$key = $value/" "$INI"
	done
fi
mkdir -p "$MS/PSP/SYSTEM"
cp "$HERE/../dev/ppsspp/controls.ini" "$MS/PSP/SYSTEM/controls.ini"

# In its own session, so that the kill below reaches the emulator inside
# the flatpak sandbox and not only the launcher: an instance that survives
# keeps writing the same files as the next run, and two runs then share one
# log.
# --nosocket=pulseaudio: a rig run has no ear on it. The client's own stream
# is checked through the emulator's DumpAudio, not through the speakers.
# The emulator inside the sandbox is not in the launcher's process group, so
# it is found again by a tag in its environment -- this run's own pid -- and
# only that one is stopped. Whatever else is running, a desk instance or
# someone's own, is not this run's to touch.
RIG="rig-$$"
SDL_VIDEODRIVER=wayland setsid flatpak run --socket=wayland --share=network \
  --nosocket=pulseaudio --env=PSPDX_RIG="$RIG" \
  --filesystem="$MS" org.ppsspp.PPSSPP --fullscreen=0 \
  "$MS/PSP/GAME/pspdx/EBOOT.PBP" >"$HERE/ppsspp.out" 2>&1 &
PID=$!
sleep "$SECS"
for p in $(pgrep -x PPSSPPSDL); do
	tr '\0' '\n' <"/proc/$p/environ" 2>/dev/null | grep -qx "PSPDX_RIG=$RIG" && kill "$p" 2>/dev/null
done
kill -- -"$PID" 2>/dev/null || kill "$PID" 2>/dev/null || true
sleep 2

cp "$LOGS/pspdx.log" "$HERE/PSPDX.LOG" 2>/dev/null || echo "no log written"
# Every shot the client leaves, at the name it left it under: PSPDX.BMP is
# the settled catalog, PSPDX0.BMP the screen a few seconds in, PSPDX1.BMP
# what the scripted keys led to, PSPDX2.BMP a settled row.
rm -f "$HERE"/shot.png "$HERE"/shot0.png "$HERE"/shot1.png "$HERE"/shot2.png
for n in "" 0 1 2; do
	[ -f "$DBG/PSPDX$n.BMP" ] &&
		magick "$DBG/PSPDX$n.BMP" -scale 200% "$HERE/shot$n.png"
done
cat "$HERE/PSPDX.LOG" 2>/dev/null
