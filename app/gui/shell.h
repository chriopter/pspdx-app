#ifndef PSPDX_SHELL_H
#define PSPDX_SHELL_H

#include <stddef.h>

#include "update/catalog.h"

/* The catalog browser, drawn with the GE and the system font. Everything the
   user sees after the entropy sweep goes through here; the debug screen stays
   for the failure dump, where a wall of log text is the right answer. */

/* 0 if the firmware font is missing, in which case nothing was initialised
   and the caller should stay on the debug screen. */
int shell_init(void);
void shell_shutdown(void);

/* One frame, paced at 60 Hz by the vblank wait inside. */
void shell_draw(const struct catalog *catalog, int cursor);

/* ------------------------------------------------------------------ tabs */

/* The browser shows one category at a time, chosen by the tabs across the
   top. The view is the catalog filtered to the active tab, and a cursor
   anywhere in this program is a row of the view rather than a catalog
   index -- so there is one filter and no second copy of the entries.

   Called whenever the catalog has been rewritten: it works out which tabs
   have anything in them, keeps the active category if it survived, and
   builds the view. */
void shell_view_rebuild(const struct catalog *catalog);

int shell_view_count(void);
int shell_view_index(int row);      /* view row -> catalog index, -1 if none */
int shell_view_row(int index);      /* catalog index -> view row, -1 if hidden */

/* Two of the tabs are not categories: the updates waiting and the basket
   this session has filled. Both stand to the left of All, both come and go
   with what is in them, and both carry one row that is not a package but the
   whole tab as a thing to do. */
enum shell_tab_kind { SHELL_TAB_CATEGORY, SHELL_TAB_STICK, SHELL_TAB_BASKET,
                      SHELL_TAB_GEAR };
enum shell_tab_kind shell_tab_kind(void);

/* That row. shell_view_index() answers SHELL_ROW_ACTION for it, which is
   below zero like the no-such-row answer, so anything that only ever wanted
   a package goes on being right by asking for one. */
#define SHELL_ROW_ACTION (-2)

/* Under the gear the rows are not packages but the things this session can
   do to itself: shell_view_index() answers SHELL_ROW_SETTING minus the
   row's number for them, so the one list draws and walks both kinds. */
#define SHELL_ROW_SETTING (-100)
#define SHELL_SETTINGS 7
const char *shell_setting(int n);
int shell_view_action(int row);

/* What the action row would do, counted over the rows under it: what can be
   fetched, how much of that is a package already installed and current, what
   was passed over for naming no release, and the bytes of the rest. */
struct shell_plan {
    int apps;
    int again;
    int skipped;
    unsigned long long bytes;
    int updates;                    /* the updates tab, not the basket */
};
void shell_action_plan(struct shell_plan *plan);

/* The basket: catalog indices set aside this session, a bit each. It is not
   written to the stick -- what to fetch next is a thought that lasts as long
   as the client is open, and a stale basket after a refresh would point at
   whatever had taken those indices. */
void shell_basket_toggle(int index);
void shell_basket_forget(int index);
void shell_basket_clear(void);
int shell_basket_has(int index);
int shell_basket_count(void);

/* The tabs worked out again after something inside the catalog changed rather
   than the catalog itself: an install that was the last update takes the
   updates tab away, a basket emptied takes the basket tab. Returns 0 when the
   tab that was active has gone -- the caller is then standing on All with a
   cursor that means nothing, and puts it back at the top. */
int shell_tabs_refresh(void);

/* How many tabs are on screen: one means there is nothing to switch. */
int shell_tab_count(void);

/* L and R: one tab along, wrapping. The view follows; the caller resets
   its cursor. */
void shell_tab_move(int step);

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
   with a title and the choices under it, the cursor on one. takeable[i]
   zero draws that row grey -- the choice exists and cannot be taken, which
   is how a package that has no update says so. keys[i], where not -1, is
   the mark of the key that does the same thing without the menu, drawn at
   the row's end: the menu is where the keys are learned. count 0 slides it
   out again. Like the question, it is drawn here and driven there. */
void shell_menu(const char *title, const char *const *items,
                const unsigned char *takeable, const signed char *keys,
                int count, int cursor);

/* The info band over the dimmed browser: what this session is connected to
   and what it is standing on, and at its foot the things that can be done
   about either -- the catalog fetched again, a list or a repository added
   to where it comes from, one repository fetched and offered, the entropy
   swept again. action is which of them the cursor is on, in that order.
   Drawn while open, and nothing more -- what moves that cursor and what
   takes the row under it is read in the main loop. */
/* The band of facts about the session, which the last row under the gear
   opens. It says and does nothing else; O closes it. */
void shell_info(int open);

/* Idle: the package's own picture rises behind the interface, which stays
   where it is. Nothing is hidden by it. */
void shell_rest(int resting);

/* Square: the band that says everything the catalog knows about one package.
   NULL takes it down. */
void shell_details(const struct app_entry *entry);

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
