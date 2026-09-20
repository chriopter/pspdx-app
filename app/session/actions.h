#ifndef PSPDX_ACTIONS_H
#define PSPDX_ACTIONS_H

#include <stddef.h>

#include "update/catalog.h"

/* What the session does: installs, removing, starting a package, fetching
   the catalog again, the sweep, the cache and the reset. Downloads run
   asynchronously; their dialogs and completion updates stay on the main thread. The questions that stand before them are
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

/* Queue hooks, main thread only: dialogs and publishing a worker result. */
int actions_download_device(const struct app_entry *entry, char out[5]);
int actions_download_connect(void);
int actions_download_folder(const struct app_entry *entry, const char *dev, int row);
void actions_download_complete(int index, struct app_entry *prepared,
                               const struct install_report *report, int rc, unsigned seconds);

/* Set when PSPDX has replaced itself: the loop asks to restart once the
   install that did it, or the batch it was the last of, is over. Hands
   over the catalog index and the version once, -1 while none waits. */
int restart_take(char *version, size_t size);

void uninstall_app(int index);
void launch_app(int index);

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
/* Why a repository named owner/repo is not in the catalog, as the status
   line says it, out of what catalog_refused gave. */
void refused_line(int why, const char *name, char *out, size_t size);
const char *wanted_name(void);
void wanted_forget(void);

void sweep_again(void);
void clear_cache(void);
void reset_completely(void);

#endif
