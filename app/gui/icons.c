/*
 * The list's icons. Each one goes through the same path as the card's still
 * -- an installed app's own EBOOT, else the cache on the stick, else one TLS
 * fetch -- and is then shrunk by
 * two, because a 144x80 picture in a 256x128 texture is 128 KB and sixty of
 * them would be a quarter of the machine, while 72x40 in 128x64 is 32 KB
 * and still twice what a row can show.
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

/* At most a screenful is ever wanted at once, and the list holds six rows. */
#define WANTED 8

static const struct catalog *g_catalog;
static struct gfx_texture g_icons[MAX_APPS];
static volatile enum icon_state g_state[MAX_APPS];
static volatile int g_want[WANTED];
static volatile int g_want_count;

void icons_bind(const struct catalog *catalog) { g_catalog = catalog; }

void icons_reset(void) {
    for (int i = 0; i < MAX_APPS; i++) {
        gfx_texture_free(&g_icons[i]);
        g_state[i] = ICON_NONE;
    }
    g_want_count = 0;
}

int icons_want(const int *index, int count) {
    if (!g_catalog) return 0;
    if (count > WANTED) count = WANTED;
    int fresh = 0, n = 0;
    /* The count goes to zero first: the media thread reads the two without
       a lock, and a stale index is worse than a short list for one frame. */
    g_want_count = 0;
    for (int i = 0; i < count; i++) {
        int at = index[i];
        if (at < 0 || at >= g_catalog->count) continue;
        g_want[n++] = at;
        if (g_state[at] == ICON_NONE) { g_state[at] = ICON_WANTED; fresh = 1; }
    }
    g_want_count = n;
    return fresh;
}

const struct gfx_texture *icons_get(int index) {
    if (index < 0 || index >= MAX_APPS || g_state[index] != ICON_READY) return 0;
    return &g_icons[index];
}

int icons_pending(void) {
    int count = g_want_count;
    for (int i = 0; i < count && i < WANTED; i++) {
        int at = g_want[i];
        if (at >= 0 && at < MAX_APPS && g_state[at] == ICON_WANTED) return at;
    }
    return -1;
}

/* Every two by two of the source averaged into one texel of the result. */
static int shrink(const struct gfx_texture *big, struct gfx_texture *small) {
    memset(small, 0, sizeof(*small));
    small->w = big->w / 2;
    small->h = big->h / 2;
    small->tw = 128;
    small->th = 64;
    if (small->w > small->tw || small->h > small->th || small->w == 0 || small->h == 0)
        return -1;
    small->pixels = memalign(16, (size_t)small->tw * small->th * 4);
    if (!small->pixels) return -1;
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
    if (!g_catalog || index < 0 || index >= g_catalog->count) return;
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
        const void *png = asset_fetch(ASSET_ICON, entry->id, entry->icon, entry->media_cached_only, &len);
        if (!png || image_decode_png(png, len, &big) != 0) {
            g_state[index] = ICON_MISSING;
            return;
        }
    }
    int rc = shrink(&big, &g_icons[index]);
    gfx_texture_free(&big);
    g_state[index] = rc == 0 ? ICON_READY : ICON_MISSING;
}
