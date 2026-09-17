#include "text.h"
/*
 * Sources, drawn: the gear's two columns, with the sources in the
 * list and what the selected one is on the right. The rows and their words
 * are session/manage_sources.c's; the room and its chrome are shell.c's.
 */

#include <stdio.h>
#include <string.h>

#include "gui/shell_internal.h"
#include "gui/sources_view.h"

/* The rows are the gear's: one line each, what a source is and how its
   last fetch went stands on the right, not under the name twice. */
#define SRC_ROW_H ITEM_H
#define SRC_STEP 10
#define SRC_VISIBLE ((FOOTER_Y - 6 - LIST_Y - SRC_STEP) / SRC_ROW_H)

static const struct manage_sources *g_m;
static float g_sel_y = LIST_Y;
static int g_first;

void sources_view_set(const struct manage_sources *m) {
    /* Opened, not merely handed new rows: the list starts at the top with
       the bar already on its row. */
    if (m && !g_m) {
        g_first = 0;
        g_sel_y = LIST_Y + m->cursor * SRC_ROW_H;
    }
    g_m = m;
}

int sources_view_shown(void) { return g_m != NULL; }

/* A URL has no space to break at: the most of it that fits, cut after a
   slash where one is in the second half of the line, in at most three
   lines, the last clipped. Worked out once a URL, not once a frame. */
#define URL_LINES 3
static char g_url_of[SOURCE_URL];
static char g_url_line[URL_LINES][SOURCE_URL];
static int g_url_lines;

static void break_url(const char *url) {
    if (!strcmp(g_url_of, url) && g_url_lines) return;
    snprintf(g_url_of, sizeof(g_url_of), "%s", url);
    g_url_lines = 0;
    const char *p = url;
    char buf[SOURCE_URL];
    while (*p && g_url_lines < URL_LINES) {
        size_t len = strlen(p), fit = len;
        if (g_url_lines < URL_LINES - 1) {
            for (; fit > 1; fit--) {
                snprintf(buf, sizeof(buf), "%.*s", (int)fit, p);
                if (font_width(FONT_META, buf) <= SHOT_W) break;
            }
            if (fit < len) {
                size_t k = fit;
                while (k > fit / 2 && p[k - 1] != '/') k--;
                if (k > fit / 2) fit = k;
            }
        }
        snprintf(g_url_line[g_url_lines++], SOURCE_URL, "%.*s", (int)fit, p);
        p += fit;
    }
}


static void draw_row(const struct manage_row *r, int i, int y, int selected, float t) {
    float gx = LIST_X + ICON_W / 2.0f, gy = y + SRC_ROW_H / 2.0f;
    int w = LIST_X + LIST_W - NAME_X;
    if (r->kind != MANAGE_SOURCE) {
        /* The two jobs, one line each: a plus for a source to be added,
           the arrow onto the floor for one package to be fetched. */
        mark_draw(r->kind == MANAGE_ADD ? MARK_PLUS : MARK_DOWNLOAD, gx, gy,
                  selected ? g_text : faded(g_dim, 170),
                  selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
        font_print_clipped(FONT_TITLE, NAME_X, y + 21, w, selected ? g_text : g_dim, r->name);
        return;
    }
    /* A source's sign is what kind of source it is: a site out on the
       network, a catalog file, a text list, one repository. */
    enum mark sign = r->skind == SOURCE_REPO ? MARK_HOME
                   : r->skind == SOURCE_LIST ? MARK_LIST
                   : r->skind == SOURCE_CATALOG ? MARK_PAGE : MARK_GLOBE;
    mark_draw(sign, gx, gy, selected ? g_text : faded(g_dim, 170),
              selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
    font_print_scrolling(FONT_TITLE, NAME_X, y + 21, w, selected ? g_text : g_dim, r->name,
                         selected ? hover_age(3, i) : 0.0f);
}

void sources_view_draw(float t) {
    const struct manage_sources *m = g_m;
    int count = m->count, cursor = m->cursor;
    if (cursor < g_first) g_first = cursor;
    if (cursor >= g_first + SRC_VISIBLE) g_first = cursor - SRC_VISIBLE + 1;
    if (g_first < 0) g_first = 0;
    /* The sources stand a step under the two jobs, with a line of the
       room's light in the step, fading out to either side the way the
       bands' rules do: what stands under it is the user's list, and what
       stands over it is done to that list. */
    int jobs = 0;
    while (jobs < count && m->row[jobs].kind != MANAGE_SOURCE) jobs++;
    int step = jobs > 0 && jobs < count ? SRC_STEP : 0;
    float target = LIST_Y + (cursor - g_first) * SRC_ROW_H + (cursor >= jobs ? step : 0);
    g_sel_y += (target - g_sel_y) * 0.25f;

    draw_rows_light_of(count < SRC_VISIBLE ? count : SRC_VISIBLE, SRC_ROW_H, g_sel_y, t);
    for (int i = g_first; i < count && i < g_first + SRC_VISIBLE; i++) {
        int y = LIST_Y + (i - g_first) * SRC_ROW_H + (i >= jobs ? step : 0);
        if (i == jobs && step && i > g_first) {
            unsigned faint = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.5f), 150);
            unsigned clear = rgb_pack(g_tint, 0);
            int cx = LIST_X + LIST_W / 2, half = LIST_W / 2, ry = y - step / 2;
            gfx_hgrad(cx - half, ry, half, 1, clear, faint);
            gfx_hgrad(cx, ry, half, 1, faint, clear);
        }
        draw_row(&m->row[i], i, y, i == cursor, t);
    }

    /* The right column is the gear's: the row's name and what it is. Under
       a source, where it is and how the last fetch went, as facts. */
    const struct manage_row *r = &m->row[cursor];
    char note[256];
    manage_note(m, cursor, note, sizeof(note));
    if (r->kind == MANAGE_SOURCE)
        draw_shade(PANEL_X + SHOT_W / 2, SHOT_Y + 120, SHOT_W, 110);
    int lines = draw_setting_note(manage_title(m, cursor), note, hover_age(3, cursor));
    if (r->kind == MANAGE_SOURCE) {
        int y = SHOT_Y + 14 + 24 + 17 * lines + 6;
        break_url(manage_url(m, cursor));
        for (int i = 0; i < g_url_lines; i++, y += 17)
            font_print_clipped(FONT_META, PANEL_X, y, SHOT_W, g_text, g_url_line[i]);
        char facts[3][MANAGE_FACT];
        int n = manage_facts(m, cursor, facts, 3);
        for (int i = 0; i < n; i++, y += 17)
            font_print_clipped(FONT_META, PANEL_X, y + 4, SHOT_W, g_dim, facts[i]);
    }

    /* The keys at the foot, the way Data has them, unless the status
       line has something to say there or a question stands with its own. */
    if (shell_footer_free()) {
        float hx = draw_hint(LIST_X, FOOTER_BASE, MARK_CROSS,
                             r->kind == MANAGE_ADD ? T_HINT_ENTER : T_HINT_DELETE, g_dim);
        draw_hint(hx, FOOTER_BASE, MARK_CIRCLE, T_HINT_BACK, g_dim);
    }
}
