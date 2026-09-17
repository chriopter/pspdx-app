#include "text.h"
/*
 * The browser's model, with nothing of the GE in it: the tabs across the
 * top, the catalog filtered to the one that is open, the basket this
 * session has filled, and what the action row would come to. A cursor
 * anywhere in the program is a row of this view; gui/shell.c draws it and
 * main.c walks it.
 */

#include <stdio.h>
#include <string.h>

#include "session/view.h"

/* The tabs on screen, in the order they are shown. Leftmost, where the
   system's own shell keeps its settings, the gear: rows about this session
   and what can be done to it, reached the way a tab is so that they need no
   key of their own. Then the stick -- what is installed, with whatever newer
   is waiting for it at the top -- and the basket this session has filled,
   each only while it holds anything, because what is one's own comes before
   what is merely there to browse. Then Homebrew, every entry the catalogs
   publish whatever its type or tags, and the UMD, which holds nothing yet. */
static int g_tab[TAB_COUNT];
static int g_tabs;
static int g_tab_at;                    /* index into g_tab */
static const struct catalog *g_view_of;
static unsigned char g_view[MAX_APPS];
static int g_view_count;                /* packages; the action row is extra */
static int g_view_action;               /* 1 when row 0 is the action row */
static unsigned g_generation;           /* counted up when the rows stand for other packages */

/* The basket: catalog indices set aside this session, a bit each. */
static unsigned char g_basket[(MAX_APPS + 7) / 8];
static int g_basket_n;

int view_basket_has(int index) {
    if (index < 0 || index >= MAX_APPS) return 0;
    return (g_basket[index >> 3] >> (index & 7)) & 1;
}

void view_basket_toggle(int index) {
    if (index < 0 || index >= MAX_APPS) return;
    g_basket[index >> 3] ^= (unsigned char)(1u << (index & 7));
    g_basket_n += view_basket_has(index) ? 1 : -1;
}

void view_basket_forget(int index) {
    if (view_basket_has(index)) view_basket_toggle(index);
}

static void view_basket_clear(void) {
    memset(g_basket, 0, sizeof(g_basket));
    g_basket_n = 0;
}

/* How many packages on the stick have a newer one published. The number is
   the updates tab's own label and the reason it exists at all, so it is asked
   for rather than remembered. */
int view_updates_waiting(void) {
    int n = 0;
    if (!g_view_of) return 0;
    for (int i = 0; i < g_view_of->count; i++)
        if (g_view_of->apps[i].state == APP_UPDATE) n++;
    return n;
}

/* restart is for a view whose rows now stand for other packages than they
   did: the list goes back to the top and the card is told to fetch afresh.
   A view merely rebuilt under the same tab keeps where it was scrolled to. */
/* The categories the catalogs name, in the order they are first met,
   each with its count: the rows the store opens with. A category is a
   word an author chose, so two spellings of one word are one category
   only when they agree letter for letter, case aside. */
static char g_cat[VIEW_CATEGORIES][PSPDX_CATEGORY_SIZE];
static int g_cat_apps[VIEW_CATEGORIES];
static int g_cats;
static int g_cat_open = -1;

