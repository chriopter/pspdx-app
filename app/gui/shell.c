#include <time.h>
#include "util/storage.h"
/*
 * The browser: a list of names on the left, the selected package on the
 * right, both standing on the lattice. Layout lives here; everything that
 * moves on its own lives in lattice.c; the room takes a new colour with
 * every selection, drawn by lot.
 *
 * Nothing here holds state the catalog already has. What lives across frames
 * is motion -- the eased selection, the theme mid-crossfade -- and the one
 * screenshot texture.
 */

#include <pspkernel.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "logic/entropy.h"
#include "network/https.h"
#include "gui/shell.h"
#include "gui/font.h"
#include "gui/gfx.h"
#include "gui/icons.h"
#include "gui/lattice.h"
#include "gui/marks.h"
#include "gui/title.h"
#include "gui/palette.h"
#include "gui/preview.h"
#include "util/runtime.h"

/* Three type roles and nowhere else a fourth: FONT_H1 for the one name on
   screen, FONT_BODY for the list and the summary, FONT_META for facts. */

#define HEADER_H 32
#define FOOTER_Y 252
#define FOOTER_BASE (FOOTER_Y + 11)     /* the hints' baseline, clear of the edge */

#define LIST_X 16
#define LIST_W 200
#define LIST_Y 44
#define ITEM_H 32
/* The bundle's icon, 144x80 shown at a third: as tall as the row allows
   with a little air, and the name starts after it. */
#define ICON_W 43
#define ICON_H 24
#define NAME_X (LIST_X + ICON_W + 9)
#define VISIBLE ((FOOTER_Y - 6 - LIST_Y) / ITEM_H)

#define PANEL_X 232
#define SHOT_W 224
#define SHOT_H (SHOT_W * SCR_H / SCR_W)     /* the screen's own 480:272 */
#define SHOT_Y 43
#define REFLECT_H 18
#define FILM_W 144                  /* an ICON1.PMF, as the firmware plays it */
#define FILM_H 80

/* ------------------------------------------------------------------ colour */

static const struct rgb NIGHT_TOP = { 2, 3, 9 };
static const struct rgb NIGHT_BOTTOM = { 6, 8, 22 };

static const struct rgb DEFAULT_TINT = { 80, 140, 255 };

/* Every selection lights the room a colour drawn by lot: a hue at full
   saturation, always at least a third of the wheel from the last one, so
   the change is a change. The lot is a plain generator seeded by the clock
   and has nothing to do with the entropy pool. */
static unsigned g_lot;
static float g_hue = 0.58f;

static struct rgb hue_rgb(float h) {
    h -= (float)(int)h;
    float x = h * 6.0f;
    int sector = (int)x;
    float f = x - sector;
    int up = (int)(f * 255.0f), down = 255 - up;
    switch (sector) {
    case 0: return (struct rgb){ 255, up, 0 };
    case 1: return (struct rgb){ down, 255, 0 };
    case 2: return (struct rgb){ 0, 255, up };
    case 3: return (struct rgb){ 0, down, 255 };
    case 4: return (struct rgb){ up, 0, 255 };
    default: return (struct rgb){ 255, 0, down };
    }
}

static struct rgb draw_lot(void) {
    if (!g_lot) g_lot = now_us() | 1;
    g_lot = g_lot * 1664525u + 1013904223u;
    /* A third to two thirds of the wheel away, either direction. */
    float step = 0.33f + 0.34f * ((g_lot >> 8) & 0xFFFF) / 65536.0f;
    g_hue += step;
    g_hue -= (float)(int)g_hue;
    /* Not the yellows: water lit yellow is mud. The band from orange-yellow
       to yellow-green is stepped over. */
    if (g_hue > 0.10f && g_hue < 0.22f) g_hue += 0.12f;
    /* Softened a little: pure spectral colours read as a warning light. */
    return rgb_mix(hue_rgb(g_hue), RGB_WHITE, 0.18f);
}

/* The palette of the current frame, derived from the eased tint once per
   frame so every element agrees. */
static struct rgb g_tint = { 80, 140, 255 };
static unsigned g_accent, g_text, g_dim;

static void derive_palette(void) {
    g_accent = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.45f), 255);
    g_text = rgb_pack(rgb_mix(RGB_WHITE, g_tint, 0.05f), 255);
    g_dim = rgb_pack(rgb_mix(rgb_mix(NIGHT_BOTTOM, RGB_WHITE, 0.58f), g_tint, 0.12f), 255);
}

/* ------------------------------------------------------------------- state */

static int g_last_cursor = -1;
static float g_sel_y = LIST_Y;
static int g_first;
static int g_fade = 255;                /* black over everything at start */

/* Install overlay, live only between shell_install_begin and _end. */
static int g_installing;
static char g_install_name[56];         /* a name, and room for "2 of 3: " */
static char g_install_phase[16];
static size_t g_install_done, g_install_total;
static float g_bar;                     /* the drawn end, chasing the reported one */
static unsigned g_install_drawn_ms;
static char g_status[96];
/* What stands over the browser, if anything: a question, a short menu with a
   cursor on one of its choices, or the info band. All of it is set from
   outside and only drawn here -- what is pressed in answer, and which row the
   cursor is on, is the main loop's business. */
static char g_ask_title[64], g_ask_line[96];
#define MENU_MAX 7
static char g_menu_title[48], g_menu_item[MENU_MAX][32];
static unsigned char g_menu_on[MENU_MAX];
static signed char g_menu_key[MENU_MAX];
static int g_menu_count, g_menu_cursor;
static float g_menu_slide;              /* 0 off the right edge, 1 in place */
static int g_menu_leaving;              /* sliding out; count drops at 0 */
static int g_info;
static const struct app_entry *g_details;   /* the package the band is about */
static int g_resting;                   /* left alone: where the picture is going */
static float g_rest;                    /* 0 no picture, 1 the picture whole */
/* The share of a frame that goes into drawing it, eased over about a second
   so the number on screen does not flicker. */
static float g_load;
static float g_frame_us;                /* frame to frame, eased, for the fps */
static char g_word[24] = "Connecting";
static const struct catalog *g_catalog;
static int g_cursor;

/* --------------------------------------------------------------- the view */

/* The tabs, in the order they are shown. The first takes everything; the
   rest match the catalog's own category word, which schema/v1.pspdx spells
   in the singular: a tab holds many, an app is one. */
static const char *const TAB_NAME[] = {
    "All", "Games", "Demos", "Apps", "Emulators", "Plugins"
};
static const char *const TAB_KEY[] = {
    "", "game", "demo", "app", "emulator", "plugin"
};
#define TAB_ALL 6

/* Two tabs are not categories and are named by a sign rather than a word:
   the stick -- what is installed, with whatever newer is waiting for it at
   the top, and the sign turning into the update arrows while anything is --
   and the basket this session has filled. They are numbered below zero so
   that a tab is either an index into TAB_NAME or one of these, with nothing
   to keep in step, and they stand to the left of All because what is one's
   own comes before what is merely there to browse. */
#define TAB_STICK   (-2)
/* And, leftmost, where the system's own shell keeps its settings: the band
   about this session -- what it is connected to, what it is standing on --
   and the two things that can be done about either. Reached the way a tab
   is, so that it needs no key of its own. */
#define TAB_GEAR    (-3)
#define TAB_BASKET  (-1)

static int g_tab[TAB_ALL + 3];          /* which of them have anything */
static int g_tabs;
static int g_tab_at;                    /* index into g_tab, not into TAB_NAME */
static const struct catalog *g_view_of;
static unsigned char g_view[MAX_APPS];
static int g_view_count;                /* packages; the action row is extra */
static int g_view_action;               /* 1 when row 0 is the action row */

/* The basket: catalog indices set aside this session, a bit each. */
static unsigned char g_basket[(MAX_APPS + 7) / 8];
static int g_basket_n;

int shell_basket_has(int index) {
    if (index < 0 || index >= MAX_APPS) return 0;
    return (g_basket[index >> 3] >> (index & 7)) & 1;
}

int shell_basket_count(void) { return g_basket_n; }

void shell_basket_toggle(int index) {
    if (index < 0 || index >= MAX_APPS) return;
    g_basket[index >> 3] ^= (unsigned char)(1u << (index & 7));
    g_basket_n += shell_basket_has(index) ? 1 : -1;
}

void shell_basket_forget(int index) {
    if (shell_basket_has(index)) shell_basket_toggle(index);
}

void shell_basket_clear(void) {
    memset(g_basket, 0, sizeof(g_basket));
    g_basket_n = 0;
}

/* How many packages on the stick have a newer one published. The number is
   the updates tab's own label and the reason it exists at all, so it is asked
   for rather than remembered. */
static int updates_waiting(void) {
    int n = 0;
    if (!g_view_of) return 0;
    for (int i = 0; i < g_view_of->count; i++)
        if (g_view_of->apps[i].state == APP_UPDATE) n++;
    return n;
}

/* restart is for a view whose rows now stand for other packages than they
   did: the list goes back to the top and the card is told to fetch afresh.
   A view merely rebuilt under the same tab keeps where it was scrolled to. */
