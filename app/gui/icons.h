#ifndef PSPDX_ICONS_H
#define PSPDX_ICONS_H

#include "gui/gfx.h"
#include "update/catalog.h"

/* The icons in the list: each entry's ICON0, the 144x80 picture every PSP
   bundle carries, kept at half size for a row 32 pixels tall. They are
   fetched one at a time on the media thread, after whatever the card is
   waiting for, and only for the rows on screen -- so a list of fifty costs
   what the viewer scrolls past, not fifty requests up front. The main
   thread says which rows are showing and draws what has arrived. */

/* The catalog the indices below refer to. Entries do not move once the
   catalog is parsed. */
void icons_bind(const struct catalog *catalog);
void icons_reset(void);

/* Main thread: the entries wanted, which a filtered list does not leave in
   one run -- the first visible of them the rows on screen, the rest the
   rows just past either edge, fetched after them so that scrolling on a
   little finds its icons already there. None while the list flies by.
   Returns 1 when that means there is something new to fetch, so the caller
   can wake the media thread. */
int icons_want(const int *index, int count, int visible);

/* Main thread: rows further out, nearest first, whose icons the media
   thread fetches into the cache when it has nothing else to do -- not
   decoded, only kept, so that scrolling there finds them on the stick. */
void icons_ahead(const int *index, int count);

/* Media thread: fetch one of those that is not kept yet. 0 when none is
   left to try. */
int icons_prefetch_one(void);

/* Main thread: the icon of an entry, or NULL while it is not here. */
const struct gfx_texture *icons_get(int index);

/* Media thread: the next row wanted and not yet tried, or -1; the second
   only among the rows on screen. */
int icons_pending(void);
int icons_pending_visible(void);

/* Media thread: fetch, decode and shrink one entry's icon. Blocks. */
void icons_load(int index);

#endif
