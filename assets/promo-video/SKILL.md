---
name: pspdx-promo-video
description: Produce the 60-second PSPDX launch video from real PSP hardware footage - screen streamed over USB by remotejoy while a scripted take drives the pad, composited with kinetic, glass-card overlays in PSPDX's own look (night-blue room, perspective grid, Inter type, blue glow) and a soundtrack built from the app's own synth. Use when asked to make, re-shoot, re-cut, restyle or update the PSPDX promo, launch, trailer or ad video, or to capture video from the PSP screen.
---

# PSPDX promo video

Everything lives in this directory (`assets/promo-video/`). `BUILD.md` records
how the current sample was made, with versions and decisions; update it
whenever you render a new version.

```text
storyboard.json   the cut: shots (which take/mark plays when), camera, room,
                  overlays, fx, audio cues, placeholder stills per mark
tokens.json       colours, type, card and motion tokens (from app/gui/palette.h
                  and hardware screenshots)
takes/hero.take   the pad script for the one hardware take the cut uses
takes/probe.take  a short offline take to vet the stream before shooting
takes/dryrun.take a desk-only take for fake_remotejoy.py
tools/psp_capture.py   screen stream + pad input over one remotejoy socket
tools/footage.py       capture -> 60 fps FFV1 + marks; or placeholder stills -> same
tools/render.py        compositor (cairo + Pango + PIL -> ffmpeg), parallel chunks
tools/audio.py + score.c   soundtrack: the app's tune/cues + generated pulse
tools/fake_remotejoy.py    simulated PSP for dry-running the capture path
tools/make.sh          end to end
placeholder/      real PSP-1000 screenshots (safe-shot) standing in for footage
fonts/            Inter 4.1 (OFL, fonts/OFL.txt)
```

Needs on the host: python3 with Pillow, pycairo, PyGObject (Pango, PangoCairo,
Rsvg typelibs), ffmpeg with libx264, a C compiler. No pip installs, no
network. `app/` is only read (glyph SVGs in `app/assets/marks/src`, audio code
in `app/audio`). Outputs go to `out/` and `footage/` (git-ignored); scratch to
`$TMPDIR/pspdx-promo-work`. Never commit or push unless the user asks.

## 1. Render without hardware (always works)

```sh
tools/make.sh placeholder             # 1080p60, ~2.5 min on 16 cores
tools/make.sh placeholder --preview   # 960x540 at 30 fps, faster
python3 tools/render.py --footage $TMPDIR/pspdx-promo-work/footage-placeholder \
    --still 6.4,17.5,36.7 --out $TMPDIR/look.mp4     # PNGs of single moments
```

Placeholder frames carry a small amber "PLACEHOLDER · hardware still" tag
under the screen; real footage has none. Judge a change with `--still` first
(seconds, not minutes), then a `--preview` render, then the full one. Look at
the output: a contact sheet is
`ffmpeg -i out/X.mp4 -vf "fps=2,scale=320:-1,tile=8x15" -frames:v 1 sheet.jpg`.

## 2. How the PSP screen gets to the desk (the chosen path)

PSP-1000 has no TV-out (component/composite out starts with the PSP-2000), so
there is no HDMI/capture-card path on this bench. The chosen path is
**remotejoy's screen stream**: the same `remotejoy.prx` the bench already loads
for pad input hooks `sceDisplaySetFrameBuf` and, after a `TYPE_SCREEN_CMD`,
sends every displayed frame over USB (`JoyScrHeader {magic, mode, size, ref}`
then pixels; `ref` is the vblank counter). `psp_capture.py` speaks that
protocol on the same TCP socket it uses for buttons (usbhostfs_pc serves
127.0.0.1:10004 to one client), so capture and input are one process and the
marks in the take line up with the footage.

- Default 16-bit (VFPU copy to RGB565, 261 KB/frame); `--full` 32-bit
  (522 KB/frame); `--half` 240x136. `--drop N` skips vblanks.
- Frame rate is unmeasured on this bench (USB 2.0 bulk through PSPLink).
  Expect 20-40 fps at 16-bit; `footage.py` retimes to 60 fps by vblank, so a
  low rate means held frames, not wrong timing.
- Risks, unverified on hardware: remotejoy's screen thread runs at priority 16
  and copies each frame on the PSP, which may cost PSPDX frames; it also writes
  its buffer at 0x08780000 after lifting DDR protection. PSPLink `scrshot`
  broke the Media Engine on this bench
  (psp-devbook `tools/psplink/scrshot-media-engine.md`), and PSPDX uses the
  ME for ICON1 previews and ATRAC. So the probe in step 3 is required before
  a real take.

Fallbacks, best first:
1. **PSPDX's own film**: `PSP/PSPDX/DEBUG/PSPDX.RECORD` on the stick makes the
   app write every 4th frame as BMP to `PSP/PSPDX/DEBUG/PSPDX_REC/` (15 fps,
   app/main.c `record_frame`). Exact pixels and no USB, but on real hardware
   each ~390 KB Memory Stick write will stall frames (unmeasured). Convert with
   `ffmpeg -framerate 15 -i M%05d.BMP` into a take .mkv and write the marks
   json by hand.
2. **PPSSPP B-roll**: `dev/start --mock` and `dev/rig` (scripted keys from
   `PSPDX.KEYS`) give deterministic, perfect footage, but it is the emulator;
   label it so, never pass it off as the hardware.
3. **Camera plate**: a locked-off phone shot of the physical PSP-1000 (60 fps,
   no flicker at 1/60 s) for the opening and closing hero moments, with the
   screen replaced by the capture in an editor. render.py draws a vector
   PSP-1000 instead today; a photo plate would need a 4-corner warp added.