static void build_view(int restart) {
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    g_view_count = 0;
    g_view_action = 0;
    if (!g_view_of || g_tabs <= 0) return;
    for (int i = 0; i < g_view_of->count; i++) {
        int take;
        if (tab == TAB_STICK) take = g_view_of->apps[i].state != APP_NOT_INSTALLED;
        else if (tab == TAB_GEAR) take = 0;     /* its rows are not packages */
        else if (tab == TAB_BASKET) take = shell_basket_has(i);
        else take = !TAB_KEY[tab][0] ||
                    strcmp(g_view_of->apps[i].category, TAB_KEY[tab]) == 0;
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
    /* A tab that is a job rather than a category carries the job itself at
       the top, above the packages it would be done to -- the stick only
       while there is a job on it. */
    g_view_action = tab == TAB_BASKET || tab == TAB_STICK;
    if (!restart) return;
    g_first = 0;
    g_last_cursor = -1;
}

/* Which tabs have anything in them, in the order they are shown, and where
   the one named by keep ended up. Returns 0 if keep did not survive. */
static int collect_tabs(int keep) {
    int found = 0;
    g_tabs = 0;
    g_tab_at = 0;
    if (!g_view_of || g_view_of->count <= 0) return 0;
    int installed = 0;
    for (int i = 0; i < g_view_of->count; i++)
        if (g_view_of->apps[i].state != APP_NOT_INSTALLED) installed = 1;
    g_tab[g_tabs++] = TAB_GEAR;
    if (installed) g_tab[g_tabs++] = TAB_STICK;
    if (g_basket_n > 0) g_tab[g_tabs++] = TAB_BASKET;
    for (int t = 0; t < TAB_ALL; t++) {
        int has = !TAB_KEY[t][0];
        for (int i = 0; !has && i < g_view_of->count; i++)
            has = strcmp(g_view_of->apps[i].category, TAB_KEY[t]) == 0;
        if (has) g_tab[g_tabs++] = t;
    }
    for (int i = 0; i < g_tabs; i++)
        if (g_tab[i] == keep) { g_tab_at = i; found = 1; }
    /* A tab that has gone -- the last thing taken out of the basket -- is
       answered with All, not with whatever stands leftmost, which is the
       band about the session and not a list at all. */
    if (!found)
        for (int i = 0; i < g_tabs; i++)
            if (g_tab[i] == 0) g_tab_at = i;
    return found;
}

void shell_view_rebuild(const struct catalog *catalog) {
    int was = g_tabs ? g_tab[g_tab_at] : 0;
    /* A fetch rewrites the array the basket's indices point into, and row
       seventeen of the new catalog is not the package row seventeen of the
       old one was. Nothing is carried across. */
    shell_basket_clear();
    g_view_of = catalog;
    collect_tabs(was);
    build_view(1);
}

int shell_tabs_refresh(void) {
    int was = g_tabs ? g_tab[g_tab_at] : 0;
    int kept = collect_tabs(was);
    build_view(!kept);
    return kept;
}

int shell_view_count(void) {
    if (g_tabs && g_tab[g_tab_at] == TAB_GEAR) return SHELL_SETTINGS;
    return g_view_count + g_view_action;
}

int shell_view_action(int row) { return g_view_action && row == 0; }

int shell_view_index(int row) {
    if (g_tabs && g_tab[g_tab_at] == TAB_GEAR)
        return row >= 0 && row < SHELL_SETTINGS ? SHELL_ROW_SETTING - row : -1;
    if (shell_view_action(row)) return SHELL_ROW_ACTION;
    row -= g_view_action;
    return row >= 0 && row < g_view_count ? g_view[row] : -1;
}

int shell_view_row(int index) {
    for (int row = 0; row < g_view_count; row++)
        if (g_view[row] == index) return row + g_view_action;
    return -1;
}

enum shell_tab_kind shell_tab_kind(void) {
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    return tab == TAB_GEAR ? SHELL_TAB_GEAR
         : tab == TAB_STICK ? SHELL_TAB_STICK
         : tab == TAB_BASKET ? SHELL_TAB_BASKET : SHELL_TAB_CATEGORY;
}

void shell_action_plan(struct shell_plan *plan) {
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
        if (!entry->has_release || !entry->release.size) { plan->skipped++; continue; }
        plan->apps++;
        plan->bytes += entry->release.size;
        if (entry->state == APP_CURRENT) plan->again++;
    }
}

int shell_tab_count(void) { return g_tabs; }

void shell_tab_move(int step) {
    if (g_tabs <= 1) return;
    g_tab_at = (g_tab_at + step + g_tabs) % g_tabs;
    build_view(1);
}

int shell_init(void) {
    if (!font_init()) return 0;
    gfx_init();
    lattice_init();
    preview_init();
    return 1;
}

void shell_shutdown(void) {
    preview_shutdown();
    icons_reset();
    font_shutdown();
    gfx_shutdown();
}

/* ------------------------------------------------------------------ marks */

/* The signs that are not letters live in gui/marks.c, drawn from the PNG
   set under assets/marks. What the shell decides here is only which sign,
   where, in what colour, and how lit. */

/* What the update sign is lit by: no spin -- a shape this small turning is a
   shape flickering -- but a slow swell of brightness, so a tab or a row with
   an update on it is alive without ever moving. */
static float update_pulse(float t) { return 0.85f + 0.15f * sinf(t * 1.6f); }

/* The same colour, quieter: a mark on a row the eye is not on should be
   read only when it is looked for. */
static unsigned faded(unsigned color, int alpha) {
    return (color & 0x00FFFFFFu) | ((unsigned)alpha << 24);
}

/* The green-white an update is said in, wherever it is said. */
#define UPDATE_RGB RGB(170, 255, 190)

/* ---------------------------------------------------------------- sizes */

/* Megabytes to a tenth: whole megabytes call everything under one of them
   nothing, and a count of bytes is not a size anybody reads. */
static void size_mb(unsigned long long bytes, char *out, size_t size) {
    snprintf(out, size, "%lu.%lu MB", (unsigned long)(bytes >> 20),
             (unsigned long)((bytes * 10 >> 20) % 10));
}

/* What the wait will be. A PSP-1004's 802.11b radio and TCP stack were
   measured at 180 KB/s, which is the only honest number to quote here --
   the emulator's host link would promise a minute the hardware cannot keep. */
#define PSP_KB_PER_S 180

static void download_time(unsigned long long bytes, char *out, size_t size) {
    unsigned secs = (unsigned)(bytes / (PSP_KB_PER_S * 1024));
    if (secs < 90) snprintf(out, size, "%u s", secs);
    else snprintf(out, size, "%u min", (secs + 30) / 60);
}

/* ------------------------------------------------------------------ chrome */

/* Under a block of text the floor is dimmed, but with a soft spot and not
   a plate: the lattice runs everywhere, it only gets quieter here. */
static void draw_shade(int cx, int cy, int w, int h) {
    gfx_shade(cx, cy, w * 1.6f, h * 1.8f, 170);
}

/* A tab that is a job rather than a category is a sign and a number -- the
   turning arrows and how many wait, the basket and what is in it -- because
   the sign is the same one the rows below it carry and a word would not be.
   The number is built into a static, so it is read before the next call. */
static const char *tab_count(int tab) {
    static char text[8];
    snprintf(text, sizeof(text), "%d",
             tab == TAB_STICK ? updates_waiting() : g_basket_n);
    return text;
}

/* The stick's sign is the stick until something is waiting for it, and the
   update arrows with the count while something is. */
static enum mark tab_mark(int tab) {
    if (tab == TAB_GEAR) return MARK_GEAR;
    if (tab == TAB_BASKET) return MARK_BASKET;
    return updates_waiting() > 0 ? MARK_UPDATE : MARK_STICK;
}

static float tab_width(int tab) {
    if (tab >= 0) return mark_width(MARK_ALL + tab);
    if (tab == TAB_GEAR) return mark_width(MARK_GEAR);
    if (tab == TAB_STICK && updates_waiting() == 0) return mark_width(MARK_STICK);
    return mark_width(tab_mark(tab)) + 5 + font_width(FONT_META, tab_count(tab));
}

/* The tabs stand where they stand. The named ones start at a fixed
   column and step right by a fixed gap, so All is always in the same
   place whatever the count on the right says and however many tabs there
   are; the two that come and go -- updates and the basket -- hang to the
   left of that column, growing leftward, so their appearing never moves a
   word the eye has learned the place of. The active one is lit rather
   than boxed: a word in the text colour with the room's own light welling
   up under it. */
#define TAB_X 232.0f
#define TAB_GAP 16.0f

static void draw_tab(int tab, int on, float x, float t) {
    float w = tab_width(tab);
    if (on) {
        gfx_glow(x + w / 2, 17, w + 30, 30, rgb_pack(g_tint, 110));
        gfx_glow(x + w / 2, 24, w + 8, 7,
                 rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.6f), 120));
    }
    if (tab >= 0) {
        /* A sign, not a word: the word is said once, at the head of the
           list, for the tab that is open. */
        mark_draw(MARK_ALL + tab, x + w / 2.0f, 16, on ? g_text : faded(g_dim, 150),
                  on ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
    } else {
        enum mark m = tab_mark(tab);
        unsigned c = m == MARK_UPDATE
            ? faded(UPDATE_RGB, (int)((on ? 255 : 170) * update_pulse(t)))
            : (on ? g_text : faded(g_dim, 150));
        mark_draw(m, x + mark_width(m) / 2.0f, 16, c, on ? MARK_LIT : MARK_PLAIN,
                  m == MARK_UPDATE ? UPDATE_RGB : rgb_pack(g_tint, 255), t);
        if (m != MARK_STICK && m != MARK_GEAR)
            font_print(FONT_META, x + mark_width(m) + 5, 21, on ? g_text : g_dim,
                       tab_count(tab));
    }
}