static int same_word(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

static int category_of(const struct app_entry *entry) {
    if (!entry->category[0]) return -1;
    for (int c = 0; c < g_cats; c++)
        if (same_word(g_cat[c], entry->category)) return c;
    return -1;
}

/* The rows the store opens with, for now: the three groups the standard
   names as the console's own, in this order, whatever else the catalogs
   write. A package in another category stands in the list below them and
   under no row. */
static const char *const STORE_CATEGORIES[] = { "game", "demo", "app" };

static void collect_categories(void) {
    g_cats = 0;
    memset(g_cat_apps, 0, sizeof(g_cat_apps));
    for (size_t c = 0; c < sizeof(STORE_CATEGORIES) / sizeof(*STORE_CATEGORIES); c++)
        snprintf(g_cat[g_cats++], sizeof(g_cat[0]), "%s", STORE_CATEGORIES[c]);
    if (!g_view_of) return;
    for (int i = 0; i < g_view_of->count; i++) {
        int c = category_of(&g_view_of->apps[i]);
        if (c >= 0) g_cat_apps[c]++;
    }
    if (g_cat_open >= g_cats) g_cat_open = -1;
}

int view_category_count(void) {
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    return tab == TAB_HOMEBREW && g_cat_open < 0 ? g_cats : 0;
}
const char *view_category(int n) { return n >= 0 && n < g_cats ? g_cat[n] : ""; }
int view_category_apps(int n) { return n >= 0 && n < g_cats ? g_cat_apps[n] : 0; }
int view_category_open_at(void) { return g_cat_open; }

static void build_view(int restart);
void view_category_open(int n) {
    if (n < 0 || n >= g_cats) return;
    g_cat_open = n;
    build_view(1);
}
void view_category_close(void) {
    if (g_cat_open < 0) return;
    g_cat_open = -1;
    build_view(1);
}

static void build_view(int restart) {
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    g_view_count = 0;
    g_view_action = 0;
    if (!g_view_of || g_tabs <= 0) return;
    for (int i = 0; i < g_view_of->count; i++) {
        int take;
        if (tab == TAB_HOMEBREW)
            take = g_cat_open < 0 || category_of(&g_view_of->apps[i]) == g_cat_open;
        else if (tab == TAB_STICK) take = g_view_of->apps[i].state != APP_NOT_INSTALLED;
        else if (tab == TAB_BASKET) take = view_basket_has(i);
        else take = 0;      /* the gear's rows are not packages; the UMD has none */
        if (take) g_view[g_view_count++] = (unsigned char)i;
    }
    /* On the stick, what has something waiting for it stands first, in the
       order the catalog has them; the rest after, likewise. */
    if (tab == TAB_STICK) {
        unsigned char sorted[MAX_APPS];
        int n = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < g_view_count; i++) {
                int waiting = g_view_of->apps[g_view[i]].state == APP_UPDATE;
                if (waiting == !pass) sorted[n++] = g_view[i];
            }
        memcpy(g_view, sorted, (size_t)n);
    }
    /* A tab that is a job as well as a list carries the job itself at
       the top, above the packages it would be done to -- the stick only
       while there is a job on it. */
    g_view_action = tab == TAB_BASKET || tab == TAB_STICK;
    if (!restart) return;
    g_generation++;
}

/* Which tabs there are, in the order they are shown, and where the one named
   by keep ended up. Returns 0 if keep did not survive. */
static int collect_tabs(int keep) {
    int found = 0;
    g_tabs = 0;
    g_tab_at = 0;
    if (!g_view_of || g_view_of->count <= 0) return 0;
    int installed = 0;
    for (int i = 0; i < g_view_of->count; i++)
        if (g_view_of->apps[i].state != APP_NOT_INSTALLED) installed = 1;
    if (installed) g_tab[g_tabs++] = TAB_STICK;
    g_tab[g_tabs++] = TAB_HOMEBREW;
    g_tab[g_tabs++] = TAB_UMD;
    if (g_basket_n > 0) g_tab[g_tabs++] = TAB_BASKET;
    g_tab[g_tabs++] = TAB_GEAR;
    for (int i = 0; i < g_tabs; i++)
        if (g_tab[i] == keep) { g_tab_at = i; found = 1; }
    /* A tab that has gone -- the last thing taken out of the basket -- is
       answered with Homebrew, the one the basket was filled from, not with
       whatever stands first. */
    if (!found)
        for (int i = 0; i < g_tabs; i++)
            if (g_tab[i] == TAB_HOMEBREW) g_tab_at = i;
    return found;
}

void view_rebuild(const struct catalog *catalog) {
    int was = g_tabs ? g_tab[g_tab_at] : 0;
    /* A fetch rewrites the array the basket's indices point into, and row
       seventeen of the new catalog is not the package row seventeen of the
       old one was. Nothing is carried across. */
    view_basket_clear();
    g_view_of = catalog;
    g_cat_open = -1;
    collect_categories();
    collect_tabs(was);
    build_view(1);
}

