#ifndef PSPDX_REACH_H
#define PSPDX_REACH_H

/* Which sources the last catalog fetch could not load, for the gear's list
   of them: a source that did not answer is marked there, while the apps of
   the ones that did are browsed as ever. Only when none answered does the
   status line say so.

   The fetching thread writes its own copy, emptied by reach_reset() at the
   start of every fetch; the main thread never reads that one. It reads a
   copy taken by reach_take() once sync_done() says the thread is through,
   the way it waits for the catalog itself. */

void reach_reset(void);
void reach_failed(const char *url);
/* A source whose apps came from nothing but the copy saved on the stick. */
void reach_saved(const char *url);
/* A source that answered, live or from its saved copy: how many apps it
   brought into the catalog, and when, in unix seconds -- 0 for a saved
   copy, which was not loaded from the network at all. */
void reach_loaded(const char *url, int apps, unsigned at);

void reach_take(void);
int reach_unreachable(const char *url);
int reach_offline_copy(const char *url);
/* What reach_loaded said about the source: -1 and 0 when it did not answer
   or was not fetched. */
int reach_apps(const char *url);
unsigned reach_loaded_at(const char *url);

#endif
