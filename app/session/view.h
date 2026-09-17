#ifndef PSPDX_VIEW_H
#define PSPDX_VIEW_H

#include "update/catalog.h"

/* The browser's model: what the list holds and in what order, with nothing
   of the drawing in it. The shell draws what is here; the main loop walks
   it. */

/* ------------------------------------------------------------------ tabs */

/* The browser shows one tab at a time, chosen by the signs across the top.
   The view is the catalog filtered to the active tab, and a cursor anywhere
   in this program is a row of the view rather than a catalog index -- so
   there is one filter and no second copy of the entries.

   Called whenever the catalog has been rewritten: it works out which tabs
   there are, keeps the active one if it survived, and builds the view. */
void view_rebuild(const struct catalog *catalog);

int view_count(void);
int view_index(int row);      /* view row -> catalog index, -1 if none */
int view_row(int index);      /* catalog index -> view row, -1 if hidden */

/* Counted up every time the rows come to stand for other packages than
   they did -- another tab, another catalog -- so that what draws the list
   can start it from the top and fetch the card afresh exactly then. */
unsigned view_generation(void);

/* The tabs, in the order they are shown and walked, left to right: the
   stick, then Homebrew and the UMD -- what is published -- then the basket
   and the gear at the far edge. The stick and the basket come and go with
   what is in them, and both carry one row that is not a package but the
   whole tab as a thing to do. */
enum view_tab_kind { VIEW_TAB_HOMEBREW, VIEW_TAB_STICK, VIEW_TAB_BASKET,
                     VIEW_TAB_GEAR, VIEW_TAB_UMD };
enum view_tab_kind view_tab_kind(void);

/* A tab is a number: Homebrew, everything the catalogs publish, at zero, and
   the rest below it. */
#define TAB_HOMEBREW  0
#define TAB_BASKET  (-1)
#define TAB_STICK   (-2)
#define TAB_GEAR    (-3)
#define TAB_UMD     (-4)
#define TAB_COUNT   5       /* all of them on screen at once */

/* The tab that is open; the ones on screen, in their order, by position;
   and which position is the open one. */
int view_tab_current(void);
int view_tab_at(int i);
int view_tab_active(void);

/* How many packages on the stick have a newer one published: the updates
   tab's own label and the reason it exists at all. */
int view_updates_waiting(void);

/* Under the store, before the packages, the categories the catalogs name:
   one row each, with how many packages stand in it. Taking one narrows the
   store to that category until O opens it back up. view_index() answers
   VIEW_ROW_CATEGORY minus the category's number for such a row. */
#define VIEW_ROW_CATEGORY (-50)
#define VIEW_CATEGORIES 32
int view_category_count(void);              /* rows on the open store */
const char *view_category(int n);           /* the word, as the catalogs write it */
int view_category_apps(int n);              /* packages in it */
int view_category_open_at(void);            /* the one the store is narrowed to, -1 if none */
void view_category_open(int n);
void view_category_close(void);

/* That row. view_index() answers VIEW_ROW_ACTION for it, which is
   below zero like the no-such-row answer, so anything that only ever wanted
   a package goes on being right by asking for one. */
#define VIEW_ROW_ACTION (-2)

/* Under the gear the rows are not packages but the things this session can
   do to itself: view_index() answers VIEW_ROW_SETTING minus the
   row's number for them, so the one list draws and walks both kinds. */
#define VIEW_ROW_SETTING (-100)
#define VIEW_SETTINGS 4

/* The word on a row under the gear. */
const char *view_setting(int n);

/* What the action row would do, counted over the rows under it: what can be
   fetched, how much of that is a package already installed and current, what
   was passed over for naming no release, and the bytes of the rest. */
struct view_plan {
    int apps;
    int again;
    int skipped;
    unsigned long long bytes;
    int updates;                    /* the updates tab, not the basket */
};
void view_action_plan(struct view_plan *plan);

/* The basket: catalog indices set aside this session, a bit each. It is not
   written to the stick -- what to fetch next is a thought that lasts as long
   as the client is open, and a stale basket after a refresh would point at
   whatever had taken those indices. */
void view_basket_toggle(int index);
void view_basket_forget(int index);
int view_basket_has(int index);
int view_basket_count(void);

/* The tabs worked out again after something inside the catalog changed rather
   than the catalog itself: an install that was the last update takes the
   updates tab away, a basket emptied takes the basket tab. Returns 0 when the
   tab that was active has gone -- the caller is then standing on Homebrew
   with a cursor that means nothing, and puts it back at the top. */
int view_tabs_refresh(void);

/* How many tabs are on screen: none until there is a catalog. */
int view_tab_count(void);

/* L and R: one tab along, wrapping. The view follows; the caller resets
   its cursor. */
void view_tab_move(int step);

#endif
