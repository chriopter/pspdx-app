#include "text.h"
/*
 * The browser's model, with nothing of the GE in it: the tabs across the
 * top, the catalog filtered to the one that is open, the basket this
 * session has filled, and what the action row would come to. A cursor
 * anywhere in the program is a row of this view; gui/shell.c draws it and
 * main.c walks it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
static unsigned short g_view[MAX_APPS];
static int g_view_count;                /* packages; the action row is extra */
static int g_view_action;               /* whether the view has an action row */
static unsigned g_generation;           /* counted up when the rows stand for other packages */

/* The basket: catalog indices set aside this session, a bit each. */
static unsigned char g_basket[(MAX_APPS + 7) / 8];
static int g_basket_n;
static unsigned g_downloads[MAX_APPS];
static int g_download_running = -1;
void view_download_running(int index) { g_download_running = index; }
int view_download_current(void) { return g_download_running; }
void view_download_set(int index, unsigned order) {
    if (index >= 0 && index < MAX_APPS) g_downloads[index] = order;
}
int view_download_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_APPS; i++) n += g_downloads[i] != 0;
    return n;
}

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

/* The rows the store opens with, to browse it by: a category, a tag or a
   source each, with how many packages stand in it. A category or a tag is
   a word an author chose, so two spellings of one word are one row only
   when they agree letter for letter, case aside. */
struct group {
    char word[PSPDX_CATEGORY_SIZE];
    int apps;
    unsigned char kind;
    unsigned short sources;         /* a source row: the lines of sources.txt it stands for, a bit each */
};
static struct group g_group[VIEW_GROUPS];
static int g_groups;
static int g_group_open = -1;
static int g_browse = -1;               /* the way open, -1 at the store's first rows */
/* The rows of the way open, as group numbers, in their order. */
static unsigned char g_way[VIEW_GROUPS];     /* VIEW_GROUPS < 256 */
static int g_ways;

