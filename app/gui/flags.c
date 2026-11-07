#include "gui/flags.h"

#include <math.h>
#include <string.h>

#include "gui/font.h"
#include "gui/gfx.h"
#include "gui/marks.h"

/* A disc of radius r about cx, cy, a row of rectangles at a time: at eleven
   pixels a circle is a handful of spans, and a span is what gfx draws
   crisp. top and bottom say which halves are drawn. */
static void disc(float cx, float cy, float r, unsigned top, unsigned bottom) {
    for (int dy = -(int)r; dy <= (int)r; dy++) {
        int half = (int)floorf(sqrtf(r * r - (dy + 0.5f) * (dy + 0.5f)) + 0.5f);
        if (half > 0)
            gfx_rect((int)cx - half, (int)cy + dy, 2 * half, 1, dy < 0 ? top : bottom);
    }
}

void flag_draw(const char *region, int x, int y, int alpha) {
    unsigned a = (unsigned)alpha & 0xFF;
#define C(r, g, b) RGBA(r, g, b, a)
    if (!strcmp(region, "EU")) {
        gfx_rect(x, y, FLAG_W, FLAG_H, C(0, 51, 153));
        /* Twelve stars on a ring, a pixel each at this size. */
        for (int i = 0; i < 12; i++) {
            float k = i * 6.2831853f / 12;
            gfx_rect(x + 8 + (int)floorf(3.6f * sinf(k)), y + 5 + (int)floorf(3.6f * -cosf(k) + 0.5f), 1, 1,
                     C(255, 204, 0));
        }
    } else if (!strcmp(region, "US")) {
        for (int row = 0; row < FLAG_H; row++)
            gfx_rect(x, y + row, FLAG_W, 1, row % 2 ? C(255, 255, 255) : C(178, 34, 52));
        gfx_rect(x, y, 7, 6, C(60, 59, 110));
        for (int sy = 1; sy < 6; sy += 2)
            for (int sx = 1; sx < 7; sx += 2)
                gfx_rect(x + sx, y + sy, 1, 1, C(255, 255, 255));
    } else if (!strcmp(region, "JP")) {
        gfx_rect(x, y, FLAG_W, FLAG_H, C(255, 255, 255));
        disc(x + 8, y + 5.5f, 3.4f, C(188, 0, 45), C(188, 0, 45));
    } else if (!strcmp(region, "KR")) {
        gfx_rect(x, y, FLAG_W, FLAG_H, C(255, 255, 255));
        disc(x + 8, y + 5.5f, 3.4f, C(205, 46, 58), C(0, 71, 160));
        /* The four trigrams, a dark bar in each corner. */
        unsigned k = C(0, 0, 0);
        gfx_rect(x + 1, y + 1, 3, 1, k);
        gfx_rect(x + 12, y + 1, 3, 1, k);
        gfx_rect(x + 1, y + 9, 3, 1, k);
        gfx_rect(x + 12, y + 9, 3, 1, k);
    } else if (!strcmp(region, "AS")) {
        mark_draw(MARK_GLOBE, x + FLAG_W / 2.0f, y + FLAG_H / 2.0f, C(255, 255, 255), MARK_PLAIN, 0, 0);
    } else if (region[0]) {
        font_print(FONT_CAPTION, x, y + 9, C(220, 220, 220), region);
    }
#undef C
}
