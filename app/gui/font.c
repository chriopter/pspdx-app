#include "util/storage.h"
#include <pspiofilemgr.h>
#include <intraFont.h>
#include <stdarg.h>
#include <stdio.h>

#include "gui/font.h"
#include "gui/gfx.h"
#include "util/runtime.h"

/* ltn8 is the sans-serif face the system shell uses; ltn0 is the serif one
   and only a fallback for firmware that lacks the first.

   The ms0 path is for emulators, which have the font files but do not mount
   flash0 for the guest. Nothing is shipped there -- the test rig copies the
   emulator's own font in. */
static const char *CANDIDATES[] = {
    "flash0:/font/ltn8.pgf",
    "flash0:/font/ltn0.pgf",
    NULL,
};

static intraFont *g_font;

/* size is intraFont's scale factor, where 1.0 is the face at its design size
   -- about 20 px tall on this screen. */
/* The system shell's own sizes: names at the size of an XMB item, the
   rows a step under, and nothing smaller than the XMB's smallest line. */
static const struct { float size; unsigned shadow; } STYLES[] = {
    [FONT_DISPLAY] = { 1.35f, 0x00000000 },
    [FONT_H1]   = { 1.00f, 0xA0000000 },
    [FONT_BODY] = { 0.82f, 0x80000000 },
    [FONT_META] = { 0.72f, 0x70000000 },
};

/* ---------------------------------------------------------- measurements */

/* Measuring is the expensive half of drawing text: intraFont converts the
   string and walks the charmap for every character. The screen holds the
   same dozen strings frame after frame, so remember what each one measured.
   The key is the content, not the pointer, which makes it right both for
   the catalog's stored strings and for the buffers rebuilt in place. */
#define MEASURE_SLOTS 32

struct measured {
    unsigned key;
    int len;                /* -1 for an empty slot; also guards collisions */
    int fit;                /* characters that fit within the clip width */
    float width;            /* what those characters measure */
};

static struct measured g_measured[MEASURE_SLOTS];
static int g_next_slot;

static void forget_measurements(void) {
    for (int i = 0; i < MEASURE_SLOTS; i++) g_measured[i].len = -1;
    g_next_slot = 0;
}

static unsigned key_of(const char *text, int len, enum font_style style,
                       float clip) {
    unsigned h = 2166136261u;                           /* FNV-1a */
    for (int i = 0; i < len; i++) h = (h ^ (unsigned char)text[i]) * 16777619u;
    h = (h ^ (unsigned)style) * 16777619u;
    /* Quarter-pixels: a clip width can come from where the last print ended,
       which is a float, and it is the same float on the next frame. */
    h = (h ^ (unsigned)(int)(clip < 0.0f ? -1.0f : clip * 4.0f)) * 16777619u;
    return h;
}

/* clip < 0 means no limit. The font must already carry the style. */
static const struct measured *measure(enum font_style style, const char *text,
                                      float clip) {
    int len = 0;
    while (text[len]) len++;
    unsigned key = key_of(text, len, style, clip);
    for (int i = 0; i < MEASURE_SLOTS; i++)
        if (g_measured[i].key == key && g_measured[i].len == len)
            return &g_measured[i];

    int fit = len;
    float width = intraFontMeasureTextEx(g_font, text, len);
    if (clip >= 0.0f && width > clip) {
        /* Halve the range rather than drop one character at a time: the same
           last character that fits, a handful of measurements instead of one
           per character cut. */
        int lo = 0, hi = len;                   /* lo fits, hi does not */
        float lo_width = 0.0f;
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            float w = intraFontMeasureTextEx(g_font, text, mid);
            if (w > clip) hi = mid;
            else { lo = mid; lo_width = w; }
        }
        fit = lo;
        width = lo_width;
    }

    struct measured *slot = &g_measured[g_next_slot];
    g_next_slot = (g_next_slot + 1) % MEASURE_SLOTS;
    slot->key = key;
    slot->len = len;
    slot->fit = fit;
    slot->width = width;
    return slot;
}

/* ---------------------------------------------------------------- drawing */

/* intraFont carries the style on the font itself, and every print already
   binds the font texture on its own, so restyling for a print that wants
   what the last one wanted is pure cost. */
static int g_styled = -1;
static unsigned g_color;

static void use(enum font_style style, unsigned color) {
    color = gfx_veiled(color);
    if ((int)style == g_styled && color == g_color) return;
    intraFontSetStyle(g_font, STYLES[style].size, color,
                      gfx_veiled(STYLES[style].shadow), 0.0f, INTRAFONT_ALIGN_LEFT);
    g_styled = style;
    g_color = color;
}

int font_init(void) {
    if (!intraFontInit()) {
        logline("font: intraFontInit failed");
        return 0;
    }
    for (unsigned i = 0; i < sizeof(CANDIDATES) / sizeof(*CANDIDATES); i++) {
        g_font = intraFontLoad((CANDIDATES[i] ? CANDIDATES[i] : storage_path("PSP/PSPDX/DEBUG/font/ltn8.pgf")), INTRAFONT_CACHE_ALL);
        if (g_font) {
            logline("font: %s", (CANDIDATES[i] ? CANDIDATES[i] : storage_path("PSP/PSPDX/DEBUG/font/ltn8.pgf")));
            g_styled = -1;
            forget_measurements();
            return 1;
        }
    }
    int dir = sceIoDopen("flash0:/font");
    logline("font: nothing loadable, flash0:/font dopen=%d", dir);
    if (dir >= 0) sceIoDclose(dir);
    intraFontShutdown();
    return 0;
}

void font_shutdown(void) {
    if (!g_font) return;
    intraFontUnload(g_font);
    g_font = 0;
    g_styled = -1;
    intraFontShutdown();
}

int font_ready(void) { return g_font != 0; }

float font_print(enum font_style style, float x, float y, unsigned color,
                 const char *text) {
    if (!g_font || !text) return x;
    use(style, color);
    return intraFontPrint(g_font, x, y, text);
}

float font_printf(enum font_style style, float x, float y, unsigned color,
                  const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return font_print(style, x, y, color, line);
}

float font_print_clipped(enum font_style style, float x, float y, float width,
                         unsigned color, const char *text) {
    if (!g_font || !text) return x;
    if (width < 0.0f) return x;         /* no column left to print into */
    use(style, color);
    /* Print down to the last character that still fits and stop there; the
       column printers wrap or scroll instead, and this UI wants neither. */
    return intraFontPrintEx(g_font, x, y, text, measure(style, text, width)->fit);
}

float font_width(enum font_style style, const char *text) {
    if (!g_font || !text) return 0.0f;
    /* Measuring reads the size and nothing else, so keep the colour the last
       print set rather than making the next one set it again. */
    use(style, g_color);
    return measure(style, text, -1.0f)->width;
}

float font_height(enum font_style style) {
    if (!g_font) return 0.0f;
    return g_font->advancey * STYLES[style].size / 4.0f;
}