## 3. Hardware session (only when the user says the bench is free)

Ask before touching the PSP: another session may be testing on it. Leave the
relay on afterwards. Bench facts: PSP-1000 6.61 ARK under PSPLink,
`usbhostfs_pc` exporting host0:, `pspsh` at `~/.local/opt/pspdev/bin/pspsh`,
remotejoy built with psp-devbook's `remotejoy-scan-for-jal-660.patch`. The
PSP powers off ~300 s after it joins WLAN (router timer): every online take
must finish inside 4.5 min from connect; `psp_capture.py --max 260` refuses
longer takes. It also sleeps ~5:15 into a remotejoy session (auto-sleep, not a
crash). Never use `scrshot`; stills come from safe-shot.

1. **Dry-run on the desk** (no PSP): 
   `python3 tools/fake_remotejoy.py placeholder --port 10044 &`
   `python3 tools/psp_capture.py takes/dryrun.take $TMPDIR/dry/cap --port 10044`
   `python3 tools/footage.py capture $TMPDIR/dry/cap $TMPDIR/dry/dry`
2. **Probe** (PSP, offline, ~40 s): load remotejoy, start PSPDX, cancel the
   network dialog (browse from cache). Turn on Options -> Show FPS. Capture a
   short take that opens an app page with an ICON1 preview:
   `python3 tools/psp_capture.py takes/probe.take captures/probe`, then
   `python3 tools/footage.py capture captures/probe footage/probe`.
   Check: captured fps printed by `footage.py`; PSPDX's FPS
   counter in the footage stays at 60 (Mercy) or 30 (Baked); the preview video
   plays; `pspdx.log` has no ME/decoder errors. Try `--full` once and compare.
   If the preview stalls or the app hangs: stop, power-cycle via the relay, and
   switch to fallback 1. Record the result in BUILD.md.
3. **Rehearse** offline: run a copy of `takes/hero.take` whose connect
   `tap cross` is `tap circle` (cancel: browse the cached catalog; the
   install then fails, which is fine) to find the lines marked
   REHEARSE: the rows from the top of "Game" to the install target, the tab
   order. The install target must be an app whose zip triggers the "Check
   before installing" page (two or more EBOOT.PBP or non-UTF-8 names) and is
   small (~2 MB). PSP Bound (2.1 MB, 3 EBOOT.PBP) qualified on 2026-09-28.
4. **Shoot**: make sure the target is not installed; then, with remotejoy
   loaded and PSPDX not running:
   `python3 tools/psp_capture.py takes/hero.take captures/hero`
   The take launches PSPDX itself (`sh pspsh -e "ldstart ms0:/PSP/GAME/PSPDX/pspdx.prx"`,
   the PRX build the bench's deploy script copies there), connects,
   installs, scrolls, flips tabs: ~75 s plus connect time. Do two takes if the
   budget allows (reset between them: delete the app, `reset`, reload
   remotejoy).
5. **Clean up**: delete the installed target (Installed tab -> triangle ->
   Delete), stop streaming (the script sends screen-off itself), leave relay on.

## 4. Cut the real one

```sh
tools/make.sh real captures/hero            # footage/hero.mkv + marks, then render
```

`footage.py` prints each mark's second in the footage. In `storyboard.json`
adjust per shot: `from` (seconds after the mark), `speed` (e.g. 1.5 to hurry
the download), and the shot boundaries if a moment runs long. Overlays are
timed to the cut, not the footage, so they stay put. Match the `world` tint
keys (38-52 s) to the tab order that was actually shot: Installed green
#38D060, Homebrews blue #508CFF, UMDs orange #E08A3C, gear purple #9A48E0.
Check with `--still` at every shot boundary, then render.

## Changing the film

- Words: `overlays[]` in storyboard.json. Kinds: `headline` (words rise
  through masks; `|` breaks lines; `accent` words get the blue gradient;
  `scrim`), `card` (glass panel from `side` left/right; `label`, `icon` = a
  glyph name from app/assets/marks/src, `title` or `stat` with `{n}` +
  `count` to count up, `sub`), `chip` (pill; `psp: [x,y]` pins it to a point
  on the PSP screen), `callout` (line from the current card to `psp: [x,y]`),
  `wordmark`, `endcard`.
- Only claim what the code says. Sources for the current claims: scroll ramp
  10..480 rows/s and "10,000 in under half a minute" (app/main.c
  `scroll_steps`), icons from the cursor outward (gui/shell.c), icon pack
  (gui/icons.c), review page and its three rows (text.h `T_LAYOUT_*`), SHA-256,
  journal and recovery, TLS 1.3 ChaCha20 first, 60 FPS Mercy default (README),
  archive.org mirrors (install/install.c), Shift-JIS/CP437 (install/zipread.h).
  "1,750+ apps" and "~300 KB/s" are bench numbers the user gave on 2026-09-28;
  re-check them against the live catalog and a measured download before
  release.
- Look: tokens.json. Pacing: 120 BPM, drop at 6.0 s; keep cuts and card
  entrances on half-second beats so they land with the kick.
- Sound: `audio` in storyboard.json (kick/hat ranges, riser, impacts, UI cues
  from the app's own `cues.c`, glass notes on card entrances). The bed is
  PSPDX's own tune rendered by its synth; nothing is sampled.
- New format (e.g. 9:16 for socials): a second storyboard with its own camera
  and overlay positions; render.py's design space is 1920x1080, so add a size
  switch there rather than scaling a 16:9 render.
