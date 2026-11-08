#include "text.h"
/*
 * The library, drawn: the store's UMD grid with a word over each section.
 * A line holds one section's icons only, so every section starts a line of
 * its own, under its word and how many it holds; the lines and the words
 * stand at heights worked out once a list, and the grid slides to keep the
 * chosen line -- with its section's word, on a section's first line -- in
 * the room. Nothing here reads the stick: the items are the library's, the
 * icons the icon thread's.
 */

#include <math.h>
#include <stdio.h>

#include "gui/icons.h"
#include "gui/library_view.h"
#include "gui/preview.h"
#include "gui/shell_internal.h"
#include "session/library.h"

/* The UMD grid's measures, so the two look alike. */
#define GRID_W 72
#define GRID_H 40
#define GRID_POP 18
#define GRID_GAP 8
#define GRID_LEFT ((SCR_W - LIBRARY_COLS * GRID_W - (LIBRARY_COLS - 1) * GRID_GAP) / 2)
#define GRID_LINE (GRID_H + GRID_GAP)
#define GRID_TOP (32 + 10)              /* under the header */
#define ROOM_H (4 * GRID_LINE - GRID_GAP)
#define NAME_Y (GRID_TOP + ROOM_H + GRID_POP / 2 + 12)
#define LABEL_H 18                      /* a section's word, over its first line */

static const char *const SECTION[LIBRARY_SECTIONS] = { T_LIBRARY_PSP, T_LIBRARY_PS1, T_LIBRARY_APPS };

/* Where each line and each section's word stands, from the grid's top, for
   the list as last taken over. */
#define LINES_MAX (LIBRARY_MAX / LIBRARY_COLS + LIBRARY_SECTIONS)
static int g_line_y[LINES_MAX], g_label_y[LIBRARY_SECTIONS], g_height;
static const struct library_item *g_laid_out;
static int g_laid_count = -1;
static float g_scroll, g_pop;
static int g_last = -1;

static void lay_out(void) {
    const struct library_item *first = library_item(0);
    if (first == g_laid_out && library_count() == g_laid_count) return;
    g_laid_out = first;
    g_laid_count = library_count();
    int y = 0, line = 0;
    for (int s = 0; s < LIBRARY_SECTIONS; s++) {
        int lines = (library_section_count(s) + LIBRARY_COLS - 1) / LIBRARY_COLS;
        g_label_y[s] = y;
        if (!lines) continue;
        y += LABEL_H;
        for (int k = 0; k < lines && line < LINES_MAX; k++, line++) {
            g_line_y[line] = y;
            y += GRID_LINE;
        }
    }
    g_height = y - GRID_GAP;
}

static void centred(enum font_style style, float y, unsigned color, const char *text) {
    font_print(style, SCR_W / 2.0f - font_width(style, text) / 2.0f, y, color, text);
}

