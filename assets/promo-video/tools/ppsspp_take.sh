#!/bin/sh
# A take in PPSSPP: runs app/EBOOT.PBP in the PPSSPP flatpak with a scripted
# pad (PSPDX.KEYS, "<ms> <button>" counted from the catalog's arrival) and
# PPSSPP's frame dump on (FFV1, every frame at the PSP's own 480x272), then
# moves the dump to captures/NAME.avi.
#
#   tools/ppsspp_take.sh KEYS SECONDS NAME
#
# Stage the emulator's stick first ($MS below), the way takes/tour.take's
# header says. The ini's dump switches are put back off afterwards.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
APP="$REPO/app"
MS="$HOME/.var/app/org.ppsspp.PPSSPP/config/ppsspp"
KEYS="$1"; SECS="$2"; NAME="$3"
[ -f "$KEYS" ] && [ -n "$SECS" ] && [ -n "$NAME" ] || { echo "usage: $0 KEYS SECONDS NAME" >&2; exit 2; }
DBG="$MS/PSP/PSPDX/DEBUG"
INI="$MS/PSP/SYSTEM/ppsspp.ini"

# PPSSPP over a case-sensitive host finds tmp for TMP but cannot rename or
# remove inside it; an install then stops half way (dev/start does the same).
[ -d "$MS/PSP/PSPDX/tmp" ] && [ ! -d "$MS/PSP/PSPDX/TMP" ] && mv "$MS/PSP/PSPDX/tmp" "$MS/PSP/PSPDX/TMP"
mkdir -p "$MS/PSP/GAME/PSPDX" "$DBG" "$MS/PSP/PSPDX/LOGS" "$MS/PSP/PSPDX/CRYPTO"
cp "$APP/EBOOT.PBP" "$MS/PSP/GAME/PSPDX/EBOOT.PBP"
cp "$APP/presets.txt" "$MS/PSP/GAME/PSPDX/presets.txt"
FONTS="$(flatpak info --show-location org.ppsspp.PPSSPP 2>/dev/null)/files/share/ppsspp/assets/flash0/font"
[ -f "$FONTS/ltn8.pgf" ] && mkdir -p "$DBG/font" && cp "$FONTS/ltn8.pgf" "$DBG/font/ltn8.pgf"
SEED="$MS/PSP/PSPDX/CRYPTO/seed.bin"
[ -f "$SEED" ] || printf 'PSPDX-TEST-SEED-NOT-SECRET-0000!' >"$SEED"
rm -f "$DBG/PSPDX.REPLAY" "$DBG/PSPDX.SHOTS" "$DBG/PSPDX.SLOW" "$MS/PSP/PSPDX/LOGS/pspdx.log"
cp "$KEYS" "$DBG/PSPDX.KEYS"
# SLOW=KB/s holds the download to that rate, so a big app takes long enough
# to browse and queue more while it comes in.
[ -n "$SLOW" ] && printf "%s" "$SLOW" >"$DBG/PSPDX.SLOW"

set_ini() { sed -i "s/^$1 = .*/$1 = $2/" "$INI"; }
set_ini InternalResolution 1
set_ini UseFFV1 True
set_ini DumpFrames "${DUMP:-True}"
set_ini DumpVideoOutput False
set_ini DumpAudio False
mkdir -p "$MS/PSP/VIDEO"
before="$(ls "$MS/PSP/VIDEO" 2>/dev/null)"

TAG="take-$$"
RUN="SDL_VIDEODRIVER=wayland flatpak run --socket=wayland --share=network \
  --nosocket=pulseaudio --env=PSPDX_RIG=$TAG \
  --filesystem='$MS' org.ppsspp.PPSSPP --fullscreen=0 \
  '$MS/PSP/GAME/PSPDX/EBOOT.PBP' >'$HERE/captures/$NAME.ppsspp.log' 2>&1"
# Under Hyprland the emulator opens on workspace 5, silently: a take never
# takes the focus from whoever is at the desk. WORKSPACE=N picks another.
PID=""
if command -v hyprctl >/dev/null 2>&1 && [ -n "$HYPRLAND_INSTANCE_SIGNATURE" ]; then
	LAUNCH="$HERE/captures/.launch-$TAG.sh"
	printf '#!/bin/sh\n%s\n' "$RUN" >"$LAUNCH"
	hyprctl eval "hl.exec_cmd(\"sh $LAUNCH\", { workspace = \"${WORKSPACE:-5} silent\" })" >/dev/null
	# Make sure it landed there; a take that opened in front of the user stops.
	for i in 1 2 3 4 5 6 7 8 9 10; do
		sleep 1
		ws=$(hyprctl clients -j | python3 -c 'import json,sys; print(next((str(c["workspace"]["id"]) for c in json.load(sys.stdin) if "PPSSPP" in c["class"] or "PPSSPP" in c["title"]), ""))')
		[ -n "$ws" ] && break
	done
	if [ "$ws" != "${WORKSPACE:-5}" ]; then
		echo "PPSSPP opened on workspace '$ws', not ${WORKSPACE:-5}; stopping" >&2
		pkill -x PPSSPPSDL; exit 1
	fi
else
	setsid sh -c "$RUN" &
	PID=$!
fi
sleep "$SECS"
for p in $(pgrep -x PPSSPPSDL); do
	tr '\0' '\n' <"/proc/$p/environ" 2>/dev/null | grep -qx "PSPDX_RIG=$TAG" && kill "$p" 2>/dev/null
done
[ -n "$PID" ] && { kill -- -"$PID" 2>/dev/null || kill "$PID" 2>/dev/null; } || true
sleep 3
rm -f "$HERE/captures/.launch-$TAG.sh"
set_ini DumpFrames False
set_ini UseFFV1 False
rm -f "$DBG/PSPDX.KEYS"

[ "${DUMP:-True}" = True ] || { cp "$MS/PSP/PSPDX/LOGS/pspdx.log" "$HERE/captures/$NAME.pspdx.log"; exit 0; }
new="$(ls -t "$MS/PSP/VIDEO" | head -1)"
if [ -z "$new" ] || printf '%s\n' "$before" | grep -qx "$new"; then
	echo "no frame dump appeared in $MS/PSP/VIDEO" >&2; exit 1
fi
mv "$MS/PSP/VIDEO/$new" "$HERE/captures/$NAME.avi"
cp "$MS/PSP/PSPDX/LOGS/pspdx.log" "$HERE/captures/$NAME.pspdx.log" 2>/dev/null || true
echo "$HERE/captures/$NAME.avi"
