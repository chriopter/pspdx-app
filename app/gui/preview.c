/*
 * What is on the card, and the thread that fetches it. The main thread
 * only ever hands over an entry and picks up finished pictures; every
 * blocking step -- reading an EBOOT or the cache off the stick, a TLS
 * fetch, decoding the PNG,
 * wrapping the film, starting the decoder -- runs on the media thread
 * below the interface, so a frame never waits for any of it. The decoder
 * itself has its own thread under video/player.c; this one starts and stops
 * it.
 */

#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio.h"
#include "util/storage.h"
#include "gui/preview.h"
#include "gui/icons.h"
#include "gui/image.h"
#include "update/assets.h"
#include "util/pbp.h"
#include "util/runtime.h"
#include "video/mp4.h"
#include "video/player.h"
#include "video/psmf.h"

#define SETTLE_MS 250
#define FILM_W 480
#define FILM_H 272
#define FILM_STRIDE 512

/* Below the decoder's 0x23: the wrap is a big memcpy and can wait for the
   pictures already in flight. */
#define MEDIA_PRIORITY 0x24
#define MEDIA_STACK (64 * 1024)

enum still_state { STILL_NONE, STILL_LOADING, STILL_READY, STILL_FAILED };
enum film_state { FILM_NONE, FILM_LOADING, FILM_PLAYING, FILM_FAILED };

/* What the main thread asked for. gen goes up with every new entry; work
   for an older gen is dropped on the floor when it finishes. The thread
   works from its own copy of the strings, taken when it picks the request
   up: the main thread writes these while the thread may be reading, and a
   fetch keyed on a half-written id would cache one app's picture under
   another's name. installed says the stick has a record of the entry; the
   thread turns that into the path of its EBOOT, which is then asked for
   each picture before the cache or the network is: the stick is free, and
   the bytes are the app's own rather than what a catalog says about it. */
struct request {
    char id[PSPDX_ID_SIZE], shot_url[256], video_url[256], sound_url[256];
    int installed, cached_only;
    char file[160];                     /* a file off the stick instead, and */
    int file_kind;                      /* what it is, a preview_file */
    char pbp[128];                      /* the thread's, empty in g_want */
    volatile unsigned gen;
};
static struct request g_want;
static unsigned g_shown_gen;            /* gen of what the pictures belong to */
static unsigned g_shown_ms;
static int g_immediate;
static int g_nothing;                   /* the selection has no picture at all */

/* Two still slots: the thread fills the one not on screen, then the
   pointer moves. The GE may still be reading the old one this frame; it
   is not written again before the next request, frames later. */
static struct gfx_texture g_stills[2];
static int g_still_slot;
static const struct gfx_texture *volatile g_still_pub;
static volatile enum still_state g_still_state;
static float g_still_alpha;

static struct gfx_texture g_film;
static void *g_film_buf[2];             /* one is drawn while the other fills */
static volatile enum film_state g_film_state;
static float g_film_alpha;
static unsigned char *g_psmf;
static struct mp4 g_track;
static int g_decoded;                   /* pictures since the film started */

static SceUID g_thread = -1, g_wake = -1, g_idle = -1;
static volatile int g_quit, g_hold;
static volatile unsigned g_done_gen;    /* gen the thread has finished with */

/* ------------------------------------------------------------ the thread */

static int stale(unsigned gen) { return gen != g_want.gen || g_quit || g_hold; }

static void load_still(const struct request *req, unsigned gen, struct gfx_texture *into) {
    int decoded = -1;
    if (req->pbp[0]) {
        void *png;
        size_t n;
        if (pbp_section(req->pbp, PBP_PIC1, &png, &n) == 0) {
            if (!stale(gen)) decoded = image_decode_png(png, n, into);
            free(png);
            if (stale(gen)) return;
        }
    }
    if (decoded != 0) {
        size_t len = 0;
        const void *png = asset_fetch(ASSET_SHOT, req->id, req->shot_url, req->cached_only, &len);
        if (stale(gen)) return;
        if (!png || image_decode_png(png, len, into) != 0) {
            g_still_state = STILL_FAILED;
            return;
        }
    }
    g_still_pub = into;
    g_still_state = STILL_READY;
}

/* Starts the decoder on a PSMF that is ours to keep: the asset buffer is
   the next fetch's, so the stream is moved out of it first. */