static int same_word(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

/* Packages their authors tag "unreleased" stay out of the store unless
   Options shows them, for this run. The stick lists what is installed
   either way, so nothing on it goes out of reach. */
static int g_show_unreleased;
static int has_tag(const char *tags, const char *word) {
    size_t n = strlen(word);
    for (const char *p = tags; *p; p += *p == '\n') {
        size_t k = strcspn(p, "\n");
        if (k == n && !strncmp(p, word, n)) return 1;
        p += k;
    }
    return 0;
}
/* The same, case aside: what a tag row stands for. */
static int has_tag_word(const char *tags, const char *word) {
    char one[PSPDX_CATEGORY_SIZE];
    for (const char *p = tags; *p; p += *p == '\n') {
        size_t k = strcspn(p, "\n");
        if (k < sizeof(one)) {
            memcpy(one, p, k);
            one[k] = '\0';
            if (same_word(one, word)) return 1;
        }
        p += k;
    }
    return 0;
}
int view_hidden(const struct app_entry *entry) {
    return !g_show_unreleased && has_tag(txt(entry->tags), "unreleased");
}

/* The row an entry stands under. "application" is what some catalogs call
   an app, and stands under the same row. */
static int category_of(const struct app_entry *entry) {
    if (!entry->category[0]) return -1;
    const char *word = same_word(entry->category, "application") ? "app" : entry->category;
    for (int c = 0; c < g_groups; c++)
        if (g_group[c].kind == VIEW_GROUP_CATEGORY && same_word(g_group[c].word, word)) return c;
    return -1;
}
int view_in_group(const struct app_entry *entry, int n) {
    if (n < 0 || n >= g_groups) return 0;
    const struct group *g = &g_group[n];
    if (g->kind == VIEW_GROUP_CATEGORY) return category_of(entry) == n;
    if (g->kind == VIEW_GROUP_TAG) return has_tag_word(txt(entry->tags), g->word);
    return entry->source > 0 && entry->source <= SOURCES_MAX && ((g->sources >> (entry->source - 1)) & 1);
}

/* The rows the store opens with, for now: the three groups the standard
   names as the console's own, in this order, whatever else the catalogs
   write. A package in another category stands in the list below them and
   under no row. */
static const char *const STORE_CATEGORIES[] = { "game", "demo", "app" };

/* The tags the most packages carry: every tag of every package the store
   shows counted once, in a table of places kept by the tag's letters,
   case aside, then the most common taken out of it. Once a catalog, not
   once a frame. "unreleased" is the store's own switch, not a subject. */
#define TAG_PLACES 1024
static struct { const char *at; unsigned char len; unsigned short apps; } g_tag_place[TAG_PLACES];

static unsigned fold(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int same_len_word(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (fold((unsigned char)a[i]) != fold((unsigned char)b[i])) return 0;
    return 1;
}

static void collect_tags(void) {
    memset(g_tag_place, 0, sizeof(g_tag_place));
    for (int i = 0; i < g_view_of->count; i++) {
        const struct app_entry *e = &g_view_of->apps[i];
        if (view_hidden(e)) continue;
        for (const char *p = txt(e->tags); *p; p += *p == '\n') {
            size_t k = strcspn(p, "\n");
            if (k && k < PSPDX_CATEGORY_SIZE && !(k == 10 && same_len_word(p, "unreleased", 10))) {
                unsigned h = 2166136261u;
                for (size_t j = 0; j < k; j++) h = (h ^ fold((unsigned char)p[j])) * 16777619u;
                for (unsigned at = h % TAG_PLACES, tries = 0; tries < TAG_PLACES;
                     at = (at + 1) % TAG_PLACES, tries++) {
                    if (!g_tag_place[at].at) {
                        g_tag_place[at].at = p;
                        g_tag_place[at].len = (unsigned char)k;
                        g_tag_place[at].apps = 1;
                        break;
                    }
                    if (g_tag_place[at].len == k && same_len_word(g_tag_place[at].at, p, k)) {
                        g_tag_place[at].apps++;
                        break;
                    }
                }
            }
            p += k;
        }
    }
    for (int n = 0; n < VIEW_TAGS && g_groups < VIEW_GROUPS; n++) {
        int best = -1;
        for (int at = 0; at < TAG_PLACES; at++) {
            if (g_tag_place[at].apps < VIEW_TAG_MIN) continue;
            if (best < 0 || g_tag_place[at].apps > g_tag_place[best].apps) { best = at; continue; }
            /* A tie goes to the word first in the alphabet, so the order
               is the same whatever places the words fell into. */
            if (g_tag_place[at].apps == g_tag_place[best].apps) {
                char a[PSPDX_CATEGORY_SIZE], b[PSPDX_CATEGORY_SIZE];
                snprintf(a, sizeof(a), "%.*s", g_tag_place[at].len, g_tag_place[at].at);
                snprintf(b, sizeof(b), "%.*s", g_tag_place[best].len, g_tag_place[best].at);
                if (strcasecmp(a, b) < 0) best = at;
            }
        }
        if (best < 0) break;
        struct group *g = &g_group[g_groups++];
        snprintf(g->word, sizeof(g->word), "%.*s", g_tag_place[best].len, g_tag_place[best].at);
        g->kind = VIEW_GROUP_TAG;
        g->apps = g_tag_place[best].apps;
        g_tag_place[best].apps = 0;
    }
}

/* One row a name: sources the same name stands for -- every repository
   typed into Direct Install -- are one row. The most apps first; a source
   no package the store shows came from has no row. */
static void collect_sources(void) {
    int first = g_groups, count[SOURCES_MAX] = {0};
    for (int i = 0; i < g_view_of->count; i++) {
        const struct app_entry *e = &g_view_of->apps[i];
        if (e->source > 0 && e->source <= g_view_of->sources && e->source <= SOURCES_MAX &&
            !view_hidden(e))
            count[e->source - 1]++;
    }
    for (int s = 0; s < g_view_of->sources && s < SOURCES_MAX; s++) {
        if (!count[s]) continue;
        const char *name = g_view_of->source_name[s];
        int at = first;
        while (at < g_groups && strcmp(g_group[at].word, name)) at++;
        if (at == g_groups) {
            if (g_groups >= VIEW_GROUPS) break;
            g_groups++;
            memset(&g_group[at], 0, sizeof(g_group[at]));
            snprintf(g_group[at].word, sizeof(g_group[at].word), "%s", name);
            g_group[at].kind = VIEW_GROUP_SOURCE;
        }
        g_group[at].apps += count[s];
        g_group[at].sources |= (unsigned short)(1u << s);
    }
    /* Most first, a tie in the order of the file. */
    for (int i = first + 1; i < g_groups; i++) {
        struct group g = g_group[i];
        int j = i;
        while (j > first && g_group[j - 1].apps < g.apps) { g_group[j] = g_group[j - 1]; j--; }
        g_group[j] = g;
    }
}

static void list_way(void) {
    g_ways = 0;
    for (int n = 0; g_browse >= 0 && n < g_groups; n++)
        if (g_group[n].kind == g_browse) g_way[g_ways++] = (unsigned char)n;
    /* The cloud keeps the groups' order: the tags most apps carry first,
       so the ones anybody still uses are at the top, not a 2007 contest. */
}

static void collect_groups(void) {
    g_groups = 0;
    for (size_t c = 0; c < sizeof(STORE_CATEGORIES) / sizeof(*STORE_CATEGORIES); c++) {
        memset(&g_group[g_groups], 0, sizeof(g_group[0]));
        snprintf(g_group[g_groups].word, sizeof(g_group[0].word), "%s", STORE_CATEGORIES[c]);
        g_group[g_groups++].kind = VIEW_GROUP_CATEGORY;
    }
    if (g_view_of) {
        for (int i = 0; i < g_view_of->count; i++) {
            int c = category_of(&g_view_of->apps[i]);
            if (c >= 0 && !view_hidden(&g_view_of->apps[i])) g_group[c].apps++;
        }
        collect_tags();
        collect_sources();
    }
    if (g_group_open >= g_groups) g_group_open = -1;
    list_way();
}

static const char *const BROWSE[VIEW_BROWSE] = {
    T_BROWSE_CATEGORY, T_BROWSE_TAG, T_BROWSE_SOURCE,
};
const char *view_browse_word(int k) { return k >= 0 && k < VIEW_BROWSE ? BROWSE[k] : ""; }
int view_browse_rows(int k) {
    int n = 0;
    for (int g = 0; g < g_groups; g++) n += g_group[g].kind == k;
    return n;
}
int view_browse_at(void) { return g_browse; }
int view_cloud(void) {
    return (g_tabs ? g_tab[g_tab_at] : 0) == TAB_HOMEBREW && g_browse == VIEW_GROUP_TAG &&
           g_group_open < 0;
}

static unsigned published(int index);
int view_newest(int n, int browse, int *out, int max) {
    int got = 0;
    if (!g_view_of || max <= 0) return 0;
    for (int i = 0; i < g_view_of->count; i++) {
        const struct app_entry *e = &g_view_of->apps[i];
        if (view_hidden(e)) continue;
        int in = 0;
        if (n >= 0) in = view_in_group(e, n);
        else if (browse == VIEW_GROUP_CATEGORY) in = category_of(e) >= 0;
        else
            for (int g = 0; !in && g < g_groups; g++)
                in = g_group[g].kind == browse && view_in_group(e, g);
        if (!in) continue;
        /* Kept newest first, a tie in the catalog's order, as the store
           lists them. */
        unsigned rev = published(i);
        int at = got;
        while (at > 0 && published(out[at - 1]) < rev) at--;
        if (at >= max) continue;
        if (got < max) got++;
        for (int k = got - 1; k > at; k--) out[k] = out[k - 1];
        out[at] = i;
    }
    return got;
}

static int on_store(void) { return (g_tabs ? g_tab[g_tab_at] : 0) == TAB_HOMEBREW; }
/* The rows above the packages on the open tab: the three ways at the
   store's first rows, the way's rows while one is open and no row of it
   is, nothing inside a row. */
static int root_rows(void) {
    if (!on_store() || g_group_open >= 0) return 0;
    return g_browse < 0 ? VIEW_BROWSE : g_ways;
}
static int root_index(int row) {
    return g_browse < 0 ? VIEW_ROW_BROWSE - row : VIEW_ROW_GROUP - g_way[row];
}
int view_group_count(void) { return g_groups; }
const char *view_group_word(int n) { return n >= 0 && n < g_groups ? g_group[n].word : ""; }
enum view_group_kind view_group_kind(int n) {
    return n >= 0 && n < g_groups ? (enum view_group_kind)g_group[n].kind : VIEW_GROUP_CATEGORY;
}
int view_group_apps(int n) { return n >= 0 && n < g_groups ? g_group[n].apps : 0; }
int view_group_open_at(void) { return g_group_open; }

static void build_view(int restart);
void view_browse_open(int k) {
    if (k < 0 || k >= VIEW_BROWSE) return;
    g_browse = k;
    g_group_open = -1;
    list_way();
    build_view(1);
}
int view_browse_close(void) {
    int was = g_browse;
    if (was < 0) return -1;
    g_browse = -1;
    g_group_open = -1;
    list_way();
    build_view(1);
    return was;
}
void view_group_open(int n) {
    if (n < 0 || n >= g_groups) return;
    if (g_browse != g_group[n].kind) {
        g_browse = g_group[n].kind;
        list_way();
    }
    g_group_open = n;
    build_view(1);
}
int view_group_close(void) {
    int was = g_group_open;
    if (was < 0) return -1;
    g_group_open = -1;
    build_view(1);
    for (int row = 0; row < g_ways; row++)
        if (g_way[row] == was) return row;
    return 0;
}

static unsigned published(int index) {
    const struct app_entry *entry = &g_view_of->apps[index];
    return entry->has_release ? entry->release.rev : 0;
}
static int newest_first(const void *a, const void *b) {
    int x = *(const unsigned short *)a, y = *(const unsigned short *)b;
    unsigned px = published(x), py = published(y);
    if (px != py) return px > py ? -1 : 1;
    return x - y;
}

static void build_view(int restart) {
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    g_view_count = 0;
    g_view_action = 0;
    if (!g_view_of || g_tabs <= 0) return;
    for (int i = 0; i < g_view_of->count; i++) {
        int take;
        if (tab == TAB_HOMEBREW)
            take = !view_hidden(&g_view_of->apps[i]) &&
                   (g_group_open >= 0 ? view_in_group(&g_view_of->apps[i], g_group_open)
                                      : g_browse < 0);
        else if (tab == TAB_STICK)
            take = g_view_of->apps[i].state != APP_NOT_INSTALLED;
        else if (tab == TAB_BASKET) take = view_basket_has(i) || g_downloads[i] != 0;
        else take = 0;      /* the gear's rows are not packages; the UMD has none */
        if (take) g_view[g_view_count++] = (unsigned short)i;
    }
    /* Homebrew and the stick list the newest release first; what says no
       release date stands last, and a tie keeps the catalog's order. */
    if (tab == TAB_HOMEBREW || tab == TAB_STICK)
        qsort(g_view, (size_t)g_view_count, sizeof(*g_view), newest_first);
    /* On the stick, what has something waiting for it stands first, newest
       first; the rest after, likewise. */
    if (tab == TAB_STICK) {
        static unsigned short sorted[MAX_APPS];
        int n = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < g_view_count; i++) {
                int waiting = g_view_of->apps[g_view[i]].state == APP_UPDATE;
                if (waiting == !pass) sorted[n++] = g_view[i];
            }
        memcpy(g_view, sorted, (size_t)n * sizeof(*g_view));
    }
    /* The live transfer leads the cart, then queued jobs and its other apps. */
    if (tab == TAB_BASKET) {
        for (int i = 1; i < g_view_count; i++) {
            unsigned short value = g_view[i];
            int j = i;
            while (j > 0 && g_view[j - 1] != g_download_running &&
                   (value == g_download_running ||
                    (g_downloads[value] && (!g_downloads[g_view[j - 1]] ||
                     g_downloads[g_view[j - 1]] > g_downloads[value])))) {
                g_view[j] = g_view[j - 1]; j--;
            }
            g_view[j] = value;
        }
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
    if (g_basket_n > 0 || view_download_count()) g_tab[g_tabs++] = TAB_BASKET;
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
    memset(g_downloads, 0, sizeof(g_downloads));
    g_download_running = -1;
    g_view_of = catalog;
    g_group_open = -1;
    g_browse = -1;
    collect_groups();
    collect_tabs(was);
    build_view(1);
}

void view_show_unreleased(int on) {
    g_show_unreleased = on;
    /* Other tags may lead now: the one open is found again by its word,
       and the way's rows stand in its place if it has gone. */
    struct group was = g_group_open >= 0 ? g_group[g_group_open] : (struct group){0};
    g_group_open = -1;
    collect_groups();
    for (int n = 0; was.word[0] && n < g_groups; n++)
        if (g_group[n].kind == was.kind && !strcmp(g_group[n].word, was.word)) g_group_open = n;
    build_view(1);
}
int view_unreleased_shown(void) { return g_show_unreleased; }

int view_tabs_refresh(void) {
    int was = g_tabs ? g_tab[g_tab_at] : 0;
    int kept = collect_tabs(was);
    build_view(!kept);
    return kept;
}

int view_count(void) {
    if (g_tabs && g_tab[g_tab_at] == TAB_GEAR) return VIEW_SETTINGS;
    return root_rows() + g_view_count + g_view_action;
}

static int view_action(int row) { return g_view_action && row == 0; }

int view_index(int row) {
    if (g_tabs && g_tab[g_tab_at] == TAB_GEAR)
        return row >= 0 && row < VIEW_SETTINGS ? VIEW_ROW_SETTING - row : -1;
    int roots = root_rows();
    if (row >= 0 && row < roots) return root_index(row);
    row -= roots;
    if (view_action(row)) return VIEW_ROW_ACTION;
    row -= g_view_action;
    return row >= 0 && row < g_view_count ? g_view[row] : -1;
}

int view_row(int index) {
    for (int row = 0; row < g_view_count; row++)
        if (g_view[row] == index) return row + g_view_action + root_rows();
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
        if (g_downloads[g_view[row]]) continue;
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
    g_group_open = -1;
    g_browse = -1;
    list_way();
    build_view(1);
}


/* The tab that is open, and the ones on screen in their order: what the
   header names and the row of signs draws. */
int view_tab_current(void) { return g_tabs ? g_tab[g_tab_at] : TAB_HOMEBREW; }
int view_tab_at(int i) { return i >= 0 && i < g_tabs ? g_tab[i] : TAB_HOMEBREW; }
int view_tab_active(void) { return g_tab_at; }

int view_basket_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_APPS; i++) n += view_basket_has(i) || g_downloads[i] != 0;
    return n;
}

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

