---
name: pspdx-promo-video
description: Make the PSPDX tour video - Sony's straight-on product shot of the PSP with its screen replaced by the real app running in PPSSPP (frame dump), and one calm line of text below saying what happens. Use when asked to make, re-shoot, re-cut or update the PSPDX promo, tour, demo, trailer or README video.
---

# PSPDX tour video

## Principles (fixed with the user, 2026-09-28/29)

1. **The real app on screen.** The PSP's screen shows PSPDX itself, run in
   PPSSPP and taken from its frame dump: every frame at 480x272, no stills
   standing in for motion, no mock-ups.
2. **One fixed product shot.** Sony's straight-on marketing shot of the PSP,
   never tilted, never moving. Its screen is replaced exactly. No camera
   moves, no zooms, no drawn console.
3. **The video speaks for itself.** No background animation, grids, glow,
   flying cards, kinetic type, counters or beat.
4. **One calm line below.** Centred under the PSP, Inter Regular in a quiet
   grey, fading softly. Ad lines, not descriptions ("See every app in
   action.", not "its film plays on the card"). PSPDX is not a store: it
   lists homebrew from sources ("Every homebrew source. One list.").
5. **Walk through the app, once each.** Categories, a continuous scroll to an
   app with a film, install, background download, basket, download all, an
   update, all installed, then start the new app and see it boot. About 30-45
   s. Waiting is cut, not sped up. No quick back-and-forth between screens.
6. **Plain white page** around the product shot.

## Files

```text
tour.json            the cut: plate, screen rectangle, kept stretches, captions
takes/tour.keys      the scripted pad for the PPSSPP take (PSPDX.KEYS format)
takes/warm.keys      a warm-up run: Show Unreleased, visit Demo, fill caches
tools/ppsspp_take.sh one PPSSPP run with frame dump -> captures/NAME.avi
tools/tour.py        plate + footage + captions -> MP4 (ffmpeg only)
plate/               the product shot (git-ignored: Sony's image)
fonts/               Inter (OFL, fonts/OFL.txt)
placeholder/         bench screenshots, only for layout tests
takes/tour.take, tools/psp_capture.py, footage.py, fake_remotejoy.py
                     the real-PSP path over remotejoy (see "Hardware")
BUILD.md             how the current video was made
```

`out/`, `captures/`, `footage/` are git-ignored. Never commit or push unless
the user asks.

## The plate

`plate/psp-front-sony.png`: Sony's PSP-2000 front shot (1000x792, white
ground, a (c)2007 SCE line under it that the crop leaves out). `tour.json`:
`plate_crop` [60,190,880,400], `screen` [270,247,460,264] in plate pixels,
`scale` 1.62, `plate_top` 150. A new plate needs its own numbers; check with
`ffmpeg -ss T -frames:v 1` stills.

## Taking the footage in PPSSPP

`tools/ppsspp_take.sh KEYS SECONDS NAME` copies `app/EBOOT.PBP` to the
emulator's stick, puts KEYS in `PSP/PSPDX/DEBUG/PSPDX.KEYS`, turns PPSSPP's
FFV1 frame dump on, runs the emulator **silently on Hyprland workspace 5**
(the user's rule: never in their focus; `WORKSPACE=N` to change; it stops if
the window lands elsewhere), and moves the dump to `captures/NAME.avi`.
`DUMP=False` for warm-ups, `SLOW=KB/s` to hold downloads to a rate.

Things that bit, and the answers:
- **Key times are PSPDX's clock, which is the emulated clock** (counted from
  the catalog's arrival). While PPSSPP unpacks a big ZIP it runs far behind
  real time, so a key "at 600 s" may never come. Place late keys by the
  footage's own time (it is the emulated time too): everything installed at
  ~135 s, so Run at 150 s.
- Scripted keys need 1-1.5 s gaps, more after a tab switch; a key during a
  transition is lost. `up` on the top row wraps to the bottom.
- Circle on the download screen goes to the Basket tab; going back left to
  Homebrews returns to the page left, so it needs two more circles to the
  categories. Download all and Update ask "Do you want to ...?": press cross.
- A lowercase `PSP/PSPDX/tmp` on the emulator stick breaks installs (PPSSPP
  finds it for TMP but cannot rename in it); the script moves it to TMP.
- Unpacking a 669-file ZIP (Extreme Tux Racer) takes minutes in PPSSPP; give
  the run 700 s and `SLOW=800` so the basket and update are queued while Tux
  still downloads.
- Stage the stick: the apps to install absent (their `PSP/GAME` folder and
  `PSP/PSPDX/INSTALLED/<id>.*`), one app at an older release so the update is
  real (its old ZIP's files plus a state.json with the old version and SHA-256),
  a warm-up run first so the lists have their icons.

## Cutting

`tour.json`: `keep` [from, to] seconds of the footage in order, real speed;
`captions` {from, to, text} on the output timeline. Then

```sh
python3 tools/tour.py tour.json captures/tour-emu.avi out/pspdx-tour.mp4
```

Look at single seconds before handing it over, and open it for the user in
the browser. README: 480 px WebP (`fps=12,scale=480:-2`, libwebp q60 loop 0)
as `assets/pspdx-app.webp`; the MP4 (720p) goes on the release.

## Hardware (not used for the current video)

The PSP-1000 has no TV-out. remotejoy can stream the screen over USB
(`psp_capture.py`, 40-58 fps measured), but on 2026-09-28 the stream only
carried the background, not PSPDX's UI, and the bench PSP powers off ~300 s
after joining WLAN. Never use PSPLink `scrshot` (breaks the Media Engine).