static void play(unsigned char *psmf, size_t n, unsigned gen) {
    if (stale(gen)) { free(psmf); return; }
    g_psmf = psmf;
    if (player_start(g_psmf, n, g_film_buf[0], g_film_buf[1], FILM_STRIDE) != 0) {
        free(g_psmf);
        g_psmf = 0;
        g_film_state = FILM_FAILED;
        return;
    }
    g_film_state = FILM_PLAYING;
}

/* An ICON1.PMF out of an EBOOT is already what the decoder reads: it goes
   to the player as it is, once its header has said the picture fits. The
   buffer is ours to keep or to free, whichever the header decides. */
static void play_psmf(unsigned char *psmf, size_t len, unsigned gen) {
    struct psmf_info info;
    if (psmf_parse(psmf, len, &info) != 0) {
        logline("film: a PSMF the header does not describe");
        free(psmf);
        g_film_state = FILM_FAILED;
        return;
    }
    if (info.width > FILM_W || info.height > FILM_H) {
        logline("film: %dx%d, at most %dx%d", info.width, info.height, FILM_W, FILM_H);
        free(psmf);
        g_film_state = FILM_FAILED;
        return;
    }
    g_film.w = info.width;
    g_film.h = info.height;
    logline("film: psmf %dx%d, %d pictures, %u a second, %lu KB as it is", info.width,
            info.height, info.frames, 90000 / psmf_frame_ticks(&info),
            (unsigned long)(len / 1024));
    play(psmf, len, gen);
}

static void load_bytes(const unsigned char *mp4, size_t len, unsigned gen);

/* The same out of the asset buffer, which the next fetch overwrites: the
   stream is copied out first. */
static void load_psmf(const unsigned char *data, size_t len, unsigned gen) {
    unsigned char *psmf = malloc(len);
    if (!psmf) { logline("film: no room for %lu", (unsigned long)len); g_film_state = FILM_FAILED; return; }
    memcpy(psmf, data, len);
    play_psmf(psmf, len, gen);
}

static void load_film(const struct request *req, unsigned gen) {
    if (req->pbp[0]) {
        void *pmf;
        size_t n;
        if (pbp_section(req->pbp, PBP_ICON1, &pmf, &n) == 0) {
            /* Read straight into a buffer of its own, so it is the
               player's without a copy. */
            if (stale(gen)) { free(pmf); return; }
            play_psmf(pmf, n, gen);
            return;
        }
    }
    size_t len = 0;
    const unsigned char *mp4 = asset_fetch(ASSET_VIDEO, req->id, req->video_url, req->cached_only, &len);
    if (stale(gen)) return;
    if (!mp4) { g_film_state = FILM_FAILED; return; }
    load_bytes(mp4, len, gen);
}

/* A film as bytes, whatever they were called: a PSMF goes as it is, an
   MP4 is wrapped. The buffer stays the caller's. */
static void load_bytes(const unsigned char *mp4, size_t len, unsigned gen) {
    /* The bytes say what they are, not the name they were served under. */
    if (psmf_is(mp4, len)) { load_psmf(mp4, len, gen); return; }
    if (mp4_parse(mp4, len, &g_track) != 0) {
        logline("film: not a video track the PSP can play");
        g_film_state = FILM_FAILED;
        return;
    }
    /* Up to the screen's own size; the catalog's clips are half that, and
       the card scales whatever comes. The wrapper holds it to multiples of
       sixteen, which is what the decoder's header can say. */
    if (g_track.width > FILM_W || g_track.height > FILM_H) {
        logline("film: %dx%d, at most %dx%d", g_track.width, g_track.height, FILM_W, FILM_H);
        g_film_state = FILM_FAILED;
        return;
    }
    g_film.w = g_track.width;
    g_film.h = g_track.height;
    size_t cap = psmf_capacity(len);
    unsigned char *psmf = malloc(cap);
    if (!psmf) { logline("film: no room for %lu", (unsigned long)cap); g_film_state = FILM_FAILED; return; }
    size_t n = psmf_build(mp4, &g_track, psmf, cap);
    if (!n) { free(psmf); logline("film: could not wrap"); g_film_state = FILM_FAILED; return; }
    logline("film: %d pictures, %lu KB wrapped", g_track.count, (unsigned long)(n / 1024));
    play(psmf, n, gen);
}

/* The sound is the last thing fetched for a row, after the pictures the
   eye is waiting for. It is handed to the audio side, which copies it; a
   selection that moved meanwhile takes it back at once, so a fetch that
   landed late never plays under the wrong row. Without a link the cache
   is still tried, which is how a rig plants one. */
