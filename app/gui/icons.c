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
/* Ten screens of rows: scrolling back finds them still here, not on the
   stick. 64 of 32 KB. */
#define SLOTS 64

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

static int make_thumb(const struct app_entry *entry, struct gfx_texture *small);

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
        if (asset_thumb_have(e->id, txt(e->icon))) continue;
        static struct gfx_texture scratch;
        make_thumb(e, &scratch);
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

const struct gfx_texture *icons_get_entry(const struct app_entry *entry) {
    if (!g_catalog || !entry || entry < g_catalog->apps || entry >= g_catalog->apps + g_catalog->count)
        return 0;
    return icons_get((int)(entry - g_catalog->apps));
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
static int icons_pending(void) { return pending_among(g_want_count); }
static int icons_pending_visible(void) {
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

/* An icon as the pack keeps it once shrunk: its width and height, two
   bytes of nothing, then its rows of RGBA, so that one seen before is a
   read and a copy and no PNG is decoded again. */
#define THUMB_MAX (4 + 128 * 64 * 4)
static unsigned char g_thumb[THUMB_MAX];

static int from_thumb(struct gfx_texture *t, const unsigned char *b, size_t n) {
    int w = b[0], h = b[1];
    if (n < 4 || w == 0 || h == 0 || w > 128 || h > 64 || n != 4 + (size_t)w * h * 4)
        return -1;
    if (!t->pixels && !(t->pixels = memalign(16, (size_t)128 * 64 * 4)))
        return -1;
    t->w = w;
    t->h = h;
    t->tw = 128;
    t->th = 64;
    t->opaque = 0;
    memset(t->pixels, 0, (size_t)128 * 64 * 4);
    for (int y = 0; y < h; y++)
        memcpy((unsigned char *)t->pixels + (size_t)y * 128 * 4, b + 4 + (size_t)y * w * 4, (size_t)w * 4);
    sceKernelDcacheWritebackRange(t->pixels, (size_t)128 * 64 * 4);
    return 0;
}

static size_t to_thumb(const struct gfx_texture *t, unsigned char *b) {
    b[0] = (unsigned char)t->w;
    b[1] = (unsigned char)t->h;
    b[2] = b[3] = 0;
    for (int y = 0; y < t->h; y++)
        memcpy(b + 4 + (size_t)y * t->w * 4, (const unsigned char *)t->pixels + (size_t)y * t->tw * 4,
               (size_t)t->w * 4);
    return 4 + (size_t)t->w * t->h * 4;
}

/* A catalog icon fetched, decoded and shrunk into small, then kept as a
   thumbnail. -1 when there is none to be had. */
static int make_thumb(const struct app_entry *entry, struct gfx_texture *small) {
    size_t len = 0;
    const void *png = asset_fetch(ASSET_ICON, entry->id, txt(entry->icon), entry->media_cached_only, &len);
    struct gfx_texture big;
    if (!png || image_decode_png(png, len, &big) != 0)
        return -1;
    int rc = shrink(&big, small);
    gfx_texture_free(&big);
    if (rc != 0)
        return -1;
    if (txt(entry->icon)[0])
        asset_thumb_put(entry->id, txt(entry->icon), g_thumb, to_thumb(small, g_thumb));
    return 0;
}

/* The slot to fill, emptied of the entry it held. */
static int claim_slot(void) {
    int s = take_slot();
    if (s < 0) return -1;
    /* The entry the slot held has no icon from here on, before a pixel of
       it changes. */
    int old = g_slots[s].index;
    if (old >= 0 && old < MAX_APPS) {
        g_slot_of[old] = -1;
        g_state[old] = ICON_NONE;
    }
    g_slots[s].index = -1;
    return s;
}

static void ready(int s, int index) {
    g_slots[s].index = index;
    g_slots[s].used = g_clock;
    g_slot_of[index] = (signed char)s;
    g_state[index] = ICON_READY;
}

static void icons_load(int index) {
    if (!g_catalog || index < 0 || index >= g_catalog->count || index >= MAX_APPS) return;
    const struct app_entry *entry = &g_catalog->apps[index];
    /* An installed row is pictured from its own EBOOT before anything is
       asked of the cache or the network: the stick is free, and the bytes
       are the app's own rather than what a catalog says about it. Only a
       bundle without an ICON0 falls through to the catalog's. */
    if (entry->state != APP_NOT_INSTALLED) {
        char path[128];
        void *png;
        size_t len;
        struct gfx_texture big;
        if (pbp_installed_path(entry->id, path, sizeof(path)) == 0 &&
            pbp_section(path, PBP_ICON0, &png, &len) == 0) {
            int decoded = image_decode_png(png, len, &big);
            free(png);
            int s = decoded == 0 ? claim_slot() : -1;
            if (decoded == 0 && s >= 0 && shrink(&big, &g_slots[s].tex) == 0) {
                gfx_texture_free(&big);
                ready(s, index);
                return;
            }
            if (decoded == 0) gfx_texture_free(&big);
        }
    }
    /* Kept from before: a copy into a slot. */
    size_t n = txt(entry->icon)[0] ? asset_thumb_get(entry->id, txt(entry->icon), g_thumb, sizeof(g_thumb)) : 0;
    if (n) {
        int s = claim_slot();
        if (s >= 0 && from_thumb(&g_slots[s].tex, g_thumb, n) == 0) {
            ready(s, index);
            return;
        }
    }
    int s = claim_slot();
    if (s < 0) {
        g_state[index] = ICON_NONE;
        return;
    }
    if (make_thumb(entry, &g_slots[s].tex) != 0) {
        g_state[index] = ICON_MISSING;
        return;
    }
    ready(s, index);
}

/* ------------------------------------------------------------ the thread */

/* With the card's thread, below the main thread: the list keeps its frame
   rate and the fetching runs in the time it spends waiting for vblank. */
#define ICON_PRIORITY 0x24
#define ICON_STACK (32 * 1024)
static SceUID g_ithread = -1, g_iwake = -1, g_iidle = -1;
static volatile int g_iquit, g_ihold;
static int g_hold_ready;

/* The rows on screen first, then the rows past the edges; with nothing
   asked for 0.4 s, rows further out are fetched ahead, one at a time and
   stopping for any row that comes on screen meanwhile. What is held in
   memory goes to the stick once two seconds have passed with nothing to
   do. */
static int icon_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int quiet = 0;
    for (;;) {
        SceUInt wait = 400 * 1000;
        int timeout = sceKernelWaitSema(g_iwake, 1, &wait) < 0;
        if (g_iquit) break;
        if (g_ihold) {
            asset_flush();
            sceKernelSignalSema(g_iidle, 1);
            while (g_ihold && !g_iquit) sceKernelWaitSema(g_iwake, 1, 0);
            continue;
        }
        int did = 0, at;
        while (!g_ihold && !g_iquit && (at = icons_pending_visible()) >= 0) { icons_load(at); did = 1; }
        while (!g_ihold && !g_iquit && icons_pending_visible() < 0 && (at = icons_pending()) >= 0) {
            icons_load(at);
            did = 1;
        }
        if (timeout && !did)
            while (!g_ihold && !g_iquit && icons_pending() < 0 && icons_prefetch_one()) did = 1;
        quiet = did ? 0 : quiet + 1;
        if (quiet == 5) asset_flush();
    }
    asset_flush();
    sceKernelSignalSema(g_iidle, 1);
    return 0;
}

void icons_poke(void) { if (g_iwake >= 0) sceKernelSignalSema(g_iwake, 1); }

void icons_start(void) {
    g_iquit = g_ihold = 0;
    g_iwake = sceKernelCreateSema("icons_wake", 0, 0, 64, 0);
    g_iidle = sceKernelCreateSema("icons_idle", 0, 0, 64, 0);
    g_ithread = sceKernelCreateThread("icons", icon_thread, ICON_PRIORITY, ICON_STACK,
                                      PSP_THREAD_ATTR_USER, 0);
    if (g_ithread >= 0) sceKernelStartThread(g_ithread, 0, 0);
    else logline("icons: no thread %08x", (unsigned)g_ithread);
}

void icons_stop(void) {
    if (g_ithread >= 0) {
        g_iquit = 1;
        icons_poke();
        sceKernelWaitSema(g_iidle, 1, 0);
        sceKernelWaitThreadEnd(g_ithread, 0);
        sceKernelDeleteThread(g_ithread);
        g_ithread = -1;
    }
    if (g_iwake >= 0) { sceKernelDeleteSema(g_iwake); g_iwake = -1; }
    if (g_iidle >= 0) { sceKernelDeleteSema(g_iidle); g_iidle = -1; }
}

void icons_hold_begin(void) {
    g_hold_ready = g_ithread < 0;
    if (g_ithread < 0) return;
    /* Only this hold's answer counts. */
    while (sceKernelPollSema(g_iidle, 1) == 0) {}
    g_ihold = 1;
    icons_poke();
}
int icons_hold_ready(void) {
    if (!g_hold_ready && sceKernelPollSema(g_iidle, 1) == 0) g_hold_ready = 1;
    return g_hold_ready;
}
void icons_hold_end(void) {
    g_ihold = 0;
    g_hold_ready = 0;
    icons_poke();
}