/* Tabs have stable IDs even when Installed or Basket temporarily disappears. */
static struct {
    int row, details;
    float scroll;
    char app[PSPDX_ID_SIZE], group[PSPDX_CATEGORY_SIZE];
    int group_kind, browse;
} bookmarks[TAB_COUNT];

void view_remember(int cursor, int details_index, float detail_scroll) {
    int tab = -view_tab_current();
    if (tab < 0 || tab >= TAB_COUNT || !g_view_of) return;
    int index = details_index >= 0 ? details_index : view_index(cursor);
    bookmarks[tab].row = cursor;
    bookmarks[tab].details = details_index >= 0;
    bookmarks[tab].scroll = detail_scroll;
    snprintf(bookmarks[tab].app, sizeof(bookmarks[tab].app), "%s",
             index >= 0 && index < g_view_of->count ? g_view_of->apps[index].id : "");
    int open = view_tab_current() == TAB_HOMEBREW ? g_group_open : -1;
    snprintf(bookmarks[tab].group, sizeof(bookmarks[tab].group), "%s", view_group_word(open));
    bookmarks[tab].group_kind = view_group_kind(open);
    bookmarks[tab].browse = view_tab_current() == TAB_HOMEBREW ? g_browse + 1 : 0;   /* 0: none */
}