static void load_sound(const struct request *req, unsigned gen) {
    if (req->pbp[0]) {
        void *at3;
        size_t n;
        if (pbp_section(req->pbp, PBP_SND0, &at3, &n) == 0) {
            if (!stale(gen)) {
                audio_sound_play(at3, n);
                if (stale(gen)) audio_sound_stop();
            }
            free(at3);
            return;
        }
    }
    size_t len = 0;
    const void *at3 = asset_fetch(ASSET_SOUND, req->id, req->sound_url, req->cached_only, &len);
    if (!at3 || stale(gen)) return;
    audio_sound_play(at3, len);
    if (stale(gen)) audio_sound_stop();
}

/* The list's icons come after the card: one at a time, and only while no
   newer selection is waiting, so a scroll through the list is never held
   up by the icons of the rows it left. */
static void load_icons(unsigned gen) {
    int index;
    while (!stale(gen) && (index = icons_pending()) >= 0) icons_load(index);
}

/* One request at a time, the newest. Between requests the decoder is
   stopped and the last film let go of, so nothing here ever runs two
   films or two fetches at once. */
static int media_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned served = 0;
    for (;;) {
        sceKernelWaitSema(g_wake, 1, 0);
        if (g_quit) break;
        if (g_hold) { sceKernelSignalSema(g_idle, 1); continue; }
        unsigned gen = g_want.gen;
        if (gen == served) { load_icons(gen); continue; }
        /* The strings, copied whole: the main thread writes them before it
           raises gen, so a copy that sees the same gen after as before saw
           a finished request. */
        struct request req;
        do {
            gen = g_want.gen;
            memcpy(&req, &g_want, sizeof(req));
        } while (gen != g_want.gen);
        served = gen;
        /* The record on the stick is read here, off the main thread: a
           selection is not held up by a file open. An installed entry
           whose record names no directory is pictured like any other. */
        req.pbp[0] = '\0';
        if (req.installed) pbp_installed_path(req.id, req.pbp, sizeof(req.pbp));

        player_stop();
        free(g_psmf);
        g_psmf = 0;

        if (req.file[0]) {
            /* One file for its own sake: read whole, played, nothing else
               touched. The still is left empty on purpose. */
            char *bytes = NULL;
            int n = storage_read(req.file, &bytes, 2 * 1024 * 1024);
            if (n <= 0 || stale(gen)) {
                if (n <= 0) logline("file: %s would not read", req.file);
                g_film_state = FILM_FAILED;
            } else if (req.file_kind == PREVIEW_FILE_FILM) {
                g_film_state = FILM_LOADING;
                load_bytes((unsigned char *)bytes, (size_t)n, gen);
            } else {
                audio_sound_play(bytes, (size_t)n);
                if (stale(gen)) audio_sound_stop();
            }
            free(bytes);
            if (gen == g_want.gen) g_done_gen = gen;
            else if (g_film_state == FILM_PLAYING) { player_stop(); free(g_psmf); g_psmf = 0; g_film_state = FILM_NONE; }
            load_icons(gen);
            continue;
        }

        int slot = g_still_slot ^ 1;
        gfx_texture_free(&g_stills[slot]);
        g_still_state = STILL_LOADING;
        load_still(&req, gen, &g_stills[slot]);
        if (stale(gen)) { gfx_texture_free(&g_stills[slot]); continue; }
        g_still_slot = slot;

        /* The cache is tried even without a link, so this is asked of
           every entry; it comes back at once when there is nothing. */
        if (g_film_buf[0]) {
            g_film_state = FILM_LOADING;
            load_film(&req, gen);
        } else {
            g_film_state = FILM_FAILED;
        }
        load_sound(&req, gen);
        if (gen == g_want.gen) g_done_gen = gen;
        else if (g_film_state == FILM_PLAYING) { player_stop(); free(g_psmf); g_psmf = 0; g_film_state = FILM_NONE; }
        load_icons(gen);
    }
    player_stop();
    free(g_psmf);
    g_psmf = 0;
    sceKernelSignalSema(g_idle, 1);
    return 0;
}

static void wake(void) { if (g_wake >= 0) sceKernelSignalSema(g_wake, 1); }

/* ----------------------------------------------------------- the interface */

