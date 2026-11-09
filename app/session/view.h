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
   stick, then Homebrew -- what is published --, the library in the middle
   of the header -- what is on the stick to be started --, the basket beside
   it, and the gear at the far edge. The stick and the basket come and go
   with what is in them, and both carry one row that is not a package but
   the whole tab as a thing to do. Homebrew, the library and the gear are
   there before any catalog is: the library and the gear need none. */
enum view_tab_kind { VIEW_TAB_HOMEBREW, VIEW_TAB_STICK, VIEW_TAB_BASKET,
                     VIEW_TAB_GEAR, VIEW_TAB_LIBRARY, VIEW_TAB_UMD };
enum view_tab_kind view_tab_kind(void);

/* A tab is a number: Homebrew, everything the catalogs publish, at zero, and
   the rest below it. */
#define TAB_HOMEBREW  0
#define TAB_BASKET  (-1)
#define TAB_STICK   (-2)
#define TAB_GEAR    (-3)
#define TAB_LIBRARY (-4)
#define TAB_UMD     (-5)
#define TAB_COUNT   6       /* all of them on screen at once */

/* The library's rows are not the catalog's: session/library.c reads them
   off the stick, and the view is only told how many there are, so that a
   cursor on the library is walked and remembered like any other.
   view_index() answers -1 for them -- no package. */
void view_library_count(int n);
/* The tab named opened, if it is on screen; 0 if it is not. */
int view_tab_open(int tab);

/* The tab that is open; the ones on screen, in their order, by position;
   and which position is the open one. */
int view_tab_current(void);
int view_tab_at(int i);
int view_tab_active(void);

/* How many packages on the stick have a newer one published: the updates
   tab's own label and the reason it exists at all. */
int view_updates_waiting(void);

/* The store opens with three rows above its packages, one for each way
   it is browsed by: the categories the catalogs name, the tags most apps
   carry, and the sources. view_index() answers VIEW_ROW_BROWSE minus the
   way's number for such a row. Taking one lists its rows in the store's
   place, each with how many packages stand in it, and view_index() answers
   VIEW_ROW_GROUP minus the row's number for those; taking one of them
   narrows the store to what stands in it. O goes back a step at a time. */
#define VIEW_ROW_BROWSE (-10)
#define VIEW_BROWSE 3
#define VIEW_IS_BROWSE(at) ((at) <= VIEW_ROW_BROWSE && (at) > VIEW_ROW_BROWSE - VIEW_BROWSE)
#define VIEW_ROW_GROUP (-50)
#define VIEW_TAGS 128           /* the most tags there are */
#define VIEW_TAG_MIN 2          /* and the fewest apps one stands for */
#define VIEW_CATEGORIES_MORE 12  /* categories the catalogs name beyond the store's own four */
#define VIEW_GROUPS (4 + 1 + VIEW_CATEGORIES_MORE + VIEW_TAGS + 16 + VIEW_GENRES + 16)   /* the UMDs' among them */
enum view_group_kind { VIEW_GROUP_CATEGORY, VIEW_GROUP_TAG, VIEW_GROUP_SOURCE };
int view_group_count(void);                 /* rows there are to browse by, of every way */
const char *view_group_word(int n);         /* the word, as the catalogs write it */
enum view_group_kind view_group_kind(int n);
int view_group_apps(int n);                 /* packages in it */
int view_in_group(const struct app_entry *entry, int n);
int view_group_open_at(void);               /* the one the store is narrowed to, -1 if none */
/* Narrowed to row n, the way it belongs to standing open behind it. */
void view_group_open(int n);
/* Back to the way's rows: the row the group stood on, -1 when none was
   open. */
int view_group_close(void);
/* The ways: the words on their rows, how many rows each lists, which one
   stands open (-1 for none), and back to the store's first rows, where the
   way stood on the row it answers. */
const char *view_browse_word(int k);
int view_browse_rows(int k);
int view_browse_at(void);
void view_browse_open(int k);
int view_browse_close(void);
/* The tags stand open as a cloud, not as a list: their rows are the
   chips, in the order of their words. */
int view_cloud(void);
/* Whether the open list shows its packages as a grid of their icons rather
   than as rows: the store's UMDs, as the PSP's own game menu shows discs. */
int view_grid(void);
/* The UMD tab: the disc images the catalogs publish, one tile a game,
   which no list of the store shows. The group that holds them, and whether
   the UMD tab is the open one. */
