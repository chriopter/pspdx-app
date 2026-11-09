#include "text.h"
/*
 * The library, drawn: a grid as wide as the header, its edges where the
 * header's word starts and the gear ends, of the places session/library.c
 * lays out -- the favourites, then each section from its sign on a new
 * line -- sliding to keep the chosen line in the room. Nothing here reads the stick: the items are the library's, the
 * icons the icon thread's.
 */

#include <math.h>
#include <stdio.h>

#include "gui/icons.h"
#include "gui/library_view.h"
#include "gui/marks.h"
#include "gui/preview.h"
#include "gui/shell_internal.h"
#include "session/library.h"

/* The UMD grid's measures, so the two look alike: four to a line from the
   header's word to the gear, three lines whole between the store's top and
   the name -- the icons a little wider than their own 9:5 for it. */
#define GRID_GAP 8
#define GRID_H 54
#define GRID_W ((SCR_W - 2 * LIST_X - (LIBRARY_COLS - 1) * GRID_GAP) / LIBRARY_COLS)
#define GRID_POP 16
/* The header's edges: the first column under its word, the last ending
   under the gear. */
#define GRID_LEFT LIST_X
#define GRID_STEP (GRID_W + GRID_GAP)
#define GRID_LINE (GRID_H + GRID_GAP)
#define GRID_TOP (LIST_Y - 2)           /* the store's breath under the header */
#define ROOM_H (3 * GRID_LINE - GRID_GAP) /* three lines whole */
#define NAME_Y (GRID_TOP + ROOM_H + GRID_POP / 2 + 13)

/* Each section's sign, in its first place. */
static const signed char SECTION_SIGN[LIBRARY_SECTIONS] = { MARK_UMD, MARK_GAMES, MARK_APPS };
static const char *const SECTION_WORD[LIBRARY_SECTIONS] = { T_LIBRARY_PSP, T_LIBRARY_PS1, T_LIBRARY_APPS };

/* Where each line stands, from the grid's top. */
static float g_scroll, g_pop;
static int g_last = -1;

static void centred(enum font_style style, float y, unsigned color, const char *text) {
    font_print(style, SCR_W / 2.0f - font_width(style, text) / 2.0f, y, color, text);
}

/* The list: one place a row, the search's, a section's sign and word, or
   an item with its icon at the store's row size; the chosen one's film or
   icon large on the right, as the store shows a package's picture. */