static void draw_tabs(float left, float right, float t) {
    (void)left; (void)right;
    if (g_tabs <= 1) return;
    float x = TAB_X;
    for (int i = 0; i < g_tabs; i++) {
        if (g_tab[i] < 0) continue;
        draw_tab(g_tab[i], i == g_tab_at, x, t);
        x += tab_width(g_tab[i]) + TAB_GAP;
    }
    x = TAB_X - TAB_GAP;
    for (int i = g_tabs - 1; i >= 0; i--) {
        if (g_tab[i] >= 0) continue;
        x -= tab_width(g_tab[i]);
        draw_tab(g_tab[i], i == g_tab_at, x, t);
        x -= TAB_GAP;
    }
    /* A hair between the console's own tabs and the catalog's: the gear
       and the stick are about this machine, the rest about what is
       published, and the eye should not read them as one row of the same
       kind. Lit the way the header's rule is, brightest in its middle. */
    unsigned bright = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.5f), 120);
    unsigned clear = rgb_pack(g_tint, 0);
    int sx = (int)(TAB_X - TAB_GAP / 2.0f);
    gfx_vgrad(sx, 7, 1, 9, clear, bright);
    gfx_vgrad(sx, 16, 1, 9, bright, clear);
}

static void draw_chrome(const struct catalog *catalog, float t) {
    gfx_vgrad(0, 0, SCR_W, HEADER_H, RGBA(255, 255, 255, 14), RGBA(255, 255, 255, 0));
    unsigned bright = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.5f), 150);
    unsigned clear = rgb_pack(g_tint, 0);
    gfx_hgrad(0, HEADER_H, SCR_W / 2, 1, clear, bright);
    gfx_hgrad(SCR_W / 2, HEADER_H, SCR_W / 2, 1, bright, clear);

    /* The one word in the header is the name of what the list holds: the
       open tab, said in words here and lit as a sign among the others on
       the right. Nothing is counted; the list is there to be looked at. */
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    const char *title = tab == TAB_GEAR ? "PSPDX"
                      : tab == TAB_STICK ? "Installed"
                      : tab == TAB_BASKET ? "Basket" : TAB_NAME[tab];
    gfx_glow(LIST_X + 24, 18, 110, 56, rgb_pack(g_tint, 80));
    font_print(FONT_H1, LIST_X, 23, g_text, title);
    if (catalog->count > 0) draw_tabs(0, 0, t);
}

/* ------------------------------------------------------------------- list */


/* The two words the action row is headed with, and the sentence under them.
   Both are wanted in the list and again in the panel, so they are made in one
   place. */
static const char *action_title(void) {
    if (shell_tab_kind() != SHELL_TAB_STICK) return "Download all";
    return updates_waiting() > 0 ? "Update all" : "Check for updates";
}

/* "3 apps, 61.5 MB" -- or, when nothing in the tab has a release with a
   size, what is in the way instead. */
static const char *action_line(void) {
    static char line[48];
    struct shell_plan plan;
    char size[24];
    shell_action_plan(&plan);
    if (plan.apps <= 0 && shell_tab_kind() == SHELL_TAB_STICK)
        snprintf(line, sizeof(line), "%d installed, all current", g_view_count);
    else if (plan.apps <= 0)
        snprintf(line, sizeof(line), "nothing here has a release");
    else {
        size_mb(plan.bytes, size, sizeof(size));
        snprintf(line, sizeof(line), "%d app%s, %s", plan.apps,
                 plan.apps == 1 ? "" : "s", size);
    }
    return line;
}

/* The row the tab itself sits on. Two lines rather than one: at this width
   the heading and the tally do not fit on a line together in the list's own
   face, and stacked they read as a heading with its tally under it, which is
   what they are. Where a package would have its icon, the tab's own sign. */
static void draw_action_row(int y, int selected, float t) {
    int updates = shell_tab_kind() == SHELL_TAB_STICK;
    float gx = LIST_X + ICON_W / 2.0f, gy = y + ITEM_H / 2.0f;
    if (updates)
        mark_draw(MARK_UPDATE, gx, gy,
                  faded(UPDATE_RGB, (int)((selected ? 255 : 170) * update_pulse(t))),
                  selected ? MARK_LIT : MARK_PLAIN, UPDATE_RGB, t);
    else
        mark_draw(MARK_BASKET, gx, gy, selected ? g_text : g_dim,
                  selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
    int w = LIST_X + LIST_W - NAME_X;
    font_print_clipped(FONT_BODY, NAME_X, y + 13, w, selected ? g_text : g_dim,
                       action_title());
    font_print_clipped(FONT_META, NAME_X, y + 25, w, g_dim, action_line());
}

/* A row under the gear: a word and, where the word is about something that
   is fetched, the sign that names it. The sign sits where a package's icon
   sits, so the column reads as one column whichever tab it is. */
static void draw_setting_row(int n, int y, int selected, float t) {
    static const signed char SIGN[SHELL_SETTINGS] = {
        MARK_DOWNLOAD, MARK_BASKET, MARK_STICK, MARK_INFO,
    };
    float gx = LIST_X + ICON_W / 2.0f, gy = y + ITEM_H / 2.0f;
    enum mark m = (enum mark)SIGN[n];
    mark_draw(m, gx, gy, selected ? g_text : faded(g_dim, 170),
              selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
    font_print_clipped(FONT_BODY, NAME_X, y + 21, LIST_X + LIST_W - NAME_X,
                       selected ? g_text : g_dim, shell_setting(n));
}

static void draw_list(const struct catalog *catalog, int cursor, float t) {
    int count = shell_view_count();
    if (cursor < g_first) g_first = cursor;
    if (cursor >= g_first + VISIBLE) g_first = cursor - VISIBLE + 1;
    if (g_first < 0) g_first = 0;

    /* The bar chases the selection rather than jumping to it. A quarter of
       the remaining distance per frame settles in about a fifth of a second
       and never overshoots. */
    float target = LIST_Y + (cursor - g_first) * ITEM_H;
    g_sel_y += (target - g_sel_y) * 0.25f;

    int rows = count < VISIBLE ? count : VISIBLE;
    draw_shade(LIST_X + LIST_W / 2, LIST_Y + rows * ITEM_H / 2, LIST_W, rows * ITEM_H);

    /* The selected row glows: a breathing light behind it and a thin
       streak of light under it, nothing with a corner. */
    float breathe = 0.85f + 0.15f * sinf(t * 2.2f);
    float mid = g_sel_y + ITEM_H / 2 - 1;
    gfx_glow(LIST_X + 60, mid, LIST_W + 170, ITEM_H * 3.4f,
             rgb_pack(g_tint, (int)(130 * breathe)));
    gfx_glow(LIST_X + 40, mid, LIST_W + 40, ITEM_H * 1.2f,
             rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.5f), (int)(70 * breathe)));
    gfx_glow(LIST_X + LIST_W / 2, g_sel_y + ITEM_H - 3, LIST_W + 30, 10,
             rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.7f), 160));

    /* The icons module counts in catalog entries, so the rows on screen are
       handed over as the entries they stand for -- and the action row stands
       for none, so it asks for nothing. */
    int wanted[VISIBLE], want_count = 0;
    for (int i = g_first; i < count && i < g_first + VISIBLE; i++) {
        int index = shell_view_index(i);
        if (index >= 0) wanted[want_count++] = index;
    }
    icons_bind(catalog);
    if (icons_want(wanted, want_count)) preview_poke();

    for (int i = g_first; i < count && i < g_first + VISIBLE; i++) {
        int y = LIST_Y + (i - g_first) * ITEM_H;
        int selected = i == cursor;
        int index = shell_view_index(i);
        if (index == SHELL_ROW_ACTION) { draw_action_row(y, selected, t); continue; }
        if (index <= SHELL_ROW_SETTING) {
            draw_setting_row(SHELL_ROW_SETTING - index, y, selected, t);
            continue;
        }
        if (index < 0) continue;
        const struct app_entry *entry = &catalog->apps[index];

        /* The bundle's own icon, dimmed with the name; a dark plate where
           it has not arrived, so the column reads as a column. */
        const struct gfx_texture *icon = icons_get(index);
        int iy = y + (ITEM_H - ICON_H) / 2;
        if (icon)
            gfx_texture_draw(icon, LIST_X, iy, ICON_W, ICON_H,
                             selected ? RGB(255, 255, 255) : RGB(150, 150, 150));
        else
            gfx_rect(LIST_X, iy, ICON_W, ICON_H, RGBA(255, 255, 255, selected ? 24 : 12));

        /* Marks, not words, at the end of the row, read from the outside in:
           where the package stands -- the line ticked off when it is on the
           stick, the system's turning arrows when a newer one waits, nothing
           for the rest -- and then, inside that, the basket if this session
           has set the package aside. */
        float mx = LIST_X + LIST_W - 8, my = y + ITEM_H / 2 - 1;
        int name_w = LIST_X + LIST_W - NAME_X;
        /* Two marks, read from the outside in: the update arrows where a
           newer package waits, the tick where the package is on the stick.
           The basket says nothing here; it has its own tab. */
        if (entry->state == APP_UPDATE) {
            mark_draw(MARK_UPDATE, mx, my,
                      faded(UPDATE_RGB, (int)((selected ? 255 : 170) * update_pulse(t))),
                      selected ? MARK_LIT : MARK_PLAIN, UPDATE_RGB, t);
            mx -= 21;
            name_w -= 21;
        } else if (entry->state != APP_NOT_INSTALLED) {
            /* On the stick: a quiet tick, the way a list ticks off what is
               done. The catalog is mostly what is not, so that is what
               carries no mark. */
            mark_draw(MARK_TICK, mx, my, selected ? g_accent : faded(g_dim, 150),
                      selected ? MARK_PLAIN : MARK_DIM, 0, t);
            mx -= 19;
            name_w -= 19;
        }
        /* Set aside: the basket, inside whatever the row carries about the
           stick, so a package that is both waiting and set aside shows
           both. */
        if (shell_basket_has(index)) {
            mark_draw(MARK_BASKET, mx, my, selected ? g_text : faded(g_dim, 170),
                      selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
            name_w -= 22;
        }

        font_print_clipped(FONT_BODY, NAME_X, y + 21, name_w,
                           selected ? g_text : g_dim, entry->name);
    }

    if (count > VISIBLE) {
        int track = FOOTER_Y - 6 - LIST_Y;
        int knob = track * VISIBLE / count;
        int at = track * g_first / count;
        gfx_rect(LIST_X + LIST_W + 10, LIST_Y, 2, track, RGBA(255, 255, 255, 24));
        gfx_rect(LIST_X + LIST_W + 10, LIST_Y + at, 2, knob, g_accent);
    }
}

