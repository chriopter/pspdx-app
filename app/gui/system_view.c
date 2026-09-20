#include "text.h"
/*
 * Options, drawn: the gear's two columns, with the rows of what this client
 * can be set to on the left and what the selected one comes to on the
 * right. The values are read live -- gfx's frame rate, the shell's two
 * switches -- so a change made anywhere shows here at once, SELECT's
 * session preview included.
 */

#include <stdio.h>

#include "gui/shell.h"
#include "session/options.h"
#include "session/downloads.h"
#include "gui/shell_internal.h"
#include "gui/system_view.h"

static int g_open, g_cursor;
static float g_sel_y = LIST_Y;

void system_view_open(void) {
    g_open = 1;
    g_cursor = 0;
    g_sel_y = LIST_Y;
}

void system_view_close(void) { g_open = 0; }
int system_view_shown(void) { return g_open; }
int system_view_cursor(void) { return g_cursor; }

void system_view_move(int by) {
    g_cursor = (g_cursor + by + SYS_COUNT) % SYS_COUNT;
}

static const char *const WORD[SYS_COUNT] = {
    T_SYS_BAKED, T_SYS_FPS, T_SYS_DEV, T_SYS_SWEEP, T_SYS_RESET_ALL,
};
static const char *const NOTE[SYS_COUNT] = {
    T_SYS_FRAME_RATE_NOTE, T_SYS_FPS_NOTE, T_SYS_DEV_NOTE, T_SYS_SWEEP_NOTE, T_SYS_RESET_NOTE,
};
/* The sign where a package has its icon: a switch for the three rows
   that are set one way or the other, the key for the seed, the arrow
   turning back for the way back. The switch is two marks, the pill and
   the knob, and the knob slides from one end to the other when the row
   is flipped, the way the system's own switches move. */
static const signed char SIGN[SYS_COUNT] = {
    MARK_PILL, MARK_PILL, MARK_PILL, MARK_KEY, MARK_RESTORE,
};
#define KNOB_TRAVEL 4.6f

static int row_on(int i) {
    if (i == SYS_FRAME_RATE) return !options_fps_requested();      /* right is 60 */
    if (i == SYS_SHOW_FPS) return shell_show_fps();
    if (i == SYS_FAKE_UPDATES) return shell_dev_updates();
    return 1;
}

static void draw_row(int i, int y, int selected, float t) {
    float gx = LIST_X + ICON_W / 2.0f, gy = y + ITEM_H / 2.0f;
    unsigned color = selected ? g_text : g_dim;
    int w = LIST_X + LIST_W - NAME_X;
    mark_draw((enum mark)SIGN[i], gx, gy, color,
              selected ? MARK_LIT : MARK_PLAIN, rgb_pack(g_tint, 255), t);
    if (SIGN[i] == MARK_PILL) {
        /* The knob eases to its end: near half the way a frame, so a flip
           is a slide the eye follows and not a jump. */
        static float at[SYS_COUNT];
        float want = row_on(i) ? KNOB_TRAVEL : -KNOB_TRAVEL;
        at[i] += (want - at[i]) * 0.45f;
        mark_draw(MARK_KNOB, gx + at[i], gy, color, MARK_PLAIN, 0, t);
    }
    /* A value row says what it is set to at the row's end, in the row's
       own colour: the value is the row's. */
    const char *value = NULL;
    if (i == SYS_FRAME_RATE) value = options_fps_requested() ? T_VALUE_FPS30 : T_VALUE_FPS60;
    if (value) {
        float vw = font_width(FONT_TITLE, value);
        font_print(FONT_TITLE, LIST_X + LIST_W - vw, y + 21, color, value);
        w -= vw + 10;
    }
    /* The first row is named by the side it is on: Baked at 30, Mercy at 60. */
    const char *word = i == SYS_FRAME_RATE ? (options_fps_requested() ? T_SYS_BAKED : T_SYS_MERCY) : WORD[i];
    font_print_clipped(FONT_TITLE, NAME_X, y + 21, w, color, word);
}

void system_view_draw(float t) {
    float target = LIST_Y + g_cursor * ITEM_H;
    g_sel_y += (target - g_sel_y) * 0.25f;
    draw_rows_light(SYS_COUNT, g_sel_y, t);
    for (int i = 0; i < SYS_COUNT; i++)
        draw_row(i, LIST_Y + i * ITEM_H, i == g_cursor, t);

    draw_setting_note(g_cursor == SYS_FRAME_RATE ? (options_fps_requested() ? T_SYS_BAKED : T_SYS_MERCY)
                      : WORD[g_cursor], g_cursor == SYS_FRAME_RATE && downloads_busy()
                      ? "Downloads temporarily use the 60 FPS UI. Your selected mode returns when the queue is finished."
                      : NOTE[g_cursor], 0.0f);

    /* The keys at the foot, the way the system names its own: what X
       does on this row, SELECT for the frame rate of this run, O back. */
    if (shell_footer_free()) {
        const char *x = g_cursor == SYS_FRAME_RATE ? T_HINT_CHANGE
                      : g_cursor == SYS_SWEEP ? T_HINT_RUN
                      : g_cursor == SYS_RESET ? T_HINT_RESTORE : T_HINT_TOGGLE;
        float hx = draw_hint(LIST_X, FOOTER_BASE, MARK_CROSS, x, g_dim);
        hx = draw_hint(hx, FOOTER_BASE, MARK_SELECT, T_HINT_UI_MODE, g_dim);
        draw_hint(hx, FOOTER_BASE, MARK_CIRCLE, T_HINT_BACK, g_dim);
    }
}