void preview_init(void) {
    memset(g_stills, 0, sizeof(g_stills));
    g_still_pub = 0;
    g_still_state = STILL_NONE;
    g_want.id[0] = '\0';
    g_want.gen = 0;
    g_shown_gen = 0;
    g_done_gen = 0;
    g_quit = g_hold = 0;
    /* The film decodes into two textures that live for the whole run: the
       decoder writes one while the GE reads the other, and a picture
       changes hands as a pointer. The CPU never touches either. */
    g_film.w = FILM_W; g_film.h = FILM_H;
    g_film.tw = FILM_STRIDE; g_film.th = 512;
    g_film.opaque = 1;
    for (int i = 0; i < 2; i++) {
        g_film_buf[i] = memalign(16, (size_t)FILM_STRIDE * 512 * 4);
        if (g_film_buf[i]) memset(g_film_buf[i], 0, (size_t)FILM_STRIDE * 512 * 4);
    }
    if (g_film_buf[0] && g_film_buf[1]) {
        g_film.pixels = g_film_buf[0];
        sceKernelDcacheWritebackInvalidateAll();
    } else {
        free(g_film_buf[0]); free(g_film_buf[1]);
        g_film_buf[0] = g_film_buf[1] = 0;
    }
    g_wake = sceKernelCreateSema("media_wake", 0, 0, 64, 0);
    g_idle = sceKernelCreateSema("media_idle", 0, 0, 64, 0);
    g_thread = sceKernelCreateThread("media", media_thread, MEDIA_PRIORITY, MEDIA_STACK,
                                     PSP_THREAD_ATTR_USER, 0);
    if (g_thread >= 0) sceKernelStartThread(g_thread, 0, 0);
    else logline("media: no thread %08x", (unsigned)g_thread);
}

