#ifndef PSPDX_PREVIEW_H
#define PSPDX_PREVIEW_H

#include "gui/gfx.h"
#include "update/catalog.h"

/* What is on the card: the still of the selected entry, and over it, once
   it is here, the entry's film. Owns the fetching, the decoding and the
   fades; the shell only asks what to draw. Layout never touches a pixel of
   this, and nothing in here knows where the card is. */

enum preview_state {
    PREVIEW_EMPTY,          /* nothing asked for yet, or on its way */
    PREVIEW_LOADING,        /* a fetch is about to block the frame */
    PREVIEW_SHOWING,        /* frame() has a picture */
    PREVIEW_MISSING         /* the entry has none, or it failed */
};

void preview_init(void);
void preview_shutdown(void);

/* The entry the card should show. Loading waits until the cursor has
   rested a moment, so holding a direction through ten rows does not open
   ten connections -- unless immediately, for the first selection. */
void preview_show(const struct app_entry *entry, int immediately);

/* Once per frame: sends the request out once the cursor has rested and
   picks up what the media thread has finished. Never blocks. */
void preview_tick(void);

/* Wakes the media thread for work that is not the card's: the list's
   icons, once the rows on screen have changed. */
void preview_poke(void);

/* Parks the media thread between fetches, so the network and the asset
   buffer are free for whoever asks -- an install, say -- and lets it go
   again. quiesce waits for a fetch in flight to finish. */
void preview_quiesce(void);
void preview_resume(void);

/* What to draw on the card, bottom to top: the still (or NULL), then the
   film over it (or NULL), each with how far it has faded in (0..255). */
const struct gfx_texture *preview_still(int *alpha);
const struct gfx_texture *preview_film(int *alpha);
enum preview_state preview_state(void);

/* True when nothing is mid-fade or mid-fetch for the current entry. */
int preview_settled(void);

/* True while a film is on the card. */
int preview_playing(void);

#endif
