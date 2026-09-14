#ifndef PSPDX_ACTIONS_H
#define PSPDX_ACTIONS_H

#include <stddef.h>

#include "update/catalog.h"

/* What the session does: installs, removing, starting a package, fetching
   the catalog again, the sweep, the cache and the reset. Each holds the
   loop until it is over. The questions that stand before them are
   session/questions.h's. */

/* The catalog every action works on -- main.c's, handed over once, and
   read back by the questions and the options that are about its entries. */
void actions_init(struct catalog *catalog);
struct catalog *actions_catalog(void);

/* The catalog to draw: the one handed over once the sync has brought it,
   an empty one until then. */
const struct catalog *shown(void);

/* The log, and the catalog's raw response once it is there. */
void dump_diagnostics(void);

/* Frames until the shell has settled, then a screenshot to path. */
void screenshot_settled(int cursor, const char *path);

/* One package fetched and unpacked, the band saying so meanwhile. index is
   a catalog index; at and of place the install in a run of them, both zero
   for one that is only itself; screenshot takes PSPDX2.BMP after. Returns
   the installer's result, 0 when the package is on the stick. */
int install_app(int index, int screenshot, int at, int of);

/* Set when PSPDX has replaced itself: the loop asks to restart once the
   install that did it, or the batch it was the last of, is over. Hands
   over the catalog index and the version once, -1 while none waits. */
int restart_take(char *version, size_t size);

void uninstall_app(int index);
void launch_app(int index);

/* The answer to ASK_ASIDE: the directory in the release's way renamed to
   .bak. 0 when it moved. */
int set_aside(int index);

/* The action row taken: everything the tab holds, one after another. */
void install_all(void);

/* The INBOX's .pspdx files, one after another. */
void install_inbox(void);

/* The cursor after anything that changed what the tabs hold. */
void view_settled(int *cursor);

/* The rig's unattended install: PSPDX.INSTALL names a repository. Returns
   its catalog index, -1 without the file or the package. */
int auto_install_index(void);

/* The catalog fetched again, the cursor's package remembered in keep to be
   found again once it is back; synced and refreshing are the loop's. */
void refetch_now(int cursor, char *keep, size_t keep_size, int *synced,
                 int *refreshing);

/* Reads a source from the keyboard and adds it; install asks for one
   repository and remembers it as wanted. Returns 1 when the catalog should
   be fetched again, 0 when there is nothing new. */
int type_source(int install);

/* The repository "Install from GitHub" typed, while the catalog it should
   turn up in is fetched: its URL, empty while none is waited for, and
   owner/repo for the status line. */
const char *wanted_url(void);
const char *wanted_name(void);
void wanted_forget(void);

void sweep_again(void);
void clear_cache(void);
void reset_completely(void);

#endif
