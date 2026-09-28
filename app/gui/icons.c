/*
 * The list's icons. Each one goes through the same path as the card's still
 * -- an installed app's own EBOOT, else the cache on the stick, else one TLS
 * fetch -- and is then shrunk by
 * two, because a 144x80 picture in a 256x128 texture is 128 KB and sixty of
 * them would be a quarter of the machine, while 72x40 in 128x64 is 32 KB
 * and still twice what a row can show.
 *
 * A catalog can list thousands of apps, so an icon is not kept for every
 * one that was ever on screen: SLOTS of them are, and the one not wanted for
 * longest makes room for the next. A slot's pixels are allocated once and
 * never freed while the list runs, so a texture the GE is still drawing from
 * is never pulled out from under it; one that is refilled was off screen.
 */

#include <malloc.h>
#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

#include "gui/icons.h"
#include "gui/image.h"
#include "update/assets.h"
#include "util/pbp.h"
#include "util/runtime.h"

enum icon_state { ICON_NONE, ICON_WANTED, ICON_READY, ICON_MISSING };

/* A screenful and three rows past either edge. */
#define WANTED 16
/* Four screens of rows: scrolling back a little finds them still there. */
#define SLOTS 32

struct slot {
    struct gfx_texture tex;
    int index;                  /* the entry it holds, -1 for none */
    unsigned used;              /* when it was last wanted */
};

static const struct catalog *g_catalog;
/* Per entry: its state, and the slot its icon is in when READY. */
static volatile unsigned char g_state[MAX_APPS];
static volatile signed char g_slot_of[MAX_APPS];
static struct slot g_slots[SLOTS];
static unsigned g_clock;
static volatile int g_want[WANTED];
static volatile int g_want_count, g_want_visible;

void icons_bind(const struct catalog *catalog) { g_catalog = catalog; }

/* Rows further out, for fetching ahead, and which entries were tried
   already this run, fetched or not, so none is asked for twice. */
#define AHEAD 48
static volatile int g_ahead[AHEAD];
static volatile int g_ahead_count;
static unsigned char g_tried[(MAX_APPS + 7) / 8];

void icons_ahead(const int *index, int count) {
    if (count > AHEAD) count = AHEAD;
    g_ahead_count = 0;
    for (int i = 0; i < count; i++) g_ahead[i] = index[i];
    g_ahead_count = count;
}

int icons_prefetch_one(void) {
    if (!g_catalog) return 0;
    int count = g_ahead_count;
    for (int i = 0; i < count && i < AHEAD; i++) {
        int at = g_ahead[i];
        if (at < 0 || at >= g_catalog->count || at >= MAX_APPS) continue;
        if (g_tried[at >> 3] & (1 << (at & 7))) continue;
        g_tried[at >> 3] |= (unsigned char)(1 << (at & 7));
        const struct app_entry *e = &g_catalog->apps[at];
        /* An installed app is pictured from its own EBOOT; an entry from a
           saved catalog has no network to fetch from. */
        if (e->state != APP_NOT_INSTALLED || e->media_cached_only || !txt(e->icon)[0]) continue;
        if (asset_have(ASSET_ICON, e->id, txt(e->icon))) continue;
        size_t len;
        asset_fetch(ASSET_ICON, e->id, txt(e->icon), 0, &len);
        return 1;
    }
    return 0;
}

void icons_reset(void) {
    for (int i = 0; i < MAX_APPS; i++) {
        g_state[i] = ICON_NONE;
        g_slot_of[i] = -1;
    }
    for (int s = 0; s < SLOTS; s++) {
        /* The pixels stay for the next list; only what they stood for goes. */
        g_slots[s].index = -1;
        g_slots[s].used = 0;
    }
    g_want_count = 0;
    g_ahead_count = 0;
    memset(g_tried, 0, sizeof(g_tried));
}

static int wanted_now(int index) {
    int count = g_want_count;
    for (int i = 0; i < count && i < WANTED; i++)
        if (g_want[i] == index) return 1;
    return 0;
}

int icons_want(const int *index, int count, int visible) {
    if (!g_catalog) return 0;
    if (count > WANTED) count = WANTED;
    int fresh = 0, n = 0, shown = 0;
    g_clock++;
    /* The count goes to zero first: the media thread reads the two without
       a lock, and a stale index is worse than a short list for one frame. */
    g_want_count = 0;
    for (int i = 0; i < count; i++) {
        int at = index[i];
        if (at < 0 || at >= g_catalog->count || at >= MAX_APPS) continue;
        shown += i < visible;
        g_want[n++] = at;
        if (g_state[at] == ICON_NONE) { g_state[at] = ICON_WANTED; fresh = 1; }
        int s = g_slot_of[at];
        if (g_state[at] == ICON_READY && s >= 0) g_slots[s].used = g_clock;
    }
    g_want_visible = shown;
    g_want_count = n;
    return fresh;
}