int view_umd_group(void);
int view_umd_open(void);
/* The versions of the game an ISO entry is one of -- the entries with its
   "game", EU, US, JP, AS, KR and then the rest -- into out, at most max;
   an entry with none is its own one version. How many there are. */
#define VIEW_VERSIONS 16
int view_versions(int index, int *out, int max);
/* The version a game's tile stands for: one on the stick first, else the
   first by region. */
int view_face(int index);
/* The region in the parentheses an ISO entry's name ends in, or "". */
void view_region(const struct app_entry *entry, char out[8]);
/* The newest packages the store shows in group n, or with browse >= 0 in
   any row of that way: up to max catalog indices, newest first. */
int view_newest(int n, int browse, int *out, int max);

/* The store's first row is the search: taken, a word is typed and the
   store narrows to the packages it is found in -- name, author, tags,
   category, summary or description, case aside. view_search_open()
   answers how many those are and leaves the store as it was when none;
   O goes back to the first rows, onto the search's. */
#define VIEW_ROW_SEARCH (-4)
/* Over the search, while the store shows what was kept and nothing has
   connected yet: the row that connects. */
#define VIEW_ROW_CONNECT (-6)
/* The UMDs' list (SELECT) starts with Search, Genres and Sources, the
   store's way of browsing for the discs; a genre or source chosen there
   narrows the list and the icons alike. */
#define VIEW_GENRES 32          /* the most genres the UMDs' list has rows for */
int view_umd_list(void);
/* SELECT: the store or the UMDs from icons to list and back; 0 elsewhere. */
int view_grid_toggle(void);
/* Whether a group is one of this tab's rows: the UMDs' own on the UMD tab,
   the store's elsewhere. */
int view_group_here(int n);
void view_set_connect(int on);
int view_connect_shown(void);
#define VIEW_SEARCH_SIZE 40
int view_search_open(const char *word);
int view_search_close(void);            /* the row to stand on, -1 when none was open */
const char *view_search(void);          /* the word the store is narrowed to, NULL when none */
const char *view_search_last(void);     /* the word last typed, "" before the first */

/* The order of every list of packages on the store and the stick, turned
   by SELECT. */
enum { VIEW_SORT_NEWEST, VIEW_SORT_NAME, VIEW_SORT_INSTALLED, VIEW_SORTS };
int view_sort(void);
void view_sort_set(int sort);
/* The row of the first package on the open list, under whatever rows
   head it. */
int view_home(void);

/* That row. view_index() answers VIEW_ROW_ACTION for it, which is
   below zero like the no-such-row answer, so anything that only ever wanted
   a package goes on being right by asking for one. */
#define VIEW_ROW_ACTION (-2)

/* Under the gear the rows are not packages but the things this session can
   do to itself, the first of them the way out to the home menu and then the UI's mode, flipped where it stands: view_index() answers VIEW_ROW_SETTING minus the
   setting's number (enum view_setting) for them, so the one list draws and
   walks both kinds. Network is a level of its own: the ways to the
   catalogs and how the radio is used. */
#define VIEW_ROW_SETTING (-1000)     /* below every VIEW_ROW_GROUP */
enum view_setting_id {
    SET_XMB, SET_UI, SET_SOURCES, SET_OPTIONS, SET_CABLE, SET_TUNNEL, SET_FILES, SET_ABOUT,
    SET_NETWORK, SET_AUTO, SET_CONNECTION, VIEW_SETTINGS
};

/* The word for a setting under the gear. */
const char *view_setting(int n);
/* Into Network and back out of it; the row to stand on after. */
int view_network_open(void);
int view_network_close(void);
int view_network_shown(void);

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
/* Whether the store lists packages tagged "unreleased": off unless Options
   turns it on, for this run. */
void view_show_unreleased(int on);
int view_unreleased_shown(void);
/* Whether the store leaves this package out: by that switch, and an ISO,
   which only the store's UMDs list. */
int view_hidden(const struct app_entry *entry);

/* How many tabs are on screen: none until there is a catalog. */
int view_tab_count(void);
void view_download_set(int index, unsigned order);
/* The worker-owned job sorts before queued or failed entries; -1 when idle. */
void view_download_running(int index);
int view_download_current(void);
int view_download_count(void);

/* L and R: one tab along, wrapping. The view follows; the caller resets
   its cursor. */
void view_tab_move(int step);

/* Session bookmarks use app IDs so reordering the catalog cannot open
   another package. Call remember before moving, recall after moving. */
void view_remember(int cursor, int details_index, float detail_scroll);
void view_recall(int *cursor, int *details_index, float *detail_scroll);

#endif