void view_recall(int *cursor, int *details_index, float *detail_scroll) {
    int tab = -view_tab_current();
    *cursor = 0;
    *details_index = -1;
    *detail_scroll = 0;
    if (tab < 0 || tab >= TAB_COUNT || !g_view_of) return;
    if (view_tab_current() == TAB_HOMEBREW && bookmarks[tab].browse > 0)
        view_browse_open(bookmarks[tab].browse - 1);
    if (view_tab_current() == TAB_HOMEBREW && bookmarks[tab].group[0]) {
        for (int i = 0; i < g_groups; i++)
            if (g_group[i].kind == bookmarks[tab].group_kind &&
                !strcmp(g_group[i].word, bookmarks[tab].group)) {
                view_group_open(i);
                break;
            }
    }
    int count = view_count();
    *cursor = bookmarks[tab].row < count ? bookmarks[tab].row : count - 1;
    if (*cursor < 0) *cursor = 0;
    if (!bookmarks[tab].app[0]) return;
    for (int i = 0; i < g_view_of->count; i++) {
        if (strcmp(g_view_of->apps[i].id, bookmarks[tab].app)) continue;
        int row = view_row(i);
        if (row < 0) return;
        *cursor = row;
        if (bookmarks[tab].details) {
            *details_index = i;
            *detail_scroll = bookmarks[tab].scroll;
        }
        return;
    }
}