int view_tabs_refresh(void) {
    int was = g_tabs ? g_tab[g_tab_at] : 0;
    int kept = collect_tabs(was);
    build_view(!kept);
    return kept;
}

int view_count(void) {
    if (g_tabs && g_tab[g_tab_at] == TAB_GEAR) return VIEW_SETTINGS;
    return view_category_count() + g_view_count + g_view_action;
}

static int view_action(int row) { return g_view_action && row == 0; }

int view_index(int row) {
    if (g_tabs && g_tab[g_tab_at] == TAB_GEAR)
        return row >= 0 && row < VIEW_SETTINGS ? VIEW_ROW_SETTING - row : -1;
    int cats = view_category_count();
    if (row >= 0 && row < cats) return VIEW_ROW_CATEGORY - row;
    row -= cats;
    if (view_action(row)) return VIEW_ROW_ACTION;
    row -= g_view_action;
    return row >= 0 && row < g_view_count ? g_view[row] : -1;
}

int view_row(int index) {
    for (int row = 0; row < g_view_count; row++)
        if (g_view[row] == index) return row + g_view_action + view_category_count();
    return -1;
}

enum view_tab_kind view_tab_kind(void) {
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    return tab == TAB_GEAR ? VIEW_TAB_GEAR
         : tab == TAB_STICK ? VIEW_TAB_STICK
         : tab == TAB_BASKET ? VIEW_TAB_BASKET
         : tab == TAB_UMD ? VIEW_TAB_UMD : VIEW_TAB_HOMEBREW;
}

void view_action_plan(struct view_plan *plan) {
    memset(plan, 0, sizeof(*plan));
    if (!g_view_of || !g_view_action) return;
    plan->updates = g_tab[g_tab_at] == TAB_STICK;
    for (int row = 0; row < g_view_count; row++) {
        const struct app_entry *entry = &g_view_of->apps[g_view[row]];
        /* On the stick the job is the updates; what is merely installed is
           not part of it. */
        if (plan->updates && entry->state != APP_UPDATE) continue;
        /* An entry whose release says no size is one a run of installs
           cannot say beforehand what it will download for, and that is not
           one to offer in a single press. Those are counted and left out. */
        /* Nor is what this version cannot install at all, which is not
           skipped for want of a size but never offered. */
        if (entry->unsupported) continue;
        if (!entry->has_release || !entry->release.size) { plan->skipped++; continue; }
        plan->apps++;
        plan->bytes += entry->release.size;
        if (entry->state == APP_CURRENT) plan->again++;
    }
}

int view_tab_count(void) { return g_tabs; }

void view_tab_move(int step) {
    if (g_tabs <= 1) return;
    g_tab_at = (g_tab_at + step + g_tabs) % g_tabs;
    /* Leaving the store opens it back up: a category is a place inside
       the store, not a state the other tabs know about. */
    g_cat_open = -1;
    build_view(1);
}


/* The tab that is open, and the ones on screen in their order: what the
   header names and the row of signs draws. */
int view_tab_current(void) { return g_tabs ? g_tab[g_tab_at] : TAB_HOMEBREW; }
int view_tab_at(int i) { return i >= 0 && i < g_tabs ? g_tab[i] : TAB_HOMEBREW; }
int view_tab_active(void) { return g_tab_at; }

int view_basket_count(void) { return g_basket_n; }

unsigned view_generation(void) { return g_generation; }

/* What the gear holds: the things this session can do to itself, and last
   the band that says what it is. A row here is read and taken the way a
   package's row is, because at this depth nothing is deeper. */
static const char *const SETTING[VIEW_SETTINGS] = {
    T_SET_SOURCES,
    T_SET_SYSTEM,
    T_SET_FILES,
    T_SET_ABOUT,
};

const char *view_setting(int n) {
    return n >= 0 && n < VIEW_SETTINGS ? SETTING[n] : "";
}
