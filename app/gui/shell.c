#include "text.h"
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
#include <stdlib.h>
#include <string.h>

#include "pspkit-https/entropy.h"
#include "pspkit-https/https.h"
#include "gui/shell.h"
#include "gui/shell_internal.h"
#include "gui/files_view.h"
#include "gui/font.h"
#include "gui/gfx.h"
#include "gui/icons.h"
#include "gui/lattice.h"
#include "gui/marks.h"
#include "gui/title.h"
#include "gui/wrap.h"
#include "gui/palette.h"
#include "gui/preview.h"
#include "session/view.h"
#include "update/pspdx.h"
#include "update/sources.h"
#include "util/runtime.h"

/* Three type roles and nowhere else a fourth: FONT_H1 for the one name on
   screen, FONT_BODY for the list and the summary, FONT_META for facts. */

#define HEADER_H 32
#define ITEM_H 32
/* The bundle's icon, 144x80 shown at a third: as tall as the row allows
   with a little air, and the name starts after it. */
#define ICON_W 43
#define ICON_H 24
#define NAME_X (LIST_X + ICON_W + 9)
#define VISIBLE ((FOOTER_Y - 6 - LIST_Y) / ITEM_H)

#define SHOT_W 224
#define SHOT_H (SHOT_W * SCR_H / SCR_W)     /* the screen's own 480:272 */
#define SHOT_Y 43
#define REFLECT_H 18
#define FILM_W 144                  /* an ICON1.PMF, as the firmware plays it */
#define FILM_H 80

/* ------------------------------------------------------------------ colour */

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
struct rgb g_tint = { 80, 140, 255 };
unsigned g_accent, g_text, g_dim;

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
static char g_ask_title[64], g_ask_line[200];
#define MENU_MAX 7                      /* rows the panel has room for */
static const struct menu *g_menu;       /* the caller's, while it is up */
static struct menu g_menu_gone;         /* its last rows, while it slides out */
static float g_menu_slide;              /* 0 off the right edge, 1 in place */
static int g_menu_leaving;              /* sliding out; done at 0 */
static int g_info;
static const struct app_entry *g_details;   /* the package the band is about */
/* What the band has to say under its facts -- the summary, a blank line and
   the description -- copied when it opens and broken into lines once then,
   since measuring kilobytes of text is no work for a frame. The copy is the
   band's own: a refetch may free the entry's description while it is up.
   A summary of 60 characters and a description of 2500 of up to four bytes
   each; a line per character at the most, and one for the blank. */
#define DETAIL_TEXT (sizeof(((struct app_entry *)0)->summary) + 2 + 2500 * 4 + 1)
#define DETAIL_LINES (60 + 1 + 2500)
#define DETAIL_LINE_BYTES 255
static char g_detail_text[DETAIL_TEXT];
static struct wrap_line g_detail_lines[DETAIL_LINES];
static int g_detail_count;
static float g_detail_scroll, g_detail_max;     /* pixels scrolled, and how far it can */
static int g_resting;                   /* left alone: where the picture is going */
static float g_rest;                    /* 0 no picture, 1 the picture whole */
/* The share of a frame that goes into drawing it, eased over about a second
   so the number on screen does not flicker. */
static float g_load;
static float g_frame_us;                /* frame to frame, eased, for the fps */
static char g_word[24] = T_WORD_CONNECTING;
static const struct catalog *g_catalog;
static int g_cursor;

/* The view's rows have come to stand for other packages than they did --
   another tab, another catalog: the list starts from the top and the card
   is told to fetch afresh, once, before either is read for the frame. */
static void follow_view(void) {
    static unsigned seen;
    unsigned now = view_generation();
    if (now == seen) return;
    seen = now;
    g_first = 0;
    g_last_cursor = -1;
}