void library_view_draw(int cursor, float t) {
    (void)t;
    int count = library_count();
    if (count <= 0) {
        float mid = (32 + SCR_H) / 2.0f;
        if (!library_ready() || library_scanning()) {
            centred(FONT_META, mid, g_dim, T_LIBRARY_READING);
        } else {
            centred(FONT_TITLE, mid - 3, g_text, T_LIBRARY_NONE);
            centred(FONT_META, mid + 15, g_dim, T_LIBRARY_NONE_HINT);
        }
        return;
    }
    lay_out();
    if (cursor < 0) cursor = 0;
    if (cursor >= count) cursor = count - 1;
    int line = library_line(cursor), lines = library_lines();
    if (line >= LINES_MAX) line = LINES_MAX - 1;
    const struct library_item *chosen = library_item(cursor);
    int section = chosen ? chosen->section : 0;

    /* The chosen line in the room, and its section's word with it when it
       is the section's first. */
    float top = g_line_y[line] - (cursor - library_section_first(section) < LIBRARY_COLS ? LABEL_H : 0);
    float bottom = g_line_y[line] + GRID_H;
    static float want;
    if (top < want) want = top;
    if (bottom > want + ROOM_H) want = bottom - ROOM_H;
    if (want > g_height - ROOM_H) want = g_height - ROOM_H;
    if (want < 0) want = 0;
    g_scroll += (want - g_scroll) * 0.25f;
    if (cursor != g_last) g_pop = 0;
    g_last = cursor;
    g_pop += (1.0f - g_pop) * 0.25f;

    /* The icons wanted: the chosen one first, the rest of what the room
       shows, then a line past either edge. */
    const struct library_item *wanted[32];
    int wants = 0;
    wanted[wants++] = chosen;
    for (int n = 0; n < count && wants < 32; n++) {
        int l = library_line(n);
        if (n == cursor || l >= LINES_MAX) continue;
        float y = g_line_y[l] - want;
        if (y > -GRID_LINE - GRID_H && y < ROOM_H + GRID_LINE) wanted[wants++] = library_item(n);
    }
    icons_library_want(wanted, wants);
    /* The chosen one's film and sound once the cursor rests on it, as the
       XMB plays a game's over its icon; the request is the same one every
       frame and only goes out when it changes. */
    if (chosen) preview_show_file(chosen->path, PREVIEW_FILE_TITLE);

    /* The words, then the icons: the chosen one last, grown over its
       neighbours, the rest cut to the room. */
    gfx_clip(0, GRID_TOP, SCR_W, ROOM_H);
    for (int s = 0; s < LIBRARY_SECTIONS; s++) {
        if (!library_section_count(s)) continue;
        int y = GRID_TOP + (int)floorf(g_label_y[s] - g_scroll + 0.5f);
        if (y < GRID_TOP - LABEL_H || y > GRID_TOP + ROOM_H) continue;
        char head[48];
        snprintf(head, sizeof(head), "%d", library_section_count(s));
        float x = font_print(FONT_META, GRID_LEFT, y + 12, s == section ? g_text : g_dim, SECTION[s]);
        font_print(FONT_CAPTION, x + 8, y + 12, faded(g_dim, 150), head);
    }
    for (int pass = 0; pass < 2; pass++) {
        if (pass) gfx_clip(0, GRID_TOP - GRID_POP / 2, SCR_W, ROOM_H + GRID_POP);
        for (int n = 0; n < count; n++) {
            int selected = n == cursor;
            if (selected != pass) continue;
            int l = library_line(n);
            if (l >= LINES_MAX) continue;
            int y = GRID_TOP + (int)floorf(g_line_y[l] - g_scroll + 0.5f);
            if (y < GRID_TOP - GRID_LINE || y > GRID_TOP + ROOM_H) continue;
            const struct library_item *item = library_item(n);
            int col = (n - library_section_first(item->section)) % LIBRARY_COLS;
            int x = GRID_LEFT + col * (GRID_W + GRID_GAP), w = GRID_W, h = GRID_H;
            if (selected) {
                w = GRID_W + (int)(GRID_POP * g_pop + 0.5f) / 2 * 2;
                h = w * GRID_H / GRID_W / 2 * 2;
                x -= (w - GRID_W) / 2;
                y -= (h - GRID_H) / 2;
            }
            const struct gfx_texture *icon = icons_library_get(item->key);
            int film_alpha = 0;
            const struct gfx_texture *film = selected ? preview_film(&film_alpha) : NULL;
            if (film)
                gfx_texture_draw(film, x, y, w, h, RGB(255, 255, 255));
            else if (icon)
                gfx_texture_draw(icon, x, y, w, h, selected ? RGB(255, 255, 255) : RGB(170, 170, 170));
            else
                gfx_rect(x, y, w, h, RGBA(255, 255, 255, selected ? 24 : 12));
        }
    }
    gfx_unclip();

    /* Under the grid the chosen one's name, and where it is among them
       when there are more than the room shows. */
    if (chosen) {
        float w = font_width(FONT_TITLE, chosen->title), room = SCR_W - 2 * LIST_X;
        font_print_scrolling(FONT_TITLE, LIST_X + (w < room ? (int)((room - w) / 2) : 0), NAME_Y, room,
                             g_text, chosen->title, hover_age(5, (int)chosen->key));
    }
    if (g_height > ROOM_H && lines > 0) {
        char where[32];
        snprintf(where, sizeof(where), "%d / %d", cursor + 1, count);
        font_print(FONT_CAPTION, SCR_W - LIST_X - font_width(FONT_CAPTION, where), NAME_Y,
                   faded(g_dim, 170), where);
    }
}
