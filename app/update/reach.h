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

void reach_take(void);
int reach_unreachable(const char *url);

#endif
