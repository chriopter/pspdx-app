#ifndef PSPDX_PLAYER_H
#define PSPDX_PLAYER_H

#include <stddef.h>

/* The PSP's own movie decoder, sceMpeg on the Media Engine, fed a PSMF out
   of RAM and decoding straight into a texture. The one file in video/ that
   knows it is on a PSP.

   Every call into sceMpeg blocks for as long as the Media Engine takes --
   ten to fifteen milliseconds a picture, most of a frame -- and the library
   is not thread-safe, so all of it lives on one thread of its own below the
   interface: opening, decoding, looping and closing. The interface only
   picks up pictures that are already finished. */

/* Starts that thread, which opens the stream and from then on decodes at
   the film's own thirty a second, alternating between the two frame
   buffers: 32-bit ABGR, stride pixels per row, at least the stream's width
   by height. Returns 0 when the thread is running; the stream itself is
   opened on it, so player_failed() is what says the film played. psmf must
   stay where it is until player_stop() returns. */
int player_start(const unsigned char *psmf, size_t len, void *frame_a,
                 void *frame_b, int stride);

/* The picture finished since the last call, or NULL when there is none.
   What it returns is the caller's to draw until the next call hands back
   another one; the decoder writes the other buffer meanwhile. */
void *player_take(void);

/* True once the film has turned out to be unplayable. */
int player_failed(void);

/* Stops the thread and lets go of everything, after any decode in flight
   has finished. Safe when nothing was started. */
void player_stop(void);
void player_pause(int on);

/* Loads the firmware's AVCODEC module, once, for whichever side asks
   first: the film's decoder sits on it and so does the card's sound, and
   the firmware refuses to load a module a second time. Returns 1 when it
   is there. */
int player_avcodec_up(void);

#endif