/* ------------------------------------------------------------------ panel */

/* The card's half of the screen when the cursor is on the action row. There
   is no picture: a row that stands for several packages has no one screenshot
   to show, and a card left holding the last package's would be a lie about
   what X is going to fetch. What the space is worth instead is the bill --
   every package that would come down, what each weighs, the total, and how
   long that is over a PSP's own radio. */
static float draw_hint(float x, float base, enum mark m, const char *text, unsigned color);
static int draw_wrapped(enum font_style style, float x, float y, float width,
                        float step, int lines, unsigned color, const char *text);

static void draw_action_panel(const struct catalog *catalog, float t) {
    struct shell_plan plan;
    char value[48], size[24];
    int y = SHOT_Y + 14;

    shell_action_plan(&plan);
    draw_shade(PANEL_X + SHOT_W / 2, y + 70, SHOT_W, 170);
    gfx_glow(PANEL_X + SHOT_W / 2, y + 60, SHOT_W + 90, 200,
             rgb_pack(g_tint, (int)(70 * update_pulse(t))));

    font_print(FONT_H1, PANEL_X, y, g_text, action_title());
    if (plan.apps <= 0 && shell_tab_kind() == SHELL_TAB_STICK) {
        int lines = draw_wrapped(FONT_META, PANEL_X, y + 20, SHOT_W, 16, 2, g_dim,
                                 "Compares what is installed with what is published.");
        /* The row has two keys, and the panel names them the way the footer
           does, with the key's own mark rather than a word for it. */
        float base = y + 20 + 16 * lines + 14;
        draw_hint(PANEL_X, base, MARK_CROSS, "From the catalogs", g_dim);
        draw_hint(PANEL_X, base + 18, MARK_SQUARE, "From every app's own .pspdx, slower", g_dim);
        return;
    }
    if (plan.apps > 0) {
        size_mb(plan.bytes, size, sizeof(size));
        download_time(plan.bytes, value, sizeof(value));
        font_printf(FONT_META, PANEL_X, y + 20, g_dim, "%s to download, about %s",
                    size, value);
    } else {
        font_print(FONT_META, PANEL_X, y + 20, g_dim,
                   "nothing here has a release with a size");
    }
    /* One line a package, in the order they would be fetched, for as many as
       the panel holds; the rest are counted rather than named. */
    int line = 0, room = (FOOTER_Y - 20 - (y + 40)) / 14;
    for (int row = 0; row < shell_view_count(); row++) {
        int index = shell_view_index(row);
        if (index < 0) continue;
        const struct app_entry *entry = &catalog->apps[index];
        if (!entry->has_release || !entry->release.size) continue;
        if (line >= room) {
            font_printf(FONT_META, PANEL_X, y + 40 + line * 14, g_dim,
                        "and %d more", plan.apps - line);
            line++;
            break;
        }
        size_mb(entry->release.size, size, sizeof(size));
        float sw = font_width(FONT_META, size);
        font_print_clipped(FONT_META, PANEL_X, y + 40 + line * 14,
                           SHOT_W - sw - 10,
                           entry->state == APP_UPDATE ? UPDATE_RGB : g_text,
                           entry->name);
        font_print(FONT_META, PANEL_X + SHOT_W - sw, y + 40 + line * 14,
                   g_dim, size);
        line++;
    }
    if (plan.skipped)
        font_printf(FONT_META, PANEL_X, y + 46 + line * 14, g_dim,
                    "%d without a release, left out", plan.skipped);
}

/* The picture alone: the card with the still or the film on it and its
   reflection in the water. */
/* PIC1.PNG, 480 by 272, which is the size of the screen because that is what
   an EBOOT carries it for. It is not there while somebody is working the
   list: behind rows of lettering a busy picture only makes the reading
   harder. Left alone for ten seconds it comes up behind everything, the way
   the XMB puts a game's picture behind its own menu, and the interface goes
   on standing over it with the film still playing on the card. A key takes
   it away again in a few frames. */
static void draw_backdrop(void) {
    int alpha;
    if (g_rest <= 0.0f) return;
    const struct gfx_texture *pic = preview_still(&alpha);
    if (!pic || alpha <= 0) return;
    gfx_texture_draw(pic, 0, 0, SCR_W, SCR_H,
                     RGBA(255, 255, 255, alpha * (int)(255.0f * g_rest) / 255));
}

static void draw_picture(float t) {
    /* The card stands still: a picture that drifts is a picture that is
       hard to look at. What moves is the light over it. */
    struct gfx_card card;
    card.cx = PANEL_X + SHOT_W / 2;
    card.cy = SHOT_Y + SHOT_H / 2;
    card.w = SHOT_W;
    card.h = SHOT_H;
    card.yaw = 0.0f;
    card.pitch = 0.0f;
    /* A picture, not lettering: it gets the frame and the shadow. */
    card.bare = 0;
    /* No mirrored strip under the card: what the picture is reflected in is
       the water, which is under it anyway and moving. */
    card.reflect_h = 0;
    /* No sweep across the glass: a highlight travelling over a picture reads
       as a smear on the screen rather than as light in the room. The light
       that used to cross the card crosses the water now, where it has a
       surface to lie on. */
    card.gloss = -1.0f;

    /* Backlit: the light sits behind the picture and leaks out around it. */
    gfx_glow(card.cx, card.cy, SHOT_W + 130, SHOT_H + 120, rgb_pack(g_tint, 100));

    int still_alpha, film_alpha;
    const struct gfx_texture *still = preview_still(&still_alpha);
    const struct gfx_texture *film = preview_film(&film_alpha);
    enum preview_state picture = preview_state();
    /* The film at the size it was made. An ICON1.PMF is 144 by 80, which is
       what the XMB plays and what its author framed; across the card's width
       it would be those pixels blown up half again and stretched besides,
       since the card carries the screen's shape and the clip does not. Only
       a film larger than the card is scaled, and then by the same amount in
       both directions. */
    float fw = 0.0f, fh = 0.0f;
    if (film) {
        float fit = 1.0f;
        if (film->w > SHOT_W) fit = (float)SHOT_W / film->w;
        if (film->h * fit > SHOT_H) fit = (float)SHOT_H / film->h;
        fw = film->w * fit;
        fh = film->h * fit;
    }
    /* The still at the film's size too, since the film is what follows it
       on the card in nearly every case: a picture that stood at the card's
       full width and then dropped to a clip half as wide read as the
       interface changing its mind. Full size, the picture is the room's
       backdrop once the interface rests. */
    float sw = 0.0f, sh = 0.0f;
    if (still) {
        float fit = (float)FILM_W / still->w;
        if (still->h * fit > FILM_H) fit = (float)FILM_H / still->h;
        sw = still->w * fit;
        sh = still->h * fit;
    }
    /* One or the other on the card, never both: where there is a film it is
       the film, at full strength from its first frame rather than fading in
       over the picture it was cut from. The picture is not lost -- it is the
       room's own backdrop once the interface goes. */
    if (film) film_alpha = 255;
    int only_film = film != 0;
    if (still || film) {
        /* The reflection first and under everything: it runs down over the
           water where the lines below the card are about to be written, and
           it belongs behind them. Whichever of the two is on the card, at
           the width it is drawn: the film's reflection is as wide as the
           film, not as wide as the card. */
        if (still && !only_film)
            lattice_mirror(still, still_alpha, card.cx - sw / 2,
                           SHOT_Y + SHOT_H, sw, sh);
        if (film)
            lattice_mirror(film, film_alpha, card.cx - fw / 2,
                           SHOT_Y + SHOT_H, fw, fh);
        if (still && !only_film) {
            struct gfx_card plate = card;
            plate.w = sw;
            plate.h = sh;
            plate.bare = 1;
            plate.alpha = still_alpha;
            gfx_card_draw(still, &plate);
        }
        if (film) {
            /* Plain: the film's own pixels and nothing around them. A frame
               and a shadow the size of the clip, inside the space the card
               keeps, read as a box in a box; the XMB puts the film on the
               screen, not in a picture frame. */
            struct gfx_card screen = card;
            screen.w = fw;
            screen.h = fh;
            screen.bare = 1;
            screen.alpha = film_alpha;
            gfx_card_draw(film, &screen);
        }
    } else {
        /* Nothing on the card yet. What is coming is almost always the
           film, and a film out of an EBOOT is 144 by 80, so the empty card
           is that size rather than the full one: a plate that shrinks by
           half the moment the picture arrives reads as a mistake. */
        card.w = FILM_W;
        card.h = FILM_H;
        card.alpha = 255;
        card.bare = 1;
        gfx_card_draw(0, &card);
        if (picture == PREVIEW_LOADING)
            gfx_glow(card.cx, card.cy, 70 + sinf(t * 4) * 14, 36 + sinf(t * 4) * 8,
                     rgb_pack(g_tint, 120));
        const char *note = picture == PREVIEW_LOADING ? "loading"
                         : picture == PREVIEW_MISSING ? "no picture" : "";
        font_print(FONT_META, card.cx - font_width(FONT_META, note) / 2,
                   card.cy + 4, g_dim, note);
    }
}

