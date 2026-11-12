#ifndef PSPDX_SYNC_H
#define PSPDX_SYNC_H

#include "update/catalog.h"

/* Bringing the network up, fetching the catalog and checking what is out
   of date, on a thread of its own, so the browser is on screen from the
   first frame and says what it is waiting for.

   The catalog is written by that thread and read by the main one; the
   main thread must not touch it until sync_done(). */

enum sync_state {
    SYNC_IDLE,
    SYNC_CONNECTING,
    SYNC_FETCHING,
    SYNC_CHECKING,
    SYNC_DONE,
    SYNC_FAILED
};

int sync_start(struct catalog *catalog);
void sync_set_offline(int on);          /* skip the radio on the next sync */
/* Auto Connect: join this connection (1-10) first, without the system's
   dialog, saying its name at the foot meanwhile; 0 for none. */
void sync_set_profile(int profile, const char *name);
/* Whether the last sync reached the network, or used what was kept. */
int sync_online(void);
/* The link lost after the sync, or found again: online or not from now on,
   for the catalog's fetches as well. */
void sync_set_online(int on);
enum sync_state sync_state(void);
int sync_done(void);                    /* DONE or FAILED */

/* What the status line should say right now. */
const char *sync_message(void);

#endif