const struct gfx_texture *icons_get(int index) {
    if (index < 0 || index >= MAX_APPS || g_state[index] != ICON_READY) return 0;
    int s = g_slot_of[index];
    return s >= 0 && s < SLOTS && g_slots[s].index == index ? &g_slots[s].tex : 0;
}

static int pending_among(int count) {
    for (int i = 0; i < count && i < WANTED; i++) {
        int at = g_want[i];
        if (at >= 0 && at < MAX_APPS && g_state[at] == ICON_WANTED) return at;
    }
    return -1;
}
int icons_pending(void) { return pending_among(g_want_count); }
int icons_pending_visible(void) {
    int count = g_want_count, visible = g_want_visible;
    return pending_among(visible < count ? visible : count);
}

/* The slot to fill: a free one, else the one wanted longest ago that is
   not on screen now. -1 when every slot is on screen, which WANTED < SLOTS
   rules out. */
static int take_slot(void) {
    int best = -1;
    for (int s = 0; s < SLOTS; s++) {
        if (g_slots[s].index < 0) return s;
        if (wanted_now(g_slots[s].index)) continue;
        if (best < 0 || g_slots[s].used < g_slots[best].used) best = s;
    }
    return best;
}

/* Every two by two of the source averaged into one texel of the result,
   into small's pixels, which are there already or allocated once here. */
static int shrink(const struct gfx_texture *big, struct gfx_texture *small) {
    int w = big->w / 2, h = big->h / 2;
    if (w > 128 || h > 64 || w == 0 || h == 0)
        return -1;
    if (!small->pixels) {
        small->pixels = memalign(16, (size_t)128 * 64 * 4);
        if (!small->pixels) return -1;
    }
    small->w = w;
    small->h = h;
    small->tw = 128;
    small->th = 64;
    small->opaque = 0;
    memset(small->pixels, 0, (size_t)small->tw * small->th * 4);
    const unsigned char *src = big->pixels;
    unsigned char *dst = small->pixels;
    for (int y = 0; y < small->h; y++) {
        const unsigned char *r0 = src + (size_t)(y * 2) * big->tw * 4;
        const unsigned char *r1 = r0 + (size_t)big->tw * 4;
        unsigned char *d = dst + (size_t)y * small->tw * 4;
        for (int x = 0; x < small->w; x++) {
            for (int c = 0; c < 4; c++) {
                int i = x * 8 + c;
                d[x * 4 + c] = (unsigned char)((r0[i] + r0[i + 4] + r1[i] + r1[i + 4] + 2) >> 2);
            }
        }
    }
    sceKernelDcacheWritebackRange(small->pixels, (size_t)small->tw * small->th * 4);
    return 0;
}

void icons_load(int index) {
    if (!g_catalog || index < 0 || index >= g_catalog->count || index >= MAX_APPS) return;
    const struct app_entry *entry = &g_catalog->apps[index];
    struct gfx_texture big;
    int decoded = -1;
    /* An installed row is pictured from its own EBOOT before anything is
       asked of the cache or the network: the stick is free, and the bytes
       are the app's own rather than what a catalog says about it. Only a
       bundle without an ICON0 falls through to the catalog's. */
    if (entry->state != APP_NOT_INSTALLED) {
        char path[128];
        void *png;
        size_t len;
        if (pbp_installed_path(entry->id, path, sizeof(path)) == 0 &&
            pbp_section(path, PBP_ICON0, &png, &len) == 0) {
            decoded = image_decode_png(png, len, &big);
            free(png);
        }
    }
    if (decoded != 0) {
        size_t len = 0;
        const void *png = asset_fetch(ASSET_ICON, entry->id, txt(entry->icon), entry->media_cached_only, &len);
        if (!png || image_decode_png(png, len, &big) != 0) {
            g_state[index] = ICON_MISSING;
            return;
        }
    }
    int s = take_slot();
    if (s < 0) {
        gfx_texture_free(&big);
        g_state[index] = ICON_NONE;
        return;
    }
    /* The entry the slot held has no icon from here on, before a pixel of
       it changes. */
    int old = g_slots[s].index;
    if (old >= 0 && old < MAX_APPS) {
        g_slot_of[old] = -1;
        g_state[old] = ICON_NONE;
    }
    g_slots[s].index = -1;
    int rc = shrink(&big, &g_slots[s].tex);
    gfx_texture_free(&big);
    if (rc != 0) {
        g_state[index] = ICON_MISSING;
        return;
    }
    g_slots[s].index = index;
    g_slots[s].used = g_clock;
    g_slot_of[index] = (signed char)s;
    g_state[index] = ICON_READY;
}
