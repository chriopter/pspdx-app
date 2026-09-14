# The tune and the key sounds

Everything the client plays is made by `app/audio/synth.c` at run time on
the PSP's audio thread; nothing here is loaded from a file. What is in this
directory is what that code sounds like, rendered once on a desk, so that
a change to the code can be compared against what was chosen.

- `tune.mp3` — the room's music, one full loop (128 s): two passes of the
  64 s chord cycle with different entries and different bells.
- `keys.mp3` — the interface: a scroll down five rows, an open, a done, a
  fail, two seconds apart.

## Rebuilding these

The same code renders to a WAV on the desk:

    cd app
    cc -O2 -I. audio/synth.c audio/music.c audio/cues.c ../dev/render-music.c -lm -o render
    ./render out.wav 138          # 128 s of tune, then the cues in the tail
    ffmpeg -i out.wav -t 128 -c:a libmp3lame -q:a 4 assets/music/tune.mp3
    ffmpeg -i out.wav -ss 128 -c:a libmp3lame -q:a 4 assets/music/keys.mp3

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

The result, A2:

- Six chords of uneven length over 64 s: E add9, C#m11, A maj9#11, F#m9,
  B add9, G#m11, roots E1 to C#2 under a near-sine sub. Each pad voice
  arrives staggered over five seconds in an order that differs per chord;
  outer voices sit at ±0.8 and swap sides each chord. With twelve-second
  decays every chord still rings under the next.
- The pad (`SYNTH_DRIFT`) is a stereo chorus made of the timbre itself:
  fundamental and octave a few cents sharp on the left, flat on the right,
  a slow amplitude LFO per voice with a rate that differs per note.
- The music voices go through their own bus: a two-pole low-pass whose
  cutoff drifts between 450 Hz and 2.5 kHz on two slow sines that never
  line up, plus band-limited noise, about 25 dB under the pads, breathing
  on two other sines.
- Bells (`SYNTH_BELL`, partials 1 : 2 : 2.756 : 5.404, a quarter heard
  direct and the rest through a hall of six combs and four allpasses) on
  chord tones in the D5–A5 region, eight a pass, never on a grid, sides
  alternating. The hall runs only while a bell is sounding and for eight
  seconds after.
- The interface is the same glass, damped (`SYNTH_CHIME`: half the ring, a third of the hall): one strike per key, quieter and single
  during a held scroll, two for an open, four rising for a done, two low
  and close together for a fail (`app/audio/cues.c`). It stays out of the
  music bus so it is not darkened with the pads, and it is not ducked
  under a film.

Cost on the console, measured on the desk render against the tune before
it: about 29 % more time in the synth, 65 KB more static memory for the
hall.

The candidates themselves were not kept; the brief above and the code are
what they came to.