int shell_init(void) {
    if (!font_init()) return 0;
    gfx_init();
    lattice_init();
    preview_init();
    return 1;
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
unsigned faded(unsigned color, int alpha) {
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
void draw_shade(int cx, int cy, int w, int h) {
    gfx_shade(cx, cy, w * 1.6f, h * 1.8f, 170);
}

/* A tab that is a job rather than a category is a sign and a number -- the
   turning arrows and how many wait, the basket and what is in it -- because
   the sign is the same one the rows below it carry and a word would not be.
   The number is built into a static, so it is read before the next call. */
static const char *tab_count(int tab) {
    static char text[8];
    snprintf(text, sizeof(text), "%d",
             tab == TAB_STICK ? view_updates_waiting() : view_basket_count());
    return text;
}

/* The stick's sign is the stick until something is waiting for it, and the
   update arrows with the count while something is. */
static enum mark tab_mark(int tab) {
    if (tab == TAB_GEAR) return MARK_GEAR;
    if (tab == TAB_BASKET) return MARK_BASKET;
    return view_updates_waiting() > 0 ? MARK_UPDATE : MARK_STICK;
}

static float tab_width(int tab) {
    if (tab >= 0) return mark_width(MARK_ALL + tab);
    if (tab == TAB_GEAR) return mark_width(MARK_GEAR);
    if (tab == TAB_STICK && view_updates_waiting() == 0) return mark_width(MARK_STICK);
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
    int tabs = view_tab_count(), at = view_tab_active();
    if (tabs <= 1) return;
    float x = TAB_X;
    for (int i = 0; i < tabs; i++) {
        if (view_tab_at(i) < 0) continue;
        draw_tab(view_tab_at(i), i == at, x, t);
        x += tab_width(view_tab_at(i)) + TAB_GAP;
    }
    x = TAB_X - TAB_GAP;
    for (int i = tabs - 1; i >= 0; i--) {
        if (view_tab_at(i) >= 0) continue;
        x -= tab_width(view_tab_at(i));
        draw_tab(view_tab_at(i), i == at, x, t);
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
    int tab = view_tab_current();
    const char *title = files_view_shown() ? T_HEAD_FILES : tab == TAB_GEAR ? T_HEAD_GEAR
                      : tab == TAB_STICK ? T_HEAD_STICK
                      : tab == TAB_BASKET ? T_HEAD_BASKET : view_tab_name(tab);
    gfx_glow(LIST_X + 24, 18, 110, 56, rgb_pack(g_tint, 80));
    font_print(FONT_H1, LIST_X, 23, g_text, title);
    if (catalog->count > 0) draw_tabs(0, 0, t);
}

/* ------------------------------------------------------------------- list */


/* The two words the action row is headed with, and the sentence under them.
   Both are wanted in the list and again in the panel, so they are made in one
   place. */
static const char *action_title(void) {
    if (view_tab_kind() != VIEW_TAB_STICK) return T_DOWNLOAD_ALL;
    return view_updates_waiting() > 0 ? T_UPDATE_ALL : T_CHECK;
}

/* "3 apps, 61.5 MB" -- or, when nothing in the tab has a release with a
   size, what is in the way instead. */
static const char *action_line(void) {
    static char line[48];
    struct view_plan plan;
    char size[24];
    view_action_plan(&plan);
    if (plan.apps <= 0 && view_tab_kind() == VIEW_TAB_STICK)
        snprintf(line, sizeof(line), T_ALL_CURRENT);
    else if (plan.apps <= 0)
        snprintf(line, sizeof(line), T_NO_RELEASES);
    else {
        size_mb(plan.bytes, size, sizeof(size));
        snprintf(line, sizeof(line), T_PLAN_LINE, plan.apps,
                 plan.apps == 1 ? "" : "s", size);
    }
    return line;
}

/* The row the tab itself sits on. Two lines rather than one: at this width
   the heading and the tally do not fit on a line together in the list's own
   face, and stacked they read as a heading with its tally under it, which is
   what they are. Where a package would have its icon, the tab's own sign. */
static void draw_action_row(int y, int selected, float t) {
    int updates = view_tab_kind() == VIEW_TAB_STICK;
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
    static const signed char SIGN[VIEW_SETTINGS] = {
        MARK_DOWNLOAD, MARK_BASKET, MARK_STICK, MARK_UPDATE, MARK_INFO,
    };
    float gx = LIST_X + ICON_W / 2.0f, gy = y + ITEM_H / 2.0f;
    enum mark m = (enum mark)SIGN[n];
    mark_draw(m, gx, gy, selected ? g_text : faded(g_dim, 170),
              selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
    font_print_clipped(FONT_BODY, NAME_X, y + 21, LIST_X + LIST_W - NAME_X,
                       selected ? g_text : g_dim, view_setting(n));
}

/* How long the cursor has sat on what it sits on: the scrolling of a line
   too long for its room starts over each time it moves. One clock, keyed
   by the list and the row. */
static float g_now;
float hover_age(int list, int key) {
    static int last_list = -1, last_key = -1;
    static float since;
    if (list != last_list || key != last_key) {
        last_list = list;
        last_key = key;
        since = g_now;
    }
    return g_now - since;
}

static void draw_list(const struct catalog *catalog, int cursor, float t) {
    int count = view_count();
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
        int index = view_index(i);
        if (index >= 0) wanted[want_count++] = index;
    }
    icons_bind(catalog);
    if (icons_want(wanted, want_count)) preview_poke();

    for (int i = g_first; i < count && i < g_first + VISIBLE; i++) {
        int y = LIST_Y + (i - g_first) * ITEM_H;
        int selected = i == cursor;
        int index = view_index(i);
        if (index == VIEW_ROW_ACTION) { draw_action_row(y, selected, t); continue; }
        if (index <= VIEW_ROW_SETTING) {
            draw_setting_row(VIEW_ROW_SETTING - index, y, selected, t);
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
        if (view_basket_has(index)) {
            mark_draw(MARK_BASKET, mx, my, selected ? g_text : faded(g_dim, 170),
                      selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
            name_w -= 22;
        }

        font_print_scrolling(FONT_BODY, NAME_X, y + 21, name_w,
                             selected ? g_text : g_dim, entry->name,
                             selected ? hover_age(0, index) : 0.0f);
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
static void draw_action_panel(const struct catalog *catalog, float t) {
    struct view_plan plan;
    char value[48], size[24];
    int y = SHOT_Y + 14;

    view_action_plan(&plan);
    draw_shade(PANEL_X + SHOT_W / 2, y + 70, SHOT_W, 170);
    gfx_glow(PANEL_X + SHOT_W / 2, y + 60, SHOT_W + 90, 200,
             rgb_pack(g_tint, (int)(70 * update_pulse(t))));

    font_print(FONT_H1, PANEL_X, y, g_text, action_title());
    if (plan.apps <= 0 && view_tab_kind() == VIEW_TAB_STICK) {
        int lines = draw_wrapped(FONT_META, PANEL_X, y + 20, SHOT_W, 16, 2, g_dim, T_CHECK_NOTE);
        /* The row has two keys, and the panel names them the way the footer
           does, with the key's own mark rather than a word for it; what the
           two do is said under them. */
        float base = y + 20 + 16 * lines + 14;
        draw_hint(PANEL_X, base, MARK_CROSS, T_QUICK_CHECK, g_dim);
        draw_hint(PANEL_X, base + 18, MARK_SQUARE, T_FULL_CHECK, g_dim);
        draw_wrapped(FONT_META, PANEL_X, base + 40, SHOT_W, 16, 3, g_dim, T_CHECK_EXPLAIN);
        return;
    }
    if (plan.apps > 0) {
        size_mb(plan.bytes, size, sizeof(size));
        download_time(plan.bytes, value, sizeof(value));
        font_printf(FONT_META, PANEL_X, y + 20, g_dim, T_PLAN_SIZE,
                    size, value);
    } else {
        font_print(FONT_META, PANEL_X, y + 20, g_dim,
                   T_NO_RELEASES);
    }
    /* One line a package, in the order they would be fetched, for as many as
       the panel holds; the rest are counted rather than named. */
    int line = 0, room = (FOOTER_Y - 20 - (y + 40)) / 14;
    for (int row = 0; row < view_count(); row++) {
        int index = view_index(row);
        if (index < 0) continue;
        const struct app_entry *entry = &catalog->apps[index];
        if (!entry->has_release || !entry->release.size || entry->unsupported) continue;
        if (line >= room) {
            font_printf(FONT_META, PANEL_X, y + 40 + line * 14, g_dim,
                        T_AND_MORE, plan.apps - line);
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
                    T_PLAN_SKIPPED, plan.skipped);
}

/* The picture alone: the card with the still or the film on it and its
   reflection in the water. */
/* PIC1.PNG, 480 by 272, which is the size of the screen because that is what
   an EBOOT carries it for. It is not there while somebody is working the
   list: behind rows of lettering a busy picture only makes the reading
   harder. Left alone for twenty-five seconds it comes up behind everything, the way
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
        const char *note = picture == PREVIEW_LOADING ? T_CARD_LOADING
                         : picture == PREVIEW_MISSING ? T_CARD_NO_PICTURE : "";
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
    int in_basket = view_basket_has((int)(entry - g_catalog->apps));
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
            if (catalog_new_build(entry))
                snprintf(line, sizeof(line), T_PANEL_REBUILD, entry->remote_version, size);
            else
                snprintf(line, sizeof(line), T_PANEL_UPDATE, entry->remote_version, size);
            break;
        case APP_UNKNOWN:
            snprintf(line, sizeof(line), T_PANEL_INSTALLED, entry->local_version);
            break;
        case APP_CURRENT:
            snprintf(line, sizeof(line), T_PANEL_INSTALLED, entry->local_version);
            break;
        default:
            snprintf(line, sizeof(line), "%s%s",
                     entry->unsupported ? T_PANEL_UNSUPPORTED : size[0] ? size : T_PANEL_NO_RELEASE,
                     in_basket ? T_PANEL_IN_BASKET : "");
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

static void band_edge(int y) {
    unsigned bright = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.55f), 210);
    unsigned clear = rgb_pack(g_tint, 0);
    gfx_hgrad(0, y, SCR_W / 2, 1, clear, bright);
    gfx_hgrad(SCR_W / 2, y, SCR_W / 2, 1, bright, clear);
    /* The line is one pixel; what makes it read as light is under it. */
    gfx_glow(SCR_W / 2, y, 460, 9, rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.4f), 70));
}

/* A line of the same kind but shorter, for dividing a band's inside. */
void band_rule(int y, int half_w, int alpha) {
    unsigned faint = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.4f), alpha);
    unsigned clear = rgb_pack(g_tint, 0);
    gfx_hgrad(SCR_W / 2 - half_w, y, half_w, 1, clear, faint);
    gfx_hgrad(SCR_W / 2, y, half_w, 1, faint, clear);
}

void draw_band(int y, int h) {
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

float hint_width(enum mark m, const char *text) {
    return mark_width(m) + HINT_GAP + font_width(FONT_META, text);
}

/* The mark a shade brighter than the word: the button is what the eye
   looks for, the word is what it reads once it has found it. */
float draw_hint(float x, float base, enum mark m, const char *text,
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

/* text into lines no wider than width, broken at spaces, at most max of
   them; the last takes whatever is left. Returns how many. */
static int break_lines(enum font_style style, const char *text, float width,
                       char lines[][128], int max) {
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        size_t len = strlen(p), fit = len;
        if (n < max - 1) {
            while (fit > 0) {
                snprintf(lines[n], 128, "%.*s", (int)fit, p);
                if (font_width(style, lines[n]) <= width) break;
                size_t k = fit - 1;
                while (k > 0 && p[k] != ' ') k--;
                fit = k;
            }
            if (fit == 0) fit = len;
        }
        snprintf(lines[n], 128, "%.*s", (int)fit, p);
        p += fit;
        while (*p == ' ') p++;
        n++;
    }
    return n ? n : 1;
}

/* The question in the middle, its line under it wrapped and centred, the
   band grown by a row for every line past the first. */
static void draw_ask(void) {
    char lines[3][128];
    int n = break_lines(FONT_META, g_ask_line, SCR_W - 80, lines, 3);
    int h = BAND_H + 14 * (n - 1), y = (SCR_H - h) / 2;
    draw_band(y, h);
    float w = font_width(FONT_BODY, g_ask_title);
    font_print_clipped(FONT_BODY, SCR_W / 2 - w / 2, y + 44, SCR_W - 40,
                       g_text, g_ask_title);
    for (int i = 0; i < n; i++) {
        w = font_width(FONT_META, lines[i]);
        font_print(FONT_META, SCR_W / 2 - w / 2, y + 68 + 14 * i, g_dim, lines[i]);
    }
    draw_answers(y + h - 22, T_YES, T_NO);
}

/* ------------------------------------------------------------------ menu */

/* The system's own options panel: it slides in from the right edge over
   the dimmed room and stands there, the title at its head and the choices
   under it, one lit. It is the one place every key is named: a row that a
   key does directly carries that key's mark at its end. */
#define MENU_W 214

static void draw_menu(void) {
    /* The menu draws its last rows while sliding out, from the copy taken
       as it closed. */
    const struct menu *m = g_menu_leaving ? &g_menu_gone : g_menu;
    int count = m->count > MENU_MAX ? MENU_MAX : m->count;
    /* Eased both ways: a fifth of the way there each frame is a slide that
       lands without a bump, about a quarter of a second either way. */
    float goal = g_menu_leaving ? 0.0f : 1.0f;
    g_menu_slide += (goal - g_menu_slide) * 0.22f;
    if (g_menu_leaving && g_menu_slide < 0.02f) {
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
    font_print_clipped(FONT_BODY, left, 34, right - left, g_text, m->title);
    gfx_hgrad((int)left, 44, MENU_W - 34, 1, bright, clear);

    unsigned grey = rgb_pack(rgb_mix(NIGHT_BOTTOM, RGB_WHITE, 0.32f), 255);
    for (int i = 0; i < count; i++) {
        int y = 78 + i * 26;
        int on = i == m->cursor;
        if (on) {
            gfx_glow(x0 + MENU_W / 2.0f, y - 5, MENU_W + 60, 30, rgb_pack(g_tint, 120));
            gfx_glow(x0 + MENU_W / 2.0f, y + 5, MENU_W - 20, 8,
                     rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.7f), 150));
        }
        unsigned color = !m->on[i] ? grey : on ? g_text : g_dim;
        /* The key that does this without the menu, at the row's end and
           in its own colour rather than the row's: it is a note about the
           row, not part of it. */
        float room = right - left;
        if (m->key[i] >= 0) {
            enum mark k = (enum mark)m->key[i];
            mark_draw(k, right - mark_width(k) / 2.0f, y - 4, faded(g_dim, on ? 220 : 140),
                      MARK_PLAIN, 0, 0);
            room -= mark_width(k) + 10;
        }
        /* A row can carry a note after the byte 2 -- a source the last
           fetch could not load says so -- drawn at the row's end in the
           keys' colour, for the same reason: it is about the row. */
        const char *item = m->item[i];
        char plain[64];
        const char *note = strchr(item, '\x02');
        if (note) {
            float w = font_width(FONT_META, note + 1);
            font_print(FONT_META, left + room - w, y, faded(g_dim, on ? 220 : 140), note + 1);
            room -= w + 8;
            snprintf(plain, sizeof(plain), "%.*s", (int)(note - item), item);
            item = plain;
        }
        /* A row can name a thing by its mark: the byte 1 and then the mark
           plus one end the words, and the mark stands after them. */
        const char *glyph = strchr(item, '\x01');
        if (glyph && glyph[1]) {
            char words[32];
            snprintf(words, sizeof(words), "%.*s", (int)(glyph - item), item);
            float end = font_print_clipped(FONT_BODY, left, y, room, color, words);
            enum mark gm = (enum mark)(glyph[1] - 1);
            mark_draw(gm, end + 6 + mark_width(gm) / 2.0f, y - 4, color, MARK_PLAIN, 0, 0);
        } else {
            font_print_scrolling(FONT_BODY, left, y, room, color, item,
                                 on ? hover_age(2, i) : 0.0f);
        }
    }
    /* Enter and back, the way every band ends, at the panel's foot. */
    float hx = left;
    hx = draw_hint(hx, SCR_H - 14, MARK_CROSS, T_HINT_ENTER, g_dim);
    draw_hint(hx, SCR_H - 14, MARK_CIRCLE, T_HINT_BACK, g_dim);
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
        float hw = hint_width(MARK_CIRCLE, T_HINT_CANCEL);
        draw_hint(SCR_W / 2 - hw / 2, BAND_Y + BAND_H - 18, MARK_CIRCLE, T_HINT_CANCEL, g_dim);
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
    if (!size) return;
    const char *value = *cut ? cut : name;
    size_t len = strlen(value);
    if (len >= size) len = size - 1;
    memcpy(out, value, len);
    out[len] = '\0';
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
        snprintf(g_storage, sizeof(g_storage), T_INFO_UNKNOWN);
        return;
    }
    unsigned long long unit = (unsigned long long)info.sector_count * info.sector_size;
    unsigned long long left = info.free_clusters * unit;
    unsigned long long all = info.max_clusters * unit;
    size_words(left, g_storage, sizeof(g_storage));
    if (all) g_storage_used = 1.0f - (float)((double)left / (double)all);
}

/* What each row does, said on the right while the cursor is on it: the
   list names the thing, the panel says what it comes to. */
static const char *const SETTING_NOTE[VIEW_SETTINGS] = {
    T_NOTE_SOURCES,
    T_NOTE_DIRECT,
    T_NOTE_FILES,
    T_NOTE_RESET,
    T_NOTE_INFO,
};

/* Four rows at the foot leave the facts above them 18 pixels apart rather
   than 24, which the small face reads at without touching. */
#define ACTION_Y (INFO_Y + 148)
#define ACTION_H 18

static void draw_info(void) {
    draw_band(INFO_Y, INFO_H);

    struct https_info tls;
    https_get_last_info(&tls);
    char value[96];

    /* Which build this is, above the rest: the one fact the band states about
       itself rather than about the run. */
    fact(INFO_Y + 18, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30,
         T_INFO_PSPDX, PSPDX_VERSION);

    url_host(catalog_url(), value, sizeof(value));
    fact(INFO_Y + 36, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30,
         T_INFO_CATALOG, value);

    if (tls.cipher[0]) {
        char cipher[48];
        tidy_cipher(tls.cipher, cipher, sizeof(cipher));
        snprintf(value, sizeof(value), T_INFO_TLS, cipher, tls.group);
    } else {
        snprintf(value, sizeof(value), T_INFO_NOT_CONNECTED);
    }
    fact(INFO_Y + 54, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30,
         T_INFO_CONNECTION, value);

    snprintf(value, sizeof(value), "%u ms", tls.handshake_ms);
    fact(INFO_Y + 76, FACT_LABEL, FACT_VALUE, 140, T_INFO_HANDSHAKE, value);

    snprintf(value, sizeof(value), "%d bits", entropy_get_bits());
    fact(INFO_Y + 96, FACT_LABEL, FACT_VALUE, 140, T_INFO_ENTROPY, value);

    /* Not a scheduler's number -- the PSP has none to ask. The share of each
       frame that goes into drawing it; the rest is the wait for vblank, which
       is the only idle this client has. */
    snprintf(value, sizeof(value), "%d fps, %d%% drawing",
             g_frame_us > 0.0f ? (int)(1000000.0f / g_frame_us + 0.5f) : 0,
             (int)(g_load * 100.0f + 0.5f));
    fact(INFO_Y + 116, FACT_LABEL, FACT_VALUE, 160, T_INFO_FRAMES, value);

    int installed = 0;
    if (g_catalog)
        for (int i = 0; i < g_catalog->count; i++)
            if (g_catalog->apps[i].state != APP_NOT_INSTALLED) installed++;
    snprintf(value, sizeof(value), "%d", installed);
    fact(INFO_Y + 76, FACT_LABEL2, FACT_VALUE2, 110, T_INFO_INSTALLED, value);

    snprintf(value, sizeof(value), "%u KB",
             (unsigned)sceKernelTotalFreeMemSize() / 1024);
    fact(INFO_Y + 96, FACT_LABEL2, FACT_VALUE2, 110, T_INFO_MEMORY, value);

    /* Room on the stick is the one fact here that is a proportion, so it is
       drawn as one: the line fills as the stick does, and what is left of it
       is what a package has to fit into. */
    fact(INFO_Y + 116, FACT_LABEL2, FACT_VALUE2, 110, T_INFO_STICK, g_storage);
    if (g_storage_used >= 0.0f) {
        int x = FACT_VALUE2, w = SCR_W - FACT_VALUE2 - 30, y = INFO_Y + 125;
        int used = (int)(w * g_storage_used + 0.5f);
        gfx_rect(x, y, w, 3, RGBA(255, 255, 255, 28));
        if (used > 0) gfx_hgrad(x, y, used, 3, rgb_pack(g_tint, 255), g_accent);
    }

    band_rule(INFO_Y + 134, 160, 120);
    /* The seed is renewed from here, beside the entropy it reports: not a
       setting, a fact with one thing to do about it. */
    float w = hint_width(MARK_SQUARE, T_SUB_SWEEP) + 24 + hint_width(MARK_CIRCLE, T_HINT_BACK);
    float hx = draw_hint(SCR_W / 2 - w / 2, INFO_Y + 156, MARK_SQUARE, T_SUB_SWEEP, g_dim);
    draw_hint(hx + 24, INFO_Y + 156, MARK_CIRCLE, T_HINT_BACK, g_dim);
}

/* --------------------------------------------------------------- details */

/* Prints text over at most `lines` lines of `width`, breaking at spaces,
   the last line clipped. Returns the lines used. */
int draw_wrapped(enum font_style style, float x, float y, float width,
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

/* Where the band's scrolling part stands: under the name's rule, down to
   the band's foot, facts from DETAIL_Y and the text DETAIL_TEXT_Y below
   them, a line every DETAIL_STEP, the last of them no lower than
   DETAIL_LAST once scrolled to the end. */
#define DETAIL_TOP (INFO_Y + 38)
#define DETAIL_BOTTOM (INFO_Y + INFO_H)
#define DETAIL_Y (INFO_Y + 56)
#define DETAIL_TEXT_Y 134
#define DETAIL_STEP 16
#define DETAIL_LAST (DETAIL_BOTTOM - 4)
#define DETAIL_X 40
/* The stick rests a little off centre on most PSPs: under this nothing
   moves. At a full push the text moves this many pixels a frame. */
#define DETAIL_DEAD 0.2f
#define DETAIL_SPEED 5.0f

/* A small triangle, pointing up or down, of four rows of pixels: the font
   has no arrow, and a mark would say more than "there is more". */
static void draw_more(int cx, int y, int up, unsigned color) {
    for (int i = 0; i < 4; i++)
        gfx_rect(cx - i, up ? y + i : y + 3 - i, 2 * i + 1, 1, color);
}

/* Everything the catalog says about the one package, in the band the rest
   of the questions are asked in: the card is for looking, this is for
   reading. */
static void draw_details(void) {
    const struct app_entry *e = g_details;
    /* Room for two versions of 64 characters: the row is cut to its width
       when it is drawn, not here in the middle of a letter. */
    char value[2 * VERSION_SIZE + 32], size[24];
    draw_band(INFO_Y, INFO_H);

    float w = font_width(FONT_BODY, e->name);
    font_print_clipped(FONT_BODY, SCR_W / 2 - w / 2, INFO_Y + 24, SCR_W - 40,
                       g_text, e->name);
    band_rule(INFO_Y + 34, 150, 110);

    /* Under the name everything moves as one when the stick scrolls, and
       stays inside the band while it does. */
    gfx_clip(0, DETAIL_TOP, SCR_W, DETAIL_BOTTOM - DETAIL_TOP);
    int y = DETAIL_Y - (int)g_detail_scroll;
    if (catalog_new_build(e))
        snprintf(value, sizeof(value), T_DETAIL_REBUILD, e->local_version);
    else if (e->state == APP_UPDATE)
        snprintf(value, sizeof(value), T_DETAIL_UPDATE,
                 e->local_version, e->remote_version);
    else if (e->state != APP_NOT_INSTALLED)
        snprintf(value, sizeof(value), T_DETAIL_INSTALLED, e->local_version);
    else if (e->has_release)
        snprintf(value, sizeof(value), "%s", e->release.version);
    else
        snprintf(value, sizeof(value), T_UNKNOWN);
    fact(y, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30, T_DETAIL_VERSION, value);
    fact(y + 20, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30, T_DETAIL_AUTHOR, e->author);
    fact(y + 40, FACT_LABEL, FACT_VALUE, 140, T_DETAIL_LICENSE, e->license);
    /* The tags on one line, a comma between them: eight of 24 characters of
       up to four bytes each and seven separators of two, so all of them fit
       and the row is cut to its width where it is drawn, between letters.
       Were one ever to overflow, no half of a letter is left at the end. */
    char tags[PSPDX_TAGS * 24 * 4 + (PSPDX_TAGS - 1) * 2 + 1];
    size_t t = 0;
    for (const char *p = e->tags; *p; p++) {
        if (*p == '\n' && t + 2 < sizeof(tags)) {
            tags[t++] = ',';
            tags[t++] = ' ';
        } else if (*p != '\n' && t + 1 < sizeof(tags)) {
            tags[t++] = *p;
        } else {
            break;
        }
    }
    tags[t] = '\0';
    pspdx_utf8_mend(tags);
    /* An app with no tags has no row for them, rather than a label alone. */
    if (tags[0])
        fact(y + 40, FACT_LABEL2, FACT_VALUE2, 110, T_DETAIL_TAGS, tags);
    if (e->has_release && e->release.size) {
        size_mb(e->release.size, size, sizeof(size));
        fact(y + 60, FACT_LABEL, FACT_VALUE, 140, T_DETAIL_SIZE, size);
    }
    fact(y + 80, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30, T_DETAIL_ID, e->id);

    if(e->release.checked_at){
        time_t checked=e->release.checked_at;struct tm *date=gmtime(&checked);
        char when[24]=T_UNKNOWN;if(date)strftime(when,sizeof(when),"%Y-%m-%d %H:%M UTC",date);
        snprintf(value,sizeof(value),"%s%s",e->fresh?"":T_DETAIL_SAVED,when);
    }else snprintf(value,sizeof(value),"%s",e->fresh?T_DETAIL_THIS_SESSION:T_UNKNOWN);
    fact(y+100,FACT_LABEL,FACT_VALUE,SCR_W-FACT_VALUE-30,T_DETAIL_CHECKED,value);
    band_rule(y + 116, 160, 120);
    /* The lines made when the band opened, only the ones in view. */
    char line[DETAIL_LINE_BYTES + 1];
    for (int i = 0; i < g_detail_count; i++) {
        int base = y + DETAIL_TEXT_Y + i * DETAIL_STEP;
        if (base + 4 < DETAIL_TOP)
            continue;
        if (base - DETAIL_STEP > DETAIL_BOTTOM)
            break;
        /* wrap_text keeps a line within the bytes it was given; the buffer
           does not take that on trust. A line cut here may end inside a
           letter, which the font leaves out rather than reads past. */
        size_t len = g_detail_lines[i].len;
        if (len > DETAIL_LINE_BYTES)
            len = DETAIL_LINE_BYTES;
        memcpy(line, g_detail_text + g_detail_lines[i].start, len);
        line[len] = '\0';
        font_print(FONT_META, DETAIL_X, base, g_dim, line);
    }
    gfx_unclip();
    /* More above, more below: a small point at the band's edge, quiet
       enough to be found only by an eye looking for it. */
    unsigned more = faded(g_dim, 200);
    if (g_detail_scroll >= 1.0f)
        draw_more(SCR_W - 24, DETAIL_TOP + 4, 1, more);
    if (g_detail_scroll + 1.0f <= g_detail_max)
        draw_more(SCR_W - 24, DETAIL_BOTTOM - 8, 0, more);
}

/* How wide a line of the band's text is: the measure wrap_text asks, which
   never hands over more than a line's bytes. */
static float detail_width(void *ctx, const char *text, size_t len) {
    char line[DETAIL_LINE_BYTES + 1];
    (void)ctx;
    if (len > DETAIL_LINE_BYTES)
        len = DETAIL_LINE_BYTES;
    memcpy(line, text, len);
    line[len] = '\0';
    return font_width(FONT_META, line);
}

void shell_details(const struct app_entry *entry) {
    g_details = entry;
    if (!entry)
        return;
    /* The summary, then the description a blank line below it, either one
       alone when the other is missing; broken into lines here, once. */
    const char *about = entry->description ? entry->description : "";
    snprintf(g_detail_text, sizeof(g_detail_text), "%s%s%s", entry->summary,
             entry->summary[0] && about[0] ? "\n\n" : "", about);
    pspdx_utf8_mend(g_detail_text);
    g_detail_count = wrap_text(g_detail_text, SCR_W - 2 * DETAIL_X, DETAIL_LINE_BYTES,
                               detail_width, NULL, g_detail_lines, DETAIL_LINES);
    g_detail_scroll = 0.0f;
    /* Scrolled as far as the last line standing on the band's last baseline. */
    int last = DETAIL_Y + DETAIL_TEXT_Y + (g_detail_count - 1) * DETAIL_STEP;
    g_detail_max = g_detail_count && last > DETAIL_LAST ? (float)(last - DETAIL_LAST) : 0.0f;
}

void shell_details_scroll(float push) {
    float away = push < 0.0f ? -push : push;
    if (!g_details || away < DETAIL_DEAD)
        return;
    /* Past the dead zone the speed rises with the square of the push: a
       little is a line at reading pace, all the way is a page a second. */
    float t = (away - DETAIL_DEAD) / (1.0f - DETAIL_DEAD);
    if (t > 1.0f)
        t = 1.0f;
    g_detail_scroll += (push < 0.0f ? -1.0f : 1.0f) * t * t * DETAIL_SPEED;
    if (g_detail_scroll > g_detail_max)
        g_detail_scroll = g_detail_max;
    if (g_detail_scroll < 0.0f)
        g_detail_scroll = 0.0f;
}

/* ---------------------------------------------------------------- footer */

static void draw_footer(void) {
    /* The system's own screens carry no legend: the keys are the keys, and
       a strip that names them names nothing. What the strip is for is the
       one line that is not a legend -- what is being waited for -- and the
       way back out of a band that fills the screen. Otherwise there is no
       strip, and the water runs to the edge. */
    if (g_ask_title[0] || g_menu || g_menu_leaving || g_installing) return;
    if (!g_details && !g_status[0]) return;
    /* No edge: the strip comes in as a shadow rising from the bottom, the
       way the PSP's own bars sit on their backgrounds. */
    gfx_vgrad(0, FOOTER_Y - 28, SCR_W, 28, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 120));
    gfx_vgrad(0, FOOTER_Y, SCR_W, SCR_H - FOOTER_Y, RGBA(0, 0, 0, 120),
              RGBA(0, 0, 0, 200));
    if (g_details) {
        draw_hint(LIST_X, FOOTER_BASE, MARK_CIRCLE, T_HINT_BACK, g_dim);
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
    unsigned ge, vblank, list;
    gfx_frame_worst(&ge, &vblank, &list);
    snprintf(out, size, "slowest draw %u us: back %u, front %u, end %u (ge %u, vblank %u), list %u KB",
             g_worst_total, g_worst_back, g_worst_front, g_worst_end, ge, vblank, list / 1024);
    g_worst_total = g_worst_back = g_worst_front = g_worst_end = 0;
}

/* The right half while the cursor is on a row under the gear: the row's
   name at the size the panel gives a package, and under it what taking the
   row comes to. */
static void draw_setting_panel(int n) {
    int y = SHOT_Y + 14;
    draw_shade(PANEL_X + SHOT_W / 2, y + 30, SHOT_W, 90);
    font_print(FONT_H1, PANEL_X, y, g_text, view_setting(n));
    draw_wrapped(FONT_META, PANEL_X, y + 24, SHOT_W, 16, 3, g_dim, SETTING_NOTE[n]);
}

void shell_draw(const struct catalog *catalog, int cursor) {
    follow_view();
    g_catalog = catalog;
    g_cursor = cursor;
    float t = gfx_frames() * (1.0f / 60.0f);
    g_now = t;
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
    if (files_view_shown()) {
        files_view_draw(t);
    } else if (catalog->count > 0 && view_count() > 0) {
        int rows = view_count();
        int index = view_index(cursor < rows ? cursor : 0);
        draw_list(catalog, cursor, t);
        if (index == VIEW_ROW_ACTION) draw_action_panel(catalog, t);
        else if (index <= VIEW_ROW_SETTING) draw_setting_panel(VIEW_ROW_SETTING - index);
        else if (index >= 0) draw_panel(&catalog->apps[index], t);
    } else if (g_status[0]) {
        /* Nothing to browse yet: the word stands in the room, lit from
           behind; what it is waiting for is said in the strip below. */
        title_draw(SCR_W / 2.0f, 116.0f, t, g_tint);
    } else {
        font_print(FONT_BODY, LIST_X, 120, g_dim, T_CATALOG_EMPTY);
    }
    if (g_info) draw_info();
    if (g_details) draw_details();
    if (g_menu || g_menu_leaving) draw_menu();
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
    follow_view();
    if (files_view_sync()) return;
    int index = view_index(cursor);
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
    preview_tick();
}

void shell_reshow_card(void) { g_last_cursor = -1; }

void shell_word(const char *word) {
    snprintf(g_word, sizeof(g_word), "%s", word ? word : T_WORD_CONNECTING);
}

void shell_status(const char *text) {
    snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
    pspdx_utf8_mend(g_status);
}

void shell_ask(const char *title, const char *line) {
    snprintf(g_ask_title, sizeof(g_ask_title), "%s", title ? title : "");
    snprintf(g_ask_line, sizeof(g_ask_line), "%s", line ? line : "");
    /* The last install's result has been overtaken by a new question. */
    if (g_ask_title[0]) g_status[0] = '\0';
}

void shell_menu(const struct menu *menu) {
    /* Closing is a slide out, so the rows stay until the panel is off the
       edge: a copy of the menu as it closed, g_menu_leaving saying it is
       on its way out. The cursor went with the close, as it always has, so
       the rows slide out with the first of them lit. */
    if (!menu) {
        if (g_menu) {
            g_menu_gone = *g_menu;
            g_menu_gone.cursor = 0;
            g_menu_leaving = 1;
        }
        g_menu = NULL;
        return;
    }
    if (!g_menu || g_menu_leaving) g_menu_slide = 0.0f;
    g_menu_leaving = 0;
    g_menu = menu;
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
    pspdx_utf8_mend(g_status);
    logline("%s", g_status);
    lattice_touch(0.5f);
}
