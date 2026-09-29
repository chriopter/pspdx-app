# How the current tour video was made (2026-09-29)

Output: `out/pspdx-tour.mp4`, 45.2 s, 1920x1080, 60 fps, silent. README and
release copies: 480 px WebP and a 720p MP4 (`pspdx-tour.*` on the release).

## Footage

- PSPDX with the 0.9.1 code (`app/EBOOT.PBP`, built as 0.9.0) in the PPSSPP
  flatpak 1.20.4, native 480x272, FFV1 frame dump, on workspace 5.
- Emulator stick staged: Extreme Tux Racer, Rust Raytracer and PSP Graphics
  Demo not installed; Lux Aeterna installed at v0.2.4 (files of that ZIP and
  a state.json with version 0.2.4 and its SHA-256) so the v0.2.6 update is
  real; picture pack cleared, then `takes/warm.keys` (DUMP=False, 200 s).
- Take: `SLOW=800 sh tools/ppsspp_take.sh takes/tour.keys 720 tour-emu`.
  Flow: Show Unreleased, tabs, Game, scroll down and back, Tux Racer page,
  install, circle to background, Demo: Rust Raytracer and PSP Graphics Demo
  into the basket, Download all (yes), Installed: 1 update (yes), wait,
  Basket: Tux Racer, Run, yes. Dump: 181 s.

## Cut (`tour.json`)

| footage s | shows | line |
|---|---|---|
| 38.9-54.0 | categories, Game, scroll, back to Tux Racer's film (one shot) | Every homebrew source. One list. / See every app in action. |
| 57.0-61.0, 65.4-66.8 | page, install, download screen, list while it downloads | Install with one button. / Downloads keep running in the background. |
| 74.4-77.2, 80.4-83.0 | two demos into the basket | Fill your basket. |
| 86.9-89.5 | Download all, confirm | Get them all at once. |
| 94.6-98.4 | Installed: 1 update, confirm | Updates in one press. |
| 134.5-137.0 | basket, all four ticked | All installed. |
| 149.2-153.6 | Tux Racer page, Run, confirm | Start it right from PSPDX. |
| 157.8-163.8 | Tux Racer boots to its menu | PSPDX. Install and update homebrew, right on your PSP. |

`python3 tools/tour.py tour.json captures/tour-emu.avi out/pspdx-tour.mp4`
(ffmpeg overlay + drawtext, crf 18).

## Found on the way

- PSPDX held its picture threads for the whole download queue, so rows
  browsed during a download had no icons even when cached. Fixed in 0.9.1:
  while downloads run, the rows on screen are served from the cache or the
  app's own EBOOT; only network fetches wait.
- The rig's scripted keys did not reach the download screen; they do now.
