#ifndef PSPDX_CABLE_H
#define PSPDX_CABLE_H

#include <stddef.h>

/* The way to the network on a console without Wi-Fi: pspkit-usbnet, a
 * plugin like any other in the store, of which the release carries a copy
 * beside the EBOOT so that the console can get to the store at all.
 * Nothing of it is loaded or installed unasked. Who wants the cable says so
 * once, at the first start or under Options, and then the carried copy is
 * installed through the plugin installer, from the file instead of a
 * download: the same checks, the same record under the plugin's catalog id,
 * so that the store updates it from then on. network/usbnet.h is what the
 * kernel does for it.
 *
 * The answer is the caller's to remember; session/options.h keeps it. */

/* The plugin's id in a catalog, which is the record's. */
#define CABLE_ID "io.github.chriopter.pspkitusbnet"

/* Never asked; Wi-Fi; the cable, with the way through the system's dialog
   still to be said; the cable, once a connection has been made with it
   chosen. */
enum cable { CABLE_UNASKED, CABLE_WIFI, CABLE_USB, CABLE_USB_KNOWN };

/* argv0: the path of the EBOOT, beside which the release's copy is. */
void cable_init(const char *argv0);

/* Whether the plugin is there already: PSPDX has a record of it, turned on
   or off, or the firmware loaded a copy somebody put there by hand. Then
   nothing is asked and nothing done: it is managed like any plugin. */
int cable_installed(void);

/* Whether to ask how to connect: never answered, not installed, the copy
   and the release it is of beside the EBOOT, and a kernel to load it. */
int cable_asks(enum cable choice);

/* A start with the cable chosen and no plugin installed -- its install was
   refused -- loads the carried copy for the session. */
void cable_start(enum cable choice);

/* The cable chosen: the carried copy installed as the plugin where there is
   none, the plugin turned on, and the module loaded at once, so that no
   restart stands between the answer and the connection. Returns 0 when the
   module is loaded. said: one line for what was refused on the way, empty
   when nothing was; a refused install still loads the carried copy. */
int cable_use(char *said, size_t size);

/* Whether the system's dialog has "Hi-Speed USB" to scan for now. */
int cable_ready(void);

/* Before the system's dialog opens, the gateway on the PC is looked for:
   the cable leads nowhere without it, and the dialog cannot say so. The
   plugin looks; one from before it could cannot say. Blocks for up to
   CABLE_LOOK_MS, so the caller gives it a thread and draws meanwhile. */
#define CABLE_LOOK_MS 6000
enum cable_look { CABLE_FOUND, CABLE_NOT_FOUND, CABLE_NOT_SAID };
enum cable_look cable_look(void);

/* What is put to the user after the look. Until the cable has connected
   once: found, whether to connect, with what to choose in the dialog, which
   has no ready connection for the cable; not found, to start the gateway
   and where it is to be had, to look again or leave it; not said, the
   question whether it runs, and then the first. Once it has connected,
   nothing: the dialog opens as it does for Wi-Fi, where another connection
   can be chosen with the PC out of reach. */
enum cable_ask { CABLE_ASK_NOTHING, CABLE_ASK_CONNECT, CABLE_ASK_RETRY, CABLE_ASK_RUNNING };
enum cable_ask cable_after_look(enum cable choice, enum cable_look look);

#endif
