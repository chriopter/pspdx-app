/*
 * Drawing a mark. Every glyph goes through the same three steps, so the set
 * reads as one family whatever it is spelling: a wide soft light behind it
 * when it is the one being looked at, a blurred dark copy of itself one
 * pixel down and right, then the glyph itself.
 *
 * The dark copy is the whole trick, and it is the system's own: the XMB
 * ships a blurred shadow texture beside every foreground glyph it has --
 * tex_cross is thirteen pixels and tex_cross_shadow twenty-one -- because a
 * white shape on a photograph loses its edge wherever the photograph is
 * pale. Ours are made the same way, in assets/marks, and cost one sprite.
 *
 * The highlight is the second thing taken from the originals: the lit state
 * is not a brighter glyph, it is the same glyph with more light behind it.
 * Brightening the strokes would make a selected mark read as a different,
 * bolder mark; putting the light behind leaves the shape alone.
 *
 * Both pictures live in one sheet, which is why a mark is two sprites and
 * not two textures: the whole set binds once. The sheet arrives as coverage,
 * one byte a pixel, and is opened out into RGBA at first use -- white where
 * a glyph cell is, black where a shadow cell is -- because that is the only
 * thing the alpha does not already say.
 */

#include <malloc.h>
#include <math.h>
#include <string.h>

#include <pspkernel.h>

#include "gui/gfx.h"
#include "gui/marks.h"
#include "gui/marks_data.h"

/* The order of the table is the order of the enum and nothing enforces that
   but the list at the top of tools/marks/embed.py. The count at least can be
   checked, and a negative array size is how C says no at compile time: if
   this line is what failed, a mark was added to one list and not the other. */
typedef char mark_table_matches_enum[
    (int)(sizeof(mark_glyphs) / sizeof(mark_glyphs[0])) == MARK_COUNT ? 1 : -1];

#define SHEET_BYTES ((size_t)MARK_SHEET_W * MARK_SHEET_H * 4)

static struct gfx_texture g_sheet;
static int g_opened;

static const struct mark_glyph *glyph(enum mark m) {
    if (m < 0 || m >= MARK_COUNT) return &mark_glyphs[MARK_CROSS];
    return &mark_glyphs[m];
}

int mark_width(enum mark m) { return glyph(m)->w; }

/* One cell of coverage into one colour. Everything outside a cell stays the
   zero the buffer was cleared to, so the gutters never sample as anything. */
static void open_cell(unsigned *px, int x0, int y0, int w, int h, unsigned rgb) {
    for (int y = 0; y < h; y++) {
        const unsigned char *a = mark_sheet + (y0 + y) * MARK_SHEET_W + x0;
        unsigned *row = px + (y0 + y) * MARK_SHEET_W + x0;
        for (int x = 0; x < w; x++)
            row[x] = rgb | ((unsigned)a[x] << 24);
    }
}

/* Built once, at the first mark of the run rather than at startup: the
   entropy screen draws no marks and should not pay for them. */
static const struct gfx_texture *sheet(void) {
    if (!g_opened) {
        g_opened = 1;
        g_sheet.w = g_sheet.tw = MARK_SHEET_W;
        g_sheet.h = g_sheet.th = MARK_SHEET_H;
        g_sheet.opaque = 0;
        g_sheet.pixels = memalign(16, SHEET_BYTES);
        if (g_sheet.pixels) {
            unsigned *px = g_sheet.pixels;
            memset(px, 0, SHEET_BYTES);
            for (int i = 0; i < MARK_COUNT; i++) {
                const struct mark_glyph *g = &mark_glyphs[i];
                open_cell(px, g->gx, g->gy, g->gw, g->gh, 0x00FFFFFFu);
                open_cell(px, g->sx, g->sy, g->sw, g->sh, 0x00000000u);
            }
            /* The GE reads system RAM behind the cache's back. */
            sceKernelDcacheWritebackRange(g_sheet.pixels, SHEET_BYTES);
        }
    }
    return g_sheet.pixels ? &g_sheet : NULL;
}

/* Whole pixels: a cell landing on a half pixel would be resampled and every
   two-pixel stroke would go soft. The cell is one pixel larger than the mark
   on each side, which is where the anti-aliasing lives, so centring the cell
   is what centres the mark. */
static float top_left(float centre, int size) {
    return (float)(int)(centre - size / 2.0f + 0.5f);
}

void mark_draw(enum mark m, float cx, float cy, unsigned color, int state,
               unsigned tint, float t) {
    const struct mark_glyph *g = glyph(m);
    int scale = (int)(color >> 24);
    if (scale <= 0) return;
    const struct gfx_texture *sh = sheet();
    if (!sh) return;

    if (state == MARK_LIT) {
        /* Three times the mark across, so the light is a halo around it and
           not a lamp inside it, and breathing just enough to be noticed only
           when the mark is looked at. */
        float pulse = 0.88f + 0.12f * sinf(t * 2.2f);
        int a = (int)(110.0f * pulse * scale / 255.0f);
        gfx_glow(cx, cy, g->w * 3.0f, g->h * 3.0f,
                 (tint & 0x00FFFFFFu) | ((unsigned)a << 24));
    }
    if (state != MARK_DIM)
        gfx_texture_draw_part(sh, g->sx, g->sy, g->sw, g->sh,
                              top_left(cx, g->sw) + 1.0f,
                              top_left(cy, g->sh) + 1.0f,
                              RGBA(0, 0, 0, 190 * scale / 255));
    gfx_texture_draw_part(sh, g->gx, g->gy, g->gw, g->gh,
                          top_left(cx, g->gw), top_left(cy, g->gh), color);
}
