#ifndef PSPDX_SYNTH_H
#define PSPDX_SYNTH_H

/* Six instruments over one sine table. For the interface: a piano whose
   upper partials die away faster than its fundamental, which is most of
   what makes a struck string sound struck; a glass that swells in and
   rings on evenly; and a pad that takes most of a second to arrive and
   stays for many. For the tune: a drift, which is a pad a few cents sharp
   on the left and flat on the right so the room slowly turns; a sub, the
   floor under it; a shimmer, the drift high up and quieter; and a bell,
   cold and inharmonic, heard mostly through a long hall of its own. The
   tune also goes through a low-pass that breathes over half a minute,
   with a little filtered noise for air. Plain C with no platform in it, so the
   same code renders on a PSP audio thread and into a WAV on a desk.

   Only the audio thread may call into here. Other threads post through
   cues.h. */

/* Pads ring for many seconds and a chord brings five; a dozen voices was
   a pool that every new note had to steal from. */
#define SYNTH_VOICES 32

enum synth_timbre { SYNTH_PIANO, SYNTH_GLASS, SYNTH_PAD, SYNTH_DRIFT, SYNTH_SUB, SYNTH_SHIMMER, SYNTH_BELL,
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