static void draw_panel(const struct app_entry *entry, float t) {
    draw_picture(t);

    /* Under the picture, the way the system does it under an icon: the
       name, and one line saying where it stands -- what is installed, what
       waits, or what it would weigh to fetch. The summary, dimmer, once and
       clipped; a card is not a page. */
    int y = SHOT_Y + SHOT_H + REFLECT_H + 16;
    draw_shade(PANEL_X + SHOT_W / 2, y + 6, SHOT_W, 40);

    static char line[64];
    static const struct app_entry *line_of;
    static enum app_state line_state;
    static int line_basket;
    int in_basket = shell_basket_has((int)(entry - g_catalog->apps));
    unsigned state_color = g_dim;
    if (entry != line_of || entry->state != line_state || in_basket != line_basket) {
        line_of = entry;
        line_state = entry->state;
        line_basket = in_basket;
        char size[24] = "";
        if (entry->has_release && entry->release.size)
            size_mb(entry->release.size, size, sizeof(size));
        switch (entry->state) {
        case APP_UPDATE:
            snprintf(line, sizeof(line), "Update to %s   %s", entry->remote_version, size);
            break;
        case APP_UNKNOWN:
            snprintf(line, sizeof(line), "Installed %s", entry->local_version);
            break;
        case APP_CURRENT:
            snprintf(line, sizeof(line), "Installed %s", entry->local_version);
            break;
        default:
            snprintf(line, sizeof(line), "%s%s", size[0] ? size : "No release",
                     in_basket ? "   in the basket" : "");
            break;
        }
    }
    if (entry->state == APP_UPDATE) state_color = RGB(140, 255, 170);
    /* The state on the name's own line, after it, the way the size stands
       after a title in the system's lists; the summary under both. */
    float nw = font_width(FONT_H1, entry->name), sw = font_width(FONT_META, line);
    if (nw + 10 + sw <= SHOT_W) {
        float x = font_print(FONT_H1, PANEL_X, y, g_text, entry->name);
        font_print(FONT_META, x + 10, y, state_color, line);
    } else {
        /* A long name keeps its line whole; the state takes the next. */
        font_print_clipped(FONT_H1, PANEL_X, y, SHOT_W, g_text, entry->name);
        y += 20;
        font_print_clipped(FONT_META, PANEL_X, y, SHOT_W, state_color, line);
    }
    font_print_clipped(FONT_META, PANEL_X, y + 22, SHOT_W, faded(g_dim, 170),
                       entry->summary);
}

/* --------------------------------------------------------------- overlays */

/* Everything modal is the same shape, the one the system's own message dialog
   has: a band the full width of the screen, the room still moving above and
   below it, held by a line of light along each edge that fades away toward
   both walls. Nothing here has a left side, a right side or a corner. */
#define BAND_H 118
#define BAND_Y ((SCR_H - BAND_H) / 2)
#define MENU_H 226
#define MENU_Y ((SCR_H - MENU_H) / 2)
#define INFO_Y 36
#define INFO_H (FOOTER_Y - 6 - INFO_Y)

static void band_edge(int y) {
    unsigned bright = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.55f), 210);
    unsigned clear = rgb_pack(g_tint, 0);
    gfx_hgrad(0, y, SCR_W / 2, 1, clear, bright);
    gfx_hgrad(SCR_W / 2, y, SCR_W / 2, 1, bright, clear);
    /* The line is one pixel; what makes it read as light is under it. */
    gfx_glow(SCR_W / 2, y, 460, 9, rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.4f), 70));
}

/* A line of the same kind but shorter, for dividing a band's inside. */
static void band_rule(int y, int half_w, int alpha) {
    unsigned faint = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.4f), alpha);
    unsigned clear = rgb_pack(g_tint, 0);
    gfx_hgrad(SCR_W / 2 - half_w, y, half_w, 1, clear, faint);
    gfx_hgrad(SCR_W / 2, y, half_w, 1, faint, clear);
}

static void draw_band(int y, int h) {
    /* The whole room steps back a little so the band is the front. */
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, 110));
    gfx_rect(0, y, SCR_W, h, RGBA(0, 0, 0, 215));
    /* Flat dark over the screenshot card still leaves it the brightest thing
       on screen and the text on top of it unreadable, so the band is darkest
       where the words are and lets the room back in toward the walls. */
    gfx_shade(SCR_W / 2.0f, y + h / 2.0f, SCR_W * 1.7f, h * 1.1f, 170);
    band_edge(y);
    band_edge(y + h);
}

/* One button and the word for what it does, the mark sitting on the text's
   own line. Returns where the next one may start; a row that has to be
   centred is measured with the same arithmetic first. */
#define HINT_GAP 5              /* between a mark and its word */
#define HINT_SPACE 26           /* between one hint and the next */

static float hint_width(enum mark m, const char *text) {
    return mark_width(m) + HINT_GAP + font_width(FONT_META, text);
}

/* The mark a shade brighter than the word: the button is what the eye
   looks for, the word is what it reads once it has found it. */
static float draw_hint(float x, float base, enum mark m, const char *text,
                       unsigned color) {
    unsigned bright = faded(rgb_pack(rgb_mix(RGB_WHITE, g_tint, 0.1f), 255),
                            (int)(((color >> 24) & 0xFF) * 0.9f + 25));
    mark_draw(m, x + mark_width(m) / 2.0f, base - 4, bright, MARK_PLAIN, 0, 0);
    return font_print(FONT_META, x + mark_width(m) + HINT_GAP, base, color, text)
         + HINT_SPACE;
}

/* The row of buttons a band carries at its foot, centred and both the same
   weight: which one is taken is decided by the button pressed, not by a
   cursor sitting on one of them. */
static void draw_answers(float base, const char *yes, const char *no) {
    float x = SCR_W / 2 - (hint_width(MARK_CROSS, yes) + 30 + hint_width(MARK_CIRCLE, no)) / 2;
    x = draw_hint(x, base, MARK_CROSS, yes, g_text) - HINT_SPACE + 30;
    draw_hint(x, base, MARK_CIRCLE, no, g_text);
}

/* ------------------------------------------------------------------- ask */

static void draw_ask(void) {
    draw_band(BAND_Y, BAND_H);
    float w = font_width(FONT_BODY, g_ask_title);
    font_print_clipped(FONT_BODY, SCR_W / 2 - w / 2, BAND_Y + 44, SCR_W - 40,
                       g_text, g_ask_title);
    w = font_width(FONT_META, g_ask_line);
    font_print_clipped(FONT_META, SCR_W / 2 - w / 2, BAND_Y + 68, SCR_W - 40,
                       g_dim, g_ask_line);
    draw_answers(BAND_Y + BAND_H - 22, "Yes", "No");
}

/* ------------------------------------------------------------------ menu */

/* The system's own options panel: it slides in from the right edge over
   the dimmed room and stands there, the title at its head and the choices
   under it, one lit. It is the one place every key is named: a row that a
   key does directly carries that key's mark at its end. */
#define MENU_W 214

