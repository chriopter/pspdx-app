# Build record

How the promo in `out/` was made, so it can be rebuilt and changed. Newest
entry first; add an entry for every render someone is shown.

## 2026-09-28 — placeholder cut v1 (no hardware footage)

**Output:** `out/pspdx-promo-placeholder.mp4` — 60.0 s, 1920x1080, 60 fps,
H.264 High (x264 `-preset slow -crf 14`, yuv420p, BT.709 tagged), AAC 256 kb/s
stereo 48 kHz, ~38 MB. Soundtrack `out/soundtrack.wav` (-14 LUFS integrated,
true peak -3 dBFS).

**Footage:** placeholder only. 24 real PSP-1000 screenshots (safe-shot, taken
2026-09-27/28 by the bench session, copied to `placeholder/`) stand in for
the hardware take, strung per mark with short crossfades by
`footage.py placeholder`. The scroll is faked by cutting faster and faster
between list screenshots. Every frame with placeholder footage carries the
amber "PLACEHOLDER · hardware still" tag. No video was taken from the PSP:
another session was testing on it and the hardware was off-limits.

**Rebuild:**

```sh
cd assets/promo-video
tools/make.sh placeholder
#   = python3 tools/footage.py placeholder storyboard.json $TMPDIR/pspdx-promo-work/footage-placeholder
#     python3 tools/audio.py --out out/soundtrack.wav --work $TMPDIR/pspdx-promo-work
#     python3 tools/render.py --footage $TMPDIR/pspdx-promo-work/footage-placeholder \
#         --work $TMPDIR/pspdx-promo-work --audio out/soundtrack.wav \
#         --out out/pspdx-promo-placeholder.mp4
```

Wall time on this machine (16 threads, 14 render processes): footage 10 s,
soundtrack 5 s, video ~2.5 min (~0.2-0.3 s per frame per process).

**Tools:** ffmpeg n9.0.1 (libx264, ffv1), Python 3.14.7, Pillow 12.2.0,
pycairo 1.29.1 / cairo 1.18.4, Pango 1.58.2 (PangoCairo), librsvg 2.62.3
(GObject introspection), GCC 16.2.1. Fonts: Inter 4.1 (rsms/inter release
zip, SIL OFL 1.1): InterDisplay Black/Bold/SemiBold, Inter Regular/Medium.
App read at pspdx-app `f077227` with the working tree as it stood (app/ not
modified): glyphs `app/assets/marks/src/*.svg`, tune and cues
`app/audio/{synth,music,cues}.c`.

**Pipeline:**
1. `footage.py` → `<take>.mkv` (480x272, 60 fps CFR, FFV1) + `<take>.marks.json`.
2. `render.py` decodes each take once to raw RGB in the work dir, then 14
   processes each render a contiguous chunk of frames: cairo draws the room
   (night gradient, horizon light, stars, a perspective grid on slow water
   whose speed and tint are keyed in `world`), speed streaks and the vector
   PSP-1000; PIL places the PSP picture (integer nearest-neighbour upscale,
   then smooth to size), a padded blurred bloom, backdrop-blurred glass cards;
   Pango lays out type; a quarter-size glow layer is blurred and added; then
   vignette, flashes, fade. Each chunk pipes RGB to its own x264; chunks are
   concatenated with `-c copy` and muxed with the soundtrack.
3. `audio.py` compiles `tools/score.c` against `app/audio` (a stub
   `util/version.h` in the work dir, since the Makefile normally writes it),
   renders the app's own tune at level 0.9 with UI cues (scroll ticks
   accelerating 6→28/s during the scroll, open on page/install, done on
   "Installed.") and glass notes (E major add9, one per card/headline), adds a
   generated 120 BPM kick (6–52 s) with a side-chain duck on the tune,
   off-beat hats (20–52 s), a riser into the drop (3–6 s), impacts (6.0, 35.5,
   55.8 s) and panned whooshes on overlay entrances; tanh soft clip, 0.8 s
   fade, ffmpeg `loudnorm=I=-14:TP=-1:LRA=9`. Seeded, so reproducible.

**Storyboard decisions (storyboard.json):**
- 120 BPM so cuts and card entrances fall on half-second beats; the drop at
  6.0 s is the push into the screen, the white flash and the PSPDX wordmark.
- 0–6 s the console: vector PSP-1000 in rim light, screen powers on into the
  boot (network dialog → Connecting → catalog), "Your PSP. / just got an app
  store." over it.
- 8–15 scroll (screen left, cards right): "1,750+" counting up, "480 rows a
  second." with streaks and the grid racing. 15–20 icons settle (screen
  right): "From the cursor outward." with a callout to the icon column.
- 20–24 app page ("Every app. Its own page.", an "× Install" chip pinned to the
  on-screen hint); 24–29 review page ("See exactly what goes where.", callout
  to "Installs"); 29–35 download: "300 KB/s", "TLS 1.3", "SHA-256 checked.";
  35–38 "Installed." over a scrim.
- 38–52 eight feature cards, 1.7 s apart, alternating sides and heights,
  while the room takes each tab's colour (green, blue, orange, purple) the way
  the app tints its own room per tab.
- 52–56 pull back out to the console: "Every homebrew. / One button."; 56–60
  end card (wordmark, "Free and open source · GPL-2.0",
  github.com/chriopter/pspdx-app), fade.
- Look: colours from app/gui/palette.h (night #020309→#060816, tint
  #508CFF) and sampled from hardware screenshots (surface #070E1B, accent
  #72CFF6 from the progress bar, warm #D0A860 from "Left out", Installed
  green #38D060). Inter stands in for the PSP's system font (Sony's, not
  licensable here); the icon0 "PSPDX" letter-spaced wordmark is echoed.
- No Sony logos or trademarks drawn; the PSP silhouette is generic.

**Claims and their sources:** see SKILL.md "Only claim what the code says".
"1,750+ apps" and "~300 KB/s" come from the user's bench notes of 2026-09-28,
not from a measurement in this build.

**Verified on the desk:** the capture path, with `fake_remotejoy.py` standing
in for the PSP (RGB565 frames, 30 fps): `psp_capture.py takes/dryrun.take`
recorded 164 frames in 5.5 s, `footage.py capture` retimed them to 60 fps with
marks at 0.48/1.68/4.00 s and correct colours. Nothing was verified against
the real remotejoy screen stream.

**Known gaps:** placeholder motion is cuts between stills; the scroll does not
really scroll. The device is a drawing, not a photo. Frame rate, CPU cost and
Media Engine safety of remotejoy's screen stream on this PSP are unmeasured
(SKILL.md step 3, probe).
