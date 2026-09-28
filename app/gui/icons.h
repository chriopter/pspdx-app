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

/* Main thread: fill the cache with every icon of the catalog while idle
   (on), or stop at the next icon (off). The progress through the catalog
   in percent, or -1 once every entry has been gone through. */
void icons_fill(int on);
int icons_fill_progress(void);

/* Main thread: the icon of this entry of the bound catalog, or NULL. */
const struct gfx_texture *icons_get_entry(const struct app_entry *entry);

/* Main thread: the icon of an entry, or NULL while it is not here. */
const struct gfx_texture *icons_get(int index);

/* The icons have a thread of their own, beside the card's: a still or a
   film being fetched for the card never holds up the rows. It is started,
   held and stopped with the card's media thread (gui/preview.c). */
void icons_start(void);
void icons_stop(void);
void icons_poke(void);
/* Held: nothing fetched, the pack written out. begin asks, ready says it
   has been done, end lets it go on. */
void icons_hold_begin(void);
int icons_hold_ready(void);
void icons_hold_end(void);

#endif