static void draw_menu(void) {
    /* Eased both ways: a fifth of the way there each frame is a slide that
       lands without a bump, about a quarter of a second either way. */
    float goal = g_menu_leaving ? 0.0f : 1.0f;
    g_menu_slide += (goal - g_menu_slide) * 0.22f;
    if (g_menu_leaving && g_menu_slide < 0.02f) {
        g_menu_count = 0;
        g_menu_leaving = 0;
        return;
    }
    float x0 = SCR_W - MENU_W * g_menu_slide;

    gfx_rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, (int)(110 * g_menu_slide)));
    gfx_rect((int)x0, 0, MENU_W + 2, SCR_H, RGBA(0, 0, 0, 205));
    gfx_shade(x0 + MENU_W / 2.0f, SCR_H / 2.0f, MENU_W * 1.6f, SCR_H * 1.3f, 150);
    /* The edge: a line of the room's light, fading up and down the way the
       bands' edges do. */
    unsigned bright = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.5f), 150);
    unsigned clear = rgb_pack(g_tint, 0);
    gfx_vgrad((int)x0, 0, 1, SCR_H / 2, clear, bright);
    gfx_vgrad((int)x0, SCR_H / 2, 1, SCR_H / 2, bright, clear);

    float left = x0 + 18, right = x0 + MENU_W - 16;
    font_print_clipped(FONT_BODY, left, 34, right - left, g_text, g_menu_title);
    gfx_hgrad((int)left, 44, MENU_W - 34, 1, bright, clear);

    unsigned grey = rgb_pack(rgb_mix(NIGHT_BOTTOM, RGB_WHITE, 0.32f), 255);
    for (int i = 0; i < g_menu_count; i++) {
        int y = 78 + i * 26;
        int on = i == g_menu_cursor;
        if (on) {
            gfx_glow(x0 + MENU_W / 2.0f, y - 5, MENU_W + 60, 30, rgb_pack(g_tint, 120));
            gfx_glow(x0 + MENU_W / 2.0f, y + 5, MENU_W - 20, 8,
                     rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.7f), 150));
        }
        unsigned color = !g_menu_on[i] ? grey : on ? g_text : g_dim;
        /* The key that does this without the menu, at the row's end and
           in its own colour rather than the row's: it is a note about the
           row, not part of it. */
        float room = right - left;
        if (g_menu_key[i] >= 0) {
            enum mark m = (enum mark)g_menu_key[i];
            mark_draw(m, right - mark_width(m) / 2.0f, y - 4, faded(g_dim, on ? 220 : 140),
                      MARK_PLAIN, 0, 0);
            room -= mark_width(m) + 10;
        }
        /* A row can name a thing by its mark: the byte 1 and then the mark
           plus one end the words, and the mark stands after them. */
        const char *glyph = strchr(g_menu_item[i], '\x01');
        if (glyph && glyph[1]) {
            char words[32];
            snprintf(words, sizeof(words), "%.*s", (int)(glyph - g_menu_item[i]), g_menu_item[i]);
            float end = font_print_clipped(FONT_BODY, left, y, room, color, words);
            enum mark gm = (enum mark)(glyph[1] - 1);
            mark_draw(gm, end + 6 + mark_width(gm) / 2.0f, y - 4, color, MARK_PLAIN, 0, 0);
        } else {
            font_print_clipped(FONT_BODY, left, y, room, color, g_menu_item[i]);
        }
    }
    /* Enter and back, the way every band ends, at the panel's foot. */
    float hx = left;
    hx = draw_hint(hx, SCR_H - 14, MARK_CROSS, "Enter", g_dim);
    draw_hint(hx, SCR_H - 14, MARK_CIRCLE, "Back", g_dim);
}

/* --------------------------------------------------------------- install */

static void draw_install(void) {
    draw_band(BAND_Y, BAND_H);

    int bar_x = 60, bar_w = SCR_W - 120, bar_y = BAND_Y + 84;
    font_print_clipped(FONT_BODY, bar_x, BAND_Y + 44, bar_w, g_text, g_install_name);
    font_print(FONT_META, bar_x, BAND_Y + 68, g_dim, g_install_phase);

    /* Three pixels of line, not a trough with a fill: the bar is the same
       kind of thing as the band's own edges. */
    gfx_rect(bar_x, bar_y, bar_w, 3, RGBA(255, 255, 255, 28));
    if (g_install_total) {
        /* Progress arrives in whatever lumps the network hands over, and a
           bar that steps by those lumps reads as a stall between them. The
           drawn end chases the reported one instead, a third of the way per
           frame, so it is always moving and never ahead. */
        float target = (float)g_install_done / g_install_total;
        g_bar += (target - g_bar) * 0.34f;
        if (target >= 1.0f && g_bar > 0.995f) g_bar = 1.0f;
        int filled = (int)(bar_w * g_bar + 0.5f);
        gfx_hgrad(bar_x, bar_y, filled, 3, rgb_pack(g_tint, 255), g_accent);
        gfx_glow(bar_x + filled, bar_y + 1, 44, 22, rgb_pack(RGB_WHITE, 150));
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%",
                 (int)((unsigned long long)g_install_done * 100 / g_install_total));
        font_print(FONT_META, bar_x + bar_w - font_width(FONT_META, pct),
                   BAND_Y + 68, g_dim, pct);
    } else if (g_install_done) {
        /* No content-length: show that bytes are moving, not how far. */
        int slide = (int)(gfx_frames() * 3 % (unsigned)bar_w);
        int w = 40 > bar_w - slide ? bar_w - slide : 40;
        gfx_hgrad(bar_x + slide, bar_y, w, 3, g_accent, rgb_pack(g_tint, 0));
    }
    /* Circle calls it off while there is still something to call off: the
       rename at the end is not left half done, so the key is not offered
       there. */
    if (strcmp(g_install_phase, "commit") != 0) {
        float hw = hint_width(MARK_CIRCLE, "Cancel");
        draw_hint(SCR_W / 2 - hw / 2, BAND_Y + BAND_H - 18, MARK_CIRCLE, "Cancel", g_dim);
    }
}

/* -------------------------------------------------------------- info band */

/* Labels end and values begin at the same places all the way down, so the
   rows read as a column of facts and not as a page of sentences. The two
   that need the room -- where the catalog is and what was negotiated with it
   -- have the band to themselves; the short ones pair up. */
#define FACT_LABEL 150
#define FACT_VALUE 166
#define FACT_LABEL2 330
#define FACT_VALUE2 346

static void fact(int y, float label_end, float value_x, float clip,
                 const char *label, const char *value) {
    font_print(FONT_META, label_end - font_width(FONT_META, label), y, g_dim, label);
    font_print_clipped(FONT_META, value_x, y, clip, g_text, value);
}

/* wolfSSL names a suite the way its own tables do -- TLS13-CHACHA20-POLY1305-
   SHA256. In a TLS 1.3 row the version is already said and the hash cannot be
   anything else, so both ends come off and what is left is the part that
   differs between one connection and the next. */
static void tidy_cipher(const char *name, char *out, size_t size) {
    const char *cut = name;
    if (strncmp(cut, "TLS13", 5) == 0) cut += 5;
    else if (strncmp(cut, "TLS", 3) == 0) cut += 3;
    if (*cut == '-' || *cut == '_') cut++;
    snprintf(out, size, "%s", *cut ? cut : name);
    for (char *p = out; *p; p++) if (*p == '_') *p = '-';
    size_t n = strlen(out);
    if (n > 7 && strncmp(out + n - 7, "-SHA", 4) == 0) out[n - 7] = '\0';
}

/* The catalog is named by where it came from, not by which file on it. */
static void url_host(const char *url, char *out, size_t size) {
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    size_t n = strcspn(host, "/");
    if (n >= size) n = size - 1;
    memcpy(out, host, n);
    out[n] = '\0';
}

/* What is left on the stick. The driver counts in clusters and answers
   through a pointer handed to it in a struct, which is the one call in this
   file that looks like firmware because it is. Asked once when the band
   opens: a FAT32 free count walks the allocation table. */
static char g_storage[24] = "?";
static float g_storage_used = -1.0f;    /* below zero while the stick is unread */

static void size_words(unsigned long long bytes, char *out, size_t size) {
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(out, size, "%lu.%lu GB", (unsigned long)(bytes >> 30),
                 (unsigned long)((bytes * 10 >> 30) % 10));
    else
        snprintf(out, size, "%lu MB", (unsigned long)(bytes >> 20));
}

static void read_storage(void) {
    struct ms_info {
        unsigned max_clusters, free_clusters, max_sectors, sector_size, sector_count;
    } info;
    struct { struct ms_info *at; } command = { &info };
    memset(&info, 0, sizeof(info));
    g_storage_used = -1.0f;
    if (sceIoDevctl(storage_device(), 0x02425818, &command, sizeof(command), NULL, 0) < 0) {
        snprintf(g_storage, sizeof(g_storage), "unknown");
        return;
    }
    unsigned long long unit = (unsigned long long)info.sector_count * info.sector_size;
    unsigned long long left = info.free_clusters * unit;
    unsigned long long all = info.max_clusters * unit;
    size_words(left, g_storage, sizeof(g_storage));
    if (all) g_storage_used = 1.0f - (float)((double)left / (double)all);
}

/* What the gear holds: the things this session can do to itself, and last
   the band that says what it is. A row here is read and taken the way a
   package's row is, because at this depth nothing is deeper. */
static const char *const SETTING[SHELL_SETTINGS] = {
    "Add sources",
    "Direct install",
    "Reset",
    "Info",
};

/* What each row does, said on the right while the cursor is on it: the
   list names the thing, the panel says what it comes to. */
static const char *const SETTING_NOTE[SHELL_SETTINGS] = {
    "The lists this console reads apps from: a catalog, a list of repositories, or one repository. Add one by its URL, or take one out.",
    "One app straight from its GitHub repository, or the .pspdx files put in PSP/PSPDX/INBOX.",
    "Sweep the stick again for fresh TLS entropy, or put PSPDX back to its first start: everything under PSP/PSPDX goes, the apps in PSP/GAME stay.",
    "What this session is connected to and what it is standing on.",
};

const char *shell_setting(int n) {
    return n >= 0 && n < SHELL_SETTINGS ? SETTING[n] : "";
}

/* Four rows at the foot leave the facts above them 18 pixels apart rather
   than 24, which the small face reads at without touching. */
#define ACTION_Y (INFO_Y + 148)
#define ACTION_H 18

