#ifndef PSPDX_SHELL_H
#define PSPDX_SHELL_H

#include <stddef.h>

#include "update/catalog.h"
#include "update/sources.h"

/* The catalog browser, drawn with the GE and the system font. Everything the
   user sees after the entropy sweep goes through here; the debug screen stays
   for the failure dump, where a wall of log text is the right answer. */

/* 0 if the firmware font is missing, in which case nothing was initialised
   and the caller should stay on the debug screen. */
int shell_init(void);

/* One frame, paced at 60 Hz by the vblank wait inside. */
void shell_draw(const struct catalog *catalog, int cursor);
/* Whether a direction is being held this frame: while it is, the room
   keeps the colour it has, and takes the colour of the row the cursor
   ends on when the key is let go. Rows flying by under a held key would
   otherwise each start a crossfade of their own. */
void shell_hold(int held);

/* The list it draws is the view -- the catalog filtered to the open tab --
   which is session/view.h's: a cursor here is a row of that. */

/* True once nothing is mid-transition: the start fade is over, the selection
   bar has arrived, the screenshot has faded in. What a screenshot of the
   screen should wait for. */
int shell_settled(void);

/* Worst frame phases since the last call, as a line for the log. */
void shell_profile(char *out, int size);

/* Fetches and decodes the selected entry's screenshot once the cursor has
   stopped moving, so holding a direction does not start a download per row.
   Blocks for the length of the fetch. */
void shell_shot_sync(const struct catalog *catalog, int cursor);

/* The line at the bottom: what the client is doing right now. Empty
   returns to the key hints. Cleared by a cursor move, like the install
   result it also carries. */
void shell_status(const char *text);

/* The word that stands in the room while there is no catalog: Connecting
   by default, or what the caller says the wait has become. */
void shell_word(const char *word);

/* A question standing over the browser until it is answered: a title, a line
   under it, and a footer saying which button means what. The shell only draws
   it -- the pad belongs to the main loop, and so does the answer. A null or
   empty title takes the question away again. */
void shell_ask(const char *title, const char *line);

/* The options menu, the system's own: a panel sliding in from the right
   with a title and the choices under it, the cursor on one. on[i] zero
   draws that row grey -- the choice exists and cannot be taken, which is
   how a package that has no update says so. key[i], where not -1, is the
   mark of the key that does the same thing without the menu, drawn at the
   row's end: the menu is where the keys are learned. The menu is the
   caller's: built and walked there, drawn from here through the pointer
   for as long as it is up, so the words it points at have to stay where
   they are until it has slid out. NULL slides it out again. Like the
   question, it is drawn here and driven there. */
#define MENU_ROWS (SOURCES_MAX + 1)     /* the catalogs and Add, the longest of them */
struct menu {
    const char *title;
    const char *item[MENU_ROWS];
    unsigned char on[MENU_ROWS];
    signed char key[MENU_ROWS];
    int count, cursor;
};
void shell_menu(const struct menu *menu);

/* The band of facts about the session, which the last row under the gear
   opens. It says and does nothing else; O closes it. */
void shell_info(int open);
/* The one setting the band offers: the frame rate in the bottom right
   corner, for the run. Toggled from the band, not saved. */
void shell_toggle_fps(void);
/* The other: fake updates for development. While on, every installed
   package is said to have an update waiting, so the update path can be
   walked without a release to walk it with. Not saved. */
void shell_toggle_dev(void);
int shell_dev_updates(void);
int shell_show_fps(void);

/* The next frame without the room's motion -- no water, no stars, no
   backdrop, no bloom: what the shell draws under the firmware's keyboard,
   whose own threads want the CPU. One frame; the caller says it again. */
void shell_light_frame(void);

/* Idle: the package's own picture rises behind the interface, which stays
   where it is. Nothing is hidden by it. */
void shell_rest(int resting);

/* Square: the band that says everything the catalog knows about one package.
   NULL takes it down. */
void shell_details(const struct app_entry *entry);
/* The analog stick while the band is up, -1 pushed up to 1 pushed down, once
   a frame: what the band has to say scrolls, faster the further it is
   pushed, and a hand resting near the centre moves nothing. */
void shell_details_scroll(float push);
/* Up and down on the page: the text a line at a time. */
void shell_details_step(int lines);
/* The same stick over the browser: what the card says about the package
   under the cursor scrolls, the summary and the description. */
void shell_card_scroll(float push);

/* Install progress, drawn over the browser. The two middle ones match the
   callback types install() expects.

   at and of number this install within a run of them -- "2 of 3: Rust
   Raytracer" -- so a batch says where it has got to in the same band a lone
   install uses. Both zero for an install that is only itself. */
void shell_install_begin(const char *name, int at, int of);
void shell_install_phase(void *ctx, const char *phase);
void shell_install_progress(void *ctx, size_t done, size_t total);
void shell_install_end(const char *message);

#endif
