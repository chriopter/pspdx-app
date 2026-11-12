#ifndef PSPDX_LINK_H
#define PSPDX_LINK_H

#include <stddef.h>

/* The Wi-Fi connection after it came: watched while it is there, so that
 * standby or walking out of range is seen and not only felt as fetches
 * that fail; joined again, by Auto Connect's connection without a dialog,
 * or through the system's dialog when that is the way it was made. Until
 * it is back, the store's and the UMDs' first entry says so and is the
 * key to it. The USB cable is not watched: it has no access point to
 * lose. */

enum link_state { LINK_NONE, LINK_UP, LINK_LOST, LINK_JOINING };

/* From the power callback's thread: the console is back from standby. */
void link_note_resume(void);
/* Once a frame. watching: a connection was asked for and the sync is
   through. Says at the foot when it went and when it came back. */
void link_tick(int watching);
enum link_state link_state(void);
/* Whether there was a connection in this session that is gone now: the
   first entry reconnects instead of connecting for the first time. */
int link_was_up(void);
/* X on that entry: Auto Connect's connection joined in the background,
   or else the system's dialog, which blocks until it is closed. */
void link_reconnect(void);
/* Disconnect, asked for: nothing is lost that was let go of. */
void link_forget(void);
/* For a download breaking off: whether to hold its next try, the link
   being down and on its way back or waiting for X. */
int link_waiting(void);

/* The first entry's word and the note beside it, as things stand. */
const char *link_label(void);
const char *link_note(void);

/* After the system's dialog made a connection: remember which, to offer
   it to Auto Connect once nothing else is asked -- only while Auto
   Connect is off and has never been offered. */
void link_offer_check(void);
/* That offer, once: the connection (1-10) and its name, or 0. */
int link_offer_take(char *name, size_t size);

#endif