static void draw_info(void) {
    draw_band(INFO_Y, INFO_H);

    const struct https_info *tls = https_last();
    char value[96];

    /* Which build this is, above the rest: the one fact the band states about
       itself rather than about the run. */
    fact(INFO_Y + 18, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30,
         "PSPDX", PSPDX_VERSION);

    url_host(catalog_url(), value, sizeof(value));
    fact(INFO_Y + 36, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30,
         "Catalog", value);

    if (tls->cipher[0]) {
        char cipher[48];
        tidy_cipher(tls->cipher, cipher, sizeof(cipher));
        snprintf(value, sizeof(value), "TLS 1.3  %s  %s", cipher, tls->group);
    } else {
        snprintf(value, sizeof(value), "not connected");
    }
    fact(INFO_Y + 54, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30,
         "Connection", value);

    snprintf(value, sizeof(value), "%u ms", tls->handshake_ms);
    fact(INFO_Y + 76, FACT_LABEL, FACT_VALUE, 140, "Handshake", value);

    snprintf(value, sizeof(value), "%d bits", entropy_bits());
    fact(INFO_Y + 96, FACT_LABEL, FACT_VALUE, 140, "Entropy", value);

    /* Not a scheduler's number -- the PSP has none to ask. The share of each
       frame that goes into drawing it; the rest is the wait for vblank, which
       is the only idle this client has. */
    snprintf(value, sizeof(value), "%d fps, %d%% drawing",
             g_frame_us > 0.0f ? (int)(1000000.0f / g_frame_us + 0.5f) : 0,
             (int)(g_load * 100.0f + 0.5f));
    fact(INFO_Y + 116, FACT_LABEL, FACT_VALUE, 160, "Frames", value);

    int installed = 0;
    if (g_catalog)
        for (int i = 0; i < g_catalog->count; i++)
            if (g_catalog->apps[i].state != APP_NOT_INSTALLED) installed++;
    snprintf(value, sizeof(value), "%d", installed);
    fact(INFO_Y + 76, FACT_LABEL2, FACT_VALUE2, 110, "Installed", value);

    snprintf(value, sizeof(value), "%u KB",
             (unsigned)sceKernelTotalFreeMemSize() / 1024);
    fact(INFO_Y + 96, FACT_LABEL2, FACT_VALUE2, 110, "Memory free", value);

    /* Room on the stick is the one fact here that is a proportion, so it is
       drawn as one: the line fills as the stick does, and what is left of it
       is what a package has to fit into. */
    fact(INFO_Y + 116, FACT_LABEL2, FACT_VALUE2, 110, "Stick free", g_storage);
    if (g_storage_used >= 0.0f) {
        int x = FACT_VALUE2, w = SCR_W - FACT_VALUE2 - 30, y = INFO_Y + 125;
        int used = (int)(w * g_storage_used + 0.5f);
        gfx_rect(x, y, w, 3, RGBA(255, 255, 255, 28));
        if (used > 0) gfx_hgrad(x, y, used, 3, rgb_pack(g_tint, 255), g_accent);
    }

    band_rule(INFO_Y + 134, 160, 120);
    float w = hint_width(MARK_CIRCLE, "Back");
    draw_hint(SCR_W / 2 - w / 2, INFO_Y + 156, MARK_CIRCLE, "Back", g_dim);
}

/* --------------------------------------------------------------- details */

/* Prints text over at most `lines` lines of `width`, breaking at spaces,
   the last line clipped. Returns the lines used. */
static int draw_wrapped(enum font_style style, float x, float y, float width,
                        float step, int lines, unsigned color, const char *text) {
    char line[128];
    int used = 0;
    const char *p = text;
    while (*p && used < lines) {
        if (used == lines - 1) {
            font_print_clipped(style, x, y + used * step, width, color, p);
            return used + 1;
        }
        size_t n = strlen(p), fit = n;
        while (fit > 0) {
            snprintf(line, sizeof(line), "%.*s", (int)fit, p);
            if (font_width(style, line) <= width) break;
            size_t k = fit - 1;
            while (k > 0 && p[k] != ' ') k--;
            fit = k;
        }
        if (fit == 0 || fit == n) {
            font_print_clipped(style, x, y + used * step, width, color, p);
            return used + 1;
        }
        snprintf(line, sizeof(line), "%.*s", (int)fit, p);
        font_print(style, x, y + used * step, color, line);
        p += fit;
        while (*p == ' ') p++;
        used++;
    }
    return used ? used : 1;
}

/* Everything the catalog says about the one package, in the band the rest
   of the questions are asked in: the card is for looking, this is for
   reading. */
static void draw_details(void) {
    const struct app_entry *e = g_details;
    char value[96], size[24];
    draw_band(INFO_Y, INFO_H);

    float w = font_width(FONT_BODY, e->name);
    font_print_clipped(FONT_BODY, SCR_W / 2 - w / 2, INFO_Y + 24, SCR_W - 40,
                       g_text, e->name);
    band_rule(INFO_Y + 34, 150, 110);

    int y = INFO_Y + 56;
    if (e->state == APP_UPDATE)
        snprintf(value, sizeof(value), "%.31s installed, %.31s published",
                 e->local_version, e->remote_version);
    else if (e->state != APP_NOT_INSTALLED)
        snprintf(value, sizeof(value), "%s installed", e->local_version);
    else if (e->has_release)
        snprintf(value, sizeof(value), "%s", e->release.version);
    else
        snprintf(value, sizeof(value), "unknown");
    fact(y, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30, "Version", value);
    fact(y + 20, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30, "Author", e->author);
    fact(y + 40, FACT_LABEL, FACT_VALUE, 140, "Licence", e->license);
    fact(y + 40, FACT_LABEL2, FACT_VALUE2, 110, "Category", e->category);
    if (e->has_release && e->release.size) {
        size_mb(e->release.size, size, sizeof(size));
        fact(y + 60, FACT_LABEL, FACT_VALUE, 140, "Size", size);
    }
    fact(y + 80, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30, "Id", e->id);

    if(e->release.checked_at){
        time_t checked=e->release.checked_at;struct tm *date=gmtime(&checked);
        char when[24]="unknown";if(date)strftime(when,sizeof(when),"%Y-%m-%d %H:%M UTC",date);
        snprintf(value,sizeof(value),"%s%s",e->fresh?"":"Saved: ",when);
    }else snprintf(value,sizeof(value),"%s",e->fresh?"Checked this session":"Saved result; check time unknown");
    fact(y+100,FACT_LABEL,FACT_VALUE,SCR_W-FACT_VALUE-30,"Checked",value);
    band_rule(y + 116, 160, 120);
    draw_wrapped(FONT_META, 40, y + 134, SCR_W - 80, 16, 2, g_dim, e->summary);
}

void shell_details(const struct app_entry *entry) { g_details = entry; }

/* ---------------------------------------------------------------- footer */

static void draw_footer(void) {
    /* The system's own screens carry no legend: the keys are the keys, and
       a strip that names them names nothing. What the strip is for is the
       one line that is not a legend -- what is being waited for -- and the
       way back out of a band that fills the screen. Otherwise there is no
       strip, and the water runs to the edge. */
    if (g_ask_title[0] || g_menu_count || g_installing) return;
    if (!g_details && !g_status[0]) return;
    /* No edge: the strip comes in as a shadow rising from the bottom, the
       way the PSP's own bars sit on their backgrounds. */
    gfx_vgrad(0, FOOTER_Y - 28, SCR_W, 28, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 120));
    gfx_vgrad(0, FOOTER_Y, SCR_W, SCR_H - FOOTER_Y, RGBA(0, 0, 0, 120),
              RGBA(0, 0, 0, 200));
    if (g_details) {
        draw_hint(LIST_X, FOOTER_BASE, MARK_CIRCLE, "Back", g_dim);
    } else {
        font_print_clipped(FONT_META, LIST_X, FOOTER_BASE, SCR_W - 2 * LIST_X,
                           g_accent, g_status);
    }
}

/* ------------------------------------------------------------- water light */

/* A light that crosses the water instead of the picture: one pass every
   twelve seconds, lying on the surface at a fixed depth, so perspective gives
   it its shape -- wide, flat, and the same height above the horizon all the
   way across. It is placed the way everything else that stands on the water
   is placed, by projecting a point of the world, and it fades in and out at
   the two ends rather than sliding off an edge. */
#define LIGHT_Z 2.2f
#define LIGHT_EYE_Y (150.0f / GFX_FOCAL)     /* the lattice's own eye height */

static void draw_water_light(float t) {
    float cycle = fmodf(t, 12.0f) / 12.0f;
    /* Far enough past both walls that the fade, not the edge, ends the pass. */
    float wx = (cycle * 2.0f - 1.0f) * (SCR_W * 0.62f * LIGHT_Z / GFX_FOCAL);
    float sx, sy;
    gfx_water_project(wx, -LIGHT_EYE_Y, LIGHT_Z, &sx, &sy);
    float scale = GFX_FOCAL / LIGHT_Z;
    float fade = sinf(cycle * 3.1415927f);
    fade *= fade;
    struct rgb lit = rgb_mix(g_tint, RGB_WHITE, 0.62f);
    gfx_glow(sx, sy, scale * 0.95f, scale * 0.30f, rgb_pack(lit, (int)(95 * fade)));
    gfx_glow(sx, sy, scale * 0.40f, scale * 0.11f, rgb_pack(RGB_WHITE, (int)(70 * fade)));
}

/* ------------------------------------------------------------------ frame */

/* Where the slowest frame of a window spent its time, in microseconds:
   the backdrop, the text and cards, the sync with the GE plus the wait
   for vblank. Read and reset by shell_profile(). */
static unsigned g_worst_total, g_worst_back, g_worst_front, g_worst_end;

void shell_profile(char *out, int size) {
    unsigned ge, vblank;
    gfx_frame_worst(&ge, &vblank);
    snprintf(out, size, "slowest draw %u us: back %u, front %u, end %u (ge %u, vblank %u)",
             g_worst_total, g_worst_back, g_worst_front, g_worst_end, ge, vblank);
    g_worst_total = g_worst_back = g_worst_front = g_worst_end = 0;
}

/* The right half while the cursor is on a row under the gear: the row's
   name at the size the panel gives a package, and under it what taking the
   row comes to. */