static void draw_rows(int cursor, float t) {
    static float top;
    int places = library_places();
    float want = top;
    if (cursor < want) want = cursor;
    if (cursor >= want + VISIBLE) want = cursor - VISIBLE + 1;
    top = want;
    gfx_clip(0, LIST_Y, SCR_W, VISIBLE * ITEM_H);
    for (int p = (int)top; p < places && p < (int)top + VISIBLE; p++) {
        int y = LIST_Y + (p - (int)top) * ITEM_H, selected = p == cursor;
        unsigned color = selected ? g_text : g_dim;
        if (selected) gfx_rect(LIST_X - 6, y, LIST_W + 12, ITEM_H, RGBA(255, 255, 255, 22));
        float gx = LIST_X + ICON_W / 2.0f, gy = y + ITEM_H / 2.0f;
        int w = LIST_X + LIST_W - NAME_X;
        if (library_place_search(p)) {
            mark_draw(MARK_SEARCH, gx, gy, color, selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
            char word[16 + LIBRARY_TITLE];
            if (library_searched()) snprintf(word, sizeof(word), T_SEARCH_HEAD, library_searched());
            else snprintf(word, sizeof(word), "%s", T_SEARCH);
            font_print_clipped(FONT_TITLE, NAME_X, y + 21, w, color, word);
            continue;
        }
        int s = library_place_sign(p), fav = library_place_favourites(p);
        if (s >= 0 || fav) {
            /* A row that opens its games: the sign, the word, and how many
               at the row's end, as the store's ways of browsing stand. */
            mark_draw(fav ? MARK_TICK : (enum mark)SECTION_SIGN[s], gx, gy, color,
                      selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
            char count[16];
            snprintf(count, sizeof(count), "%d", fav ? library_favourites_count() : library_section_count(s));
            float cw = font_width(FONT_META, count);
            font_print(FONT_META, LIST_X + LIST_W - cw, y + 21, faded(g_dim, selected ? 230 : 150), count);
            font_print_clipped(FONT_TITLE, NAME_X, y + 21, w - cw - 8, color,
                               fav ? T_LIBRARY_FAVOURITES : SECTION_WORD[s]);
            continue;
        }
        const struct library_item *item = library_item(library_place_item(p));
        if (!item) continue;
        const struct gfx_texture *icon = icons_library_get(item->key);
        if (icon) gfx_texture_draw(icon, LIST_X, y + 4, ICON_W, ITEM_H - 8, selected ? RGB(255, 255, 255) : RGB(170, 170, 170));
        else gfx_rect(LIST_X, y + 4, ICON_W, ITEM_H - 8, RGBA(255, 255, 255, 12));
        font_print_scrolling(FONT_TITLE, NAME_X, y + 21, w, color, item->title,
                             selected ? hover_age(5, (int)item->key) : 0.0f);
    }
    gfx_unclip();
    const struct library_item *chosen = library_item(library_place_item(cursor));
    if (!chosen) return;
    int film_alpha = 0;
    const struct gfx_texture *film = preview_film(&film_alpha);
    const struct gfx_texture *icon = icons_library_get(chosen->key);
    int h = SHOT_W * 5 / 9;
    if (film) gfx_texture_draw(film, PANEL_X, SHOT_Y + 8, SHOT_W, h, RGB(255, 255, 255));
    else if (icon) gfx_texture_draw(icon, PANEL_X, SHOT_Y + 8, SHOT_W, h, RGB(255, 255, 255));
    draw_wrapped(FONT_TITLE, PANEL_X, SHOT_Y + 8 + h + 24, SHOT_W, 18, 3, g_text, chosen->title);
}

void library_view_draw(int cursor, float t) {
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
    int places = library_places();
    if (cursor < 0) cursor = 0;
    if (cursor >= places) cursor = places - 1;
    int line = library_line(cursor), lines = library_lines();
    int height = lines * GRID_LINE - GRID_GAP;
    const struct library_item *chosen = library_item(library_place_item(cursor));

    /* The chosen line in the room. */
    float top = line * GRID_LINE, bottom = top + GRID_H;
    static float want;
    if (top < want) want = top;
    if (bottom > want + ROOM_H) want = bottom - ROOM_H;
    if (want > height - ROOM_H) want = height - ROOM_H;
    if (want < 0) want = 0;
    g_scroll += (want - g_scroll) * 0.25f;
    if (cursor != g_last) g_pop = 0;
    g_last = cursor;
    g_pop += (1.0f - g_pop) * 0.25f;

    /* The icons wanted: the chosen one first, the rest of what the room
       shows, then a line past either edge. */
    const struct library_item *wanted[32];
    int wants = 0;
    if (chosen) wanted[wants++] = chosen;
    for (int p = 0; p < places && wants < 32; p++) {
        const struct library_item *item = library_item(library_place_item(p));
        if (p == cursor || !item) continue;
        float y = library_line(p) * GRID_LINE - want;
        if (y > -GRID_LINE - GRID_H && y < ROOM_H + GRID_LINE) wanted[wants++] = item;
    }
    icons_library_want(wanted, wants);
    /* The chosen one's film and sound once the cursor rests on it, as the
       XMB plays a game's over its icon; the request is the same one every
       frame and only goes out when it changes. */
    if (chosen) preview_show_file(chosen->path, PREVIEW_FILE_TITLE);
    if (library_list()) {
        draw_rows(cursor, t);
        return;
    }

    /* The places: the chosen one last, grown over its neighbours, the rest
       cut to the room. A sign stands on a tile of its own, lit while the
       cursor is on it or in its section. */
    for (int pass = 0; pass < 2; pass++) {
        gfx_clip(0, pass ? GRID_TOP - GRID_POP / 2 : GRID_TOP, SCR_W, ROOM_H + (pass ? GRID_POP : 0));
        for (int p = 0; p < places; p++) {
            int selected = p == cursor;
            if (selected != pass) continue;
            int y = GRID_TOP + (int)floorf(library_line(p) * GRID_LINE - g_scroll + 0.5f);
            if (y < GRID_TOP - GRID_LINE || y > GRID_TOP + ROOM_H) continue;
            int x = GRID_LEFT + library_column(p) * GRID_STEP, w = GRID_W, h = GRID_H;
            int s = library_place_sign(p);
            if (s >= 0) {
                /* A tile like the others, the sign on it, lit while the
                   cursor is in its section: where that begins. */
                int here = selected || (chosen && chosen->section == s && !library_favourite(chosen->key));
                gfx_rect(x, y, w, h, RGBA(255, 255, 255, selected ? 34 : 14));
                mark_draw((enum mark)SECTION_SIGN[s], x + w / 2.0f, y + h / 2.0f,
                          here ? g_text : faded(g_dim, 190), here ? MARK_LIT : MARK_PLAIN,
                          rgb_pack(g_tint, 255), t);
                continue;
            }
            const struct library_item *item = library_item(library_place_item(p));
            if (!item) continue;
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
       when there are more than the room shows; on the search's row, the
       word in force. */
    char search[16 + LIBRARY_TITLE];
    const char *name = chosen ? chosen->title : NULL;
    if (library_place_search(cursor)) {
        if (library_searched()) snprintf(search, sizeof(search), T_SEARCH_HEAD, library_searched());
        else snprintf(search, sizeof(search), "%s", T_SEARCH);
        name = search;
    }
    if (name) {
        float w = font_width(FONT_TITLE, name), room = SCR_W - 2 * LIST_X;
        font_print_scrolling(FONT_TITLE, LIST_X + (w < room ? (int)((room - w) / 2) : 0), NAME_Y, room,
                             g_text, name, hover_age(5, chosen ? (int)chosen->key : -1));
    }
    if (height > ROOM_H && chosen) {
        /* Where it is among the items shown, the signs not counted. */
        int at = 0, shown = 0;
        for (int p = 0; p < places; p++) {
            int item = library_place_item(p) >= 0;
            at += item && p < cursor;
            shown += item;
        }
        char where[32];
        snprintf(where, sizeof(where), "%d / %d", at + 1, shown);
        font_print(FONT_CAPTION, SCR_W - LIST_X - font_width(FONT_CAPTION, where), NAME_Y,
                   faded(g_dim, 170), where);
    }
}
