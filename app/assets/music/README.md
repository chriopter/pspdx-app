# The tune and the key sounds

Everything the client plays is made by `app/audio/synth.c` at run time on
the PSP's audio thread; nothing here is loaded from a file. What is in this
directory is what that code sounds like, rendered once on a desk, so that
a change to the code can be compared against what was chosen.

- `tune.mp3` — the room's music, one whole chord cycle (274 s): two passes
  with different voicings, entries and bells.
- `keys.mp3` — the interface: a scroll down five rows, an open, a done, a
  fail, two seconds apart.

## Rebuilding these

The same code renders to a WAV on the desk:

    cd app
    cc -O2 -I. audio/synth.c audio/music.c audio/cues.c ../dev/render-music.c -lm -o render
    ./render out.wav 284          # 274 s of tune, then the cues in the tail
    ffmpeg -i out.wav -t 274 -c:a libmp3lame -q:a 4 assets/music/tune.mp3
    ffmpeg -i out.wav -ss 274 -c:a libmp3lame -q:a 4 assets/music/keys.mp3

`render` prints the peak; keep it under about 26000 of 32767.

## What it is

Chosen on 2026-09-13 out of three candidates written to a brief -- a
spherical, mystical, Sony-2000s bed under an XMB-like menu, nothing that
sounds like a keyboard being played:

- A, "XMB pads": wide detuned pads through a slowly breathing low-pass, a
  sub floor, faint air, five high shimmers a minute. Chosen.
- B, "dark drone": a held drone with a filter sweep, cold bells far back in
  a long hall, high satellites. Its bells were taken into A.
- C, "choral shimmer": vocal-like pads with slow unison beating, a hall of
  five seconds, one harmonic cycle a minute. Not taken.

A2, the first tune, was a wall of pseudo-strings: four pad voices, a sub,
shimmers and eight bells a pass, ringing so long that 28 to 32 voices sat
on top of each other all the time, nearly everything under 500 Hz. It read
as a cheap orchestra. A3, from 2026-11-08, keeps the place and drops the
orchestra:

- Six modal chords out of one scale, D dorian with a lydian turn on B flat:
  D, B flat, F, C, A minor, G sus. None is a dominant, so nothing cadences.
  Three voices and a soft root each, 19 to 26 s a chord; the second pass
  changes voicings and entries and leaves two roots out, so the cycle comes
  round only after 274 s.
- The pad (`SYNTH_HAZE`) is FM whose index moves slowly, a filter opening
  and closing, with a plain sine seven cents sharp beating against it in
  the same channel: warmth without stereo copies that cancel between the
  PSP's speakers. The root (`SYNTH_ROOT`) is a low tone with a twin three
  cents off.
- Over it, four short electric-piano phrases (`SYNTH_KEYS`, a bright strike
  that settles to near a sine), one to three notes each, every one on a
  loop of its own length (23.3, 29.9, 37.1 and 44.3 s) and skipping rounds.
  All their notes are G A C D E, which sit on every chord, so they meet
  differently each time and never clash: about ten notes a minute.
- Three faint glass bells a pass (`SYNTH_FAR`), heard mostly as room.
- The music has a room of its own: a darkening one-pole into three small
  allpasses feeding the two echoes the interface already uses; two taps of
  it are heard directly, one a side. After 20 s of silence the room is
  emptied and its loop skipped.
- The interface keys are unchanged (`SYNTH_CHIME`, `app/audio/cues.c`).

Cost on the console, against A2: about 37 % fewer instructions a sample on
average (452 against 713), 11 voices sounding on average instead of 29. Peak
24967, 2.4 dB quieter on average, to sit further under the menu.