static void draw_setting_panel(int n) {
    int y = SHOT_Y + 14;
    draw_shade(PANEL_X + SHOT_W / 2, y + 30, SHOT_W, 90);
    font_print(FONT_H1, PANEL_X, y, g_text, shell_setting(n));
    draw_wrapped(FONT_META, PANEL_X, y + 24, SHOT_W, 16, 3, g_dim, SETTING_NOTE[n]);
}

void shell_draw(const struct catalog *catalog, int cursor) {
    g_catalog = catalog;
    g_cursor = cursor;
    float t = gfx_frames() * (1.0f / 60.0f);
    unsigned t0 = now_us();

    /* The room changes colour with the selection, but slowly: an eighth of
       the way per frame is a crossfade, not a flash. */
    static struct rgb target = { 80, 140, 255 };
    static int lit_for = -1;
    if (catalog->count <= 0) target = DEFAULT_TINT;
    else if (cursor != lit_for) {
        lit_for = cursor;
        target = draw_lot();
        /* The water is told the colour outright, so the front that runs
           out from the ring carries it whole from its first frame; the
           rest of the room eases toward it below. */
        lattice_tint(target);
    }
    g_tint = rgb_mix(g_tint, target, 0.12f);
    derive_palette();

    /* The word for the wait is baked between frames, once per word. */
    if (catalog->count <= 0 && g_status[0]) title_prepare(g_word, g_tint);

    gfx_frame_begin(0xFF000000);
    gfx_vgrad(0, 0, SCR_W, SCR_H, rgb_pack(rgb_mix(NIGHT_TOP, g_tint, 0.05f), 255),
              rgb_pack(rgb_mix(NIGHT_BOTTOM, g_tint, 0.18f), 255));
    lattice_draw(t, g_tint);
    draw_water_light(t);
    /* Over the water and under everything that is read: the picture is the
       room the interface stands in while nobody is working it, which is
       where the XMB puts a game's picture too. */
    draw_backdrop();
    unsigned t1 = now_us();
    /* Left alone, the picture of the package under the cursor rises behind
       everything, over about two seconds; a key takes it down again in a
       few frames. */
    g_rest += g_resting ? 1.0f / 120.0f : -1.0f / 8.0f;
    if (g_rest < 0.0f) g_rest = 0.0f;
    if (g_rest > 1.0f) g_rest = 1.0f;
    /* And as the picture comes up the interface goes: left alone for long
       enough, what is on the screen is the package's own picture and
       nothing over it, until a key brings the rows back in a few frames. */
    gfx_veil((int)(256.0f * (1.0f - g_rest)));
    draw_chrome(catalog, t);
    if (catalog->count > 0 && shell_view_count() > 0) {
        int rows = shell_view_count();
        int index = shell_view_index(cursor < rows ? cursor : 0);
        draw_list(catalog, cursor, t);
        if (index == SHELL_ROW_ACTION) draw_action_panel(catalog, t);
        else if (index <= SHELL_ROW_SETTING) draw_setting_panel(SHELL_ROW_SETTING - index);
        else if (index >= 0) draw_panel(&catalog->apps[index], t);
    } else if (g_status[0]) {
        /* Nothing to browse yet: the word stands in the room, lit from
           behind; what it is waiting for is said in the strip below. */
        title_draw(SCR_W / 2.0f, 116.0f, t, g_tint);
    } else {
        font_print(FONT_BODY, LIST_X, 120, g_dim, "The catalog came back empty.");
    }
    if (g_info) draw_info();
    if (g_details) draw_details();
    if (g_menu_count) draw_menu();
    if (g_installing) draw_install();
    if (g_ask_title[0]) draw_ask();
    draw_footer();
    gfx_veil(256);
    if (g_fade > 0) {
        gfx_rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, g_fade));
        g_fade -= 7;
    }
    unsigned t2 = now_us();
    gfx_frame_end();
    unsigned t3 = now_us();
    /* Drawing against the whole frame, which is this one's start to the
       next one's: what is not drawing is the vblank wait inside frame_end. */
    static unsigned began;
    if (began && t0 - began > 1000 && t0 - began < 200000) {
        g_load += ((float)(t2 - t0) / (t0 - began) - g_load) * 0.05f;
        g_frame_us += ((float)(t0 - began) - g_frame_us) * 0.05f;
    }
    began = t0;
    if (t3 - t0 > g_worst_total) {
        g_worst_total = t3 - t0;
        g_worst_back = t1 - t0;
        g_worst_front = t2 - t1;
        g_worst_end = t3 - t2;
    }
}

int shell_settled(void) {
    float target = LIST_Y + (g_cursor - g_first) * ITEM_H;
    float bar = g_sel_y - target;
    int picture_done = !g_catalog || g_catalog->count <= 0 || preview_settled();
    return g_fade <= 0 && picture_done && bar > -1.0f && bar < 1.0f;
}

/* ------------------------------------------------------------- screenshot */

void shell_shot_sync(const struct catalog *catalog, int cursor) {
    int index = shell_view_index(cursor);
    if (catalog->count <= 0 || index == -1) return;
    /* The action row is a row with no package behind it, and the card is
       told to show nothing rather than left holding whatever the cursor
       passed on its way here. */
    const struct app_entry *entry = index >= 0 ? &catalog->apps[index] : 0;

    /* A row number is not enough to say the selection changed: removing an
       entry from the basket slides the rows up under a cursor that has not
       moved, and the row then stands for another package. */
    static const struct app_entry *g_shown;
    if (cursor != g_last_cursor || entry != g_shown) {
        /* The first selection is the app starting up, not the user
           scrolling past: nothing to wait for. */
        int first = g_last_cursor < 0;
        g_last_cursor = cursor;
        g_shown = entry;
        g_status[0] = '\0';
        lattice_touch((LIST_X + LIST_W / 2) / (float)SCR_W);
        preview_show(entry, first);
    }
    if (preview_tick()) {
        shell_draw(catalog, cursor);        /* say so before we block */
        preview_load();
    }
}

void shell_word(const char *word) {
    snprintf(g_word, sizeof(g_word), "%s", word ? word : "Connecting");
}

void shell_status(const char *text) {
    snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
}

void shell_ask(const char *title, const char *line) {
    snprintf(g_ask_title, sizeof(g_ask_title), "%s", title ? title : "");
    snprintf(g_ask_line, sizeof(g_ask_line), "%s", line ? line : "");
    /* The last install's result has been overtaken by a new question. */
    if (g_ask_title[0]) g_status[0] = '\0';
}

void shell_menu(const char *title, const char *const *items,
                const unsigned char *takeable, const signed char *keys,
                int count, int cursor) {
    if (count > MENU_MAX) count = MENU_MAX;
    if (count < 0) count = 0;
    g_menu_cursor = cursor;
    /* Closing is a slide out, so the rows stay until the panel is off the
       edge; the count says the menu is drawn, g_menu_leaving that it is
       on its way out. */
    if (!count) {
        g_menu_leaving = 1;
        return;
    }
    if (!g_menu_count || g_menu_leaving) g_menu_slide = 0.0f;
    g_menu_leaving = 0;
    g_menu_count = count;
    snprintf(g_menu_title, sizeof(g_menu_title), "%s", title ? title : "");
    for (int i = 0; i < count; i++) {
        snprintf(g_menu_item[i], sizeof(g_menu_item[i]), "%s", items[i] ? items[i] : "");
        g_menu_on[i] = takeable ? takeable[i] : 1;
        g_menu_key[i] = keys ? keys[i] : -1;
    }
    g_status[0] = '\0';
}

void shell_rest(int resting) { g_resting = resting; }

void shell_info(int open) {
    if (open && !g_info) read_storage();
    g_info = open;
}

/* ---------------------------------------------------------------- install */

void shell_install_begin(const char *name, int at, int of) {
    g_installing = 1;
    g_status[0] = '\0';
    /* One band for both: a lone install is its name, and one of a run says
       where in the run it is first, so the line changes as the run goes and
       the name it changes to is the one being fetched. */
    if (of > 1)
        snprintf(g_install_name, sizeof(g_install_name), "%d of %d: %s", at, of,
                 name ? name : "");
    else
        snprintf(g_install_name, sizeof(g_install_name), "%s", name ? name : "");
    g_install_phase[0] = '\0';
    g_install_done = g_install_total = 0;
    g_install_drawn_ms = 0;
    g_bar = 0.0f;
}

void shell_install_phase(void *ctx, const char *phase) {
    (void)ctx;
    snprintf(g_install_phase, sizeof(g_install_phase), "%s", phase ? phase : "");
    g_install_done = g_install_total = 0;
    g_install_drawn_ms = 0;
    g_bar = 0.0f;
    if (g_catalog) shell_draw(g_catalog, g_cursor);
}

void shell_install_progress(void *ctx, size_t done, size_t total) {
    (void)ctx;
    g_install_done = done;
    g_install_total = total;
    /* Every frame costs a vblank wait on the thread doing the downloading,
       so this cannot run at sixty; at twelve a second the eased end of the
       bar moves the way the system's own does, and the wait overlaps the
       pacing a real radio imposes anyway. */
    if (done != total && !expired(g_install_drawn_ms, 80)) return;
    g_install_drawn_ms = now_ms();
    if (g_catalog) shell_draw(g_catalog, g_cursor);
}

void shell_install_end(const char *message) {
    g_installing = 0;
    snprintf(g_status, sizeof(g_status), "%s", message ? message : "");
    logline("%s", g_status);
    lattice_touch(0.5f);
}