void preview_shutdown(void) {
    audio_sound_stop();
    if (g_thread >= 0) {
        g_quit = 1;
        wake();
        sceKernelWaitSema(g_idle, 1, 0);
        sceKernelWaitThreadEnd(g_thread, 0);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (g_wake >= 0) { sceKernelDeleteSema(g_wake); g_wake = -1; }
    if (g_idle >= 0) { sceKernelDeleteSema(g_idle); g_idle = -1; }
    gfx_texture_free(&g_stills[0]);
    gfx_texture_free(&g_stills[1]);
    g_still_pub = 0;
    free(g_film_buf[0]);
    free(g_film_buf[1]);
    g_film_buf[0] = g_film_buf[1] = 0;
    memset(&g_film, 0, sizeof(g_film));
    g_still_state = STILL_NONE;
    g_film_state = FILM_NONE;
    g_want.id[0] = '\0';
}

void preview_quiesce(void) {
    if (g_thread < 0) return;
    /* Every wake the thread takes while held is answered with an idle
       signal, and a poke for icons during an install is such a wake -- so
       the count can be ahead of the holds. A stale signal left over would
       let the next quiesce return while the thread is still in a fetch,
       and the installer and the media thread would then share the HTTPS
       stack and the asset buffer at once. The soak found exactly that: a
       film fetched in the middle of an unpack. So the count is drained
       first, and the one waited for is the one this hold earns. */
    while (sceKernelPollSema(g_idle, 1) == 0) {}
    g_hold = 1;
    wake();
    sceKernelWaitSema(g_idle, 1, 0);
}

void preview_resume(void) {
    g_hold = 0;
    wake();
}

void preview_show(const struct app_entry *entry, int immediately) {
    /* A row that is not a package -- the basket's own Download all, which
       stands for several of them -- has no picture to fetch. The card empties
       and the thread is asked for nothing at all: an id of no characters is
       not a shorter request, it is a request for the wrong thing. */
    if (strcmp(g_want.id, entry ? entry->id : "") == 0) return;
    snprintf(g_want.id, sizeof(g_want.id), "%s", entry ? entry->id : "");
    g_want.cached_only = entry ? entry->media_cached_only : 1;
    snprintf(g_want.shot_url, sizeof(g_want.shot_url), "%s",
             entry ? entry->screenshot : "");
    snprintf(g_want.video_url, sizeof(g_want.video_url), "%s",
             entry ? entry->video : "");
    snprintf(g_want.sound_url, sizeof(g_want.sound_url), "%s",
             entry ? entry->sound : "");
    g_want.installed = entry && entry->state != APP_NOT_INSTALLED;
    g_want.file[0] = '\0';
    g_nothing = !entry;
    /* The sound goes with the pictures: out now, over its fade, whether
       or not the next row has one. */
    audio_sound_stop();
    /* Nothing of the last entry stays on the card: the pictures may still
       exist, they are just not drawn until the thread has this one. */
    g_still_pub = 0;
    g_still_state = STILL_NONE;
    g_still_alpha = 0.0f;
    g_film_state = FILM_NONE;
    g_film_alpha = 0.0f;
    g_decoded = 0;
    g_shown_ms = now_ms();
    g_immediate = immediately;
    g_shown_gen = 0;
}

void preview_show_file(const char *path, enum preview_file kind) {
    /* The id is the request's identity; a path is as good a one as any,
       and cannot be a package's. */
    char id[96];
    snprintf(id, sizeof(id), "file:%s", strlen(path) > 90 ? path + strlen(path) - 90 : path);
    if (strcmp(g_want.id, id) == 0) return;
    snprintf(g_want.id, sizeof(g_want.id), "%s", id);
    g_want.cached_only = 1;
    g_want.shot_url[0] = g_want.video_url[0] = g_want.sound_url[0] = '\0';
    g_want.installed = 0;
    snprintf(g_want.file, sizeof(g_want.file), "%s", path);
    g_want.file_kind = kind;
    g_nothing = 0;
    audio_sound_stop();
    g_still_pub = 0;
    g_still_state = STILL_NONE;
    g_still_alpha = 0.0f;
    g_film_state = FILM_NONE;
    g_film_alpha = 0.0f;
    g_decoded = 0;
    g_shown_ms = now_ms();
    g_immediate = 1;
    g_shown_gen = 0;
}

int preview_film_failed(void) { return g_film_state == FILM_FAILED; }

void preview_tick(void) {
    if (g_nothing) return;
    /* The request goes out once the cursor has rested; the thread does
       the rest and this only picks up what it has finished. */
    if (g_shown_gen == 0) {
        if (!g_immediate && !expired(g_shown_ms, SETTLE_MS)) return;
        g_shown_gen = ++g_want.gen;
        wake();
        return;
    }
    if (g_still_state == STILL_READY && g_still_pub)
        g_still_alpha += (1.0f - g_still_alpha) * 0.12f;

    if (g_film_state == FILM_PLAYING) {
        void *picture = player_take();
        if (picture) { g_film.pixels = picture; g_decoded++; }
        if (g_decoded > 0) g_film_alpha += (1.0f - g_film_alpha) * 0.08f;
        if (player_failed()) g_film_state = FILM_FAILED;
    }
    return;
}

void preview_poke(void) { wake(); }

const struct gfx_texture *preview_still(int *alpha) {
    const struct gfx_texture *t = g_still_pub;
    /* Held back from nothing any more: the still is PIC1.PNG, the picture an
       EBOOT carries to stand behind the screen, and that is where it goes.
       The film plays on the row and the two are never in the same place, so
       neither waits for the other. */
    if (g_still_state != STILL_READY || !t) { *alpha = 0; return 0; }
    *alpha = (int)(g_still_alpha * 255.0f);
    return t;
}

const struct gfx_texture *preview_film(int *alpha) {
    if (g_film_state != FILM_PLAYING || g_decoded == 0) { *alpha = 0; return 0; }
    *alpha = (int)(g_film_alpha * 255.0f);
    return &g_film;
}

int preview_playing(void) {
    return g_film_state == FILM_PLAYING && g_decoded > 0;
}

enum preview_state preview_state(void) {
    if (g_nothing) return PREVIEW_MISSING;
    if (g_still_state == STILL_READY || g_film_state == FILM_PLAYING) return PREVIEW_SHOWING;
    if (g_still_state == STILL_FAILED) return PREVIEW_MISSING;
    if (g_shown_gen != 0) return PREVIEW_LOADING;
    return PREVIEW_EMPTY;
}

int preview_settled(void) {
    /* Nothing asked for is nothing to wait for. */
    if (g_nothing) return 1;
    if (g_shown_gen == 0 || g_done_gen != g_shown_gen) return 0;
    int still = g_still_state == STILL_FAILED ||
                (g_still_state == STILL_READY && g_still_alpha > 0.98f);
    int film = g_film_state == FILM_FAILED || g_film_state == FILM_NONE ||
               (g_film_state == FILM_PLAYING && g_decoded > 0 && g_film_alpha > 0.98f);
    return still && film;
}
