#ifndef PSPDX_SYNTH_H
#define PSPDX_SYNTH_H

/* The interface's instruments are additive, over one sine table: a piano
   whose upper partials die away faster than its fundamental, a glass that
   swells in and rings on evenly, a pad, and the chime every key rings.
   The tune's are two-operator FM over the same table, the brightness of
   each a slowly moving index rather than a filter: a haze, a warm pad
   beating gently against a twin a few cents off; a root, the low tone
   under it; keys, an electric piano struck soft; and a far glass, a bell
   heard mostly through the room. The tune goes through a gentle low-pass
   and, by its own send, a short cloud of allpasses before the echoes.
   Plain C with no platform in it, so the same code renders on a PSP audio
   thread and into a WAV on a desk.

   Only the audio thread may call into here. Other threads post through
   cues.h. */

/* Pads ring for many seconds, a chord brings four and the phrases ring
   on under them; a dozen voices was a pool that every new note had to
   steal from. */
#define SYNTH_VOICES 32

enum synth_timbre { SYNTH_PIANO, SYNTH_GLASS, SYNTH_PAD, SYNTH_HAZE, SYNTH_ROOT, SYNTH_KEYS, SYNTH_FAR,
                    SYNTH_CHIME };

void synth_init(int sample_rate);

/* midi note number, velocity 0..1, pan -1 (left) .. 1 (right). Takes the
   quietest voice if none is free. music marks a voice of the tune, which
   the level below applies to; the interface's sounds are not. */
void synth_strike(int note, float velocity, enum synth_timbre timbre, float pan,
                  int music);

/* Where the tune's level should go, 0..1; it eases there over a second or
   two, voices already sounding included. */
void synth_set_music_level(float level);

/* Mixes frames stereo 16-bit interleaved samples into out, replacing what
   was there, and moves time forward by that much. */
void synth_render(short *out, int frames);

#endif
