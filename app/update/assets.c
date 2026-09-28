#include "util/storage.h"
#include <pspiofilemgr.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update/assets.h"
#include "pspkit-https/https.h"
#include "util/runtime.h"

#define CACHE_DIR storage_path("PSP/PSPDX/CACHE/media")

/* A screen-sized PNG is under 100 KB; ten seconds of video at 600 kbit land
   around 750. One and a half megabytes is well above both and still refuses
   a runaway body. */
#define ASSET_MAX (1536 * 1024)

/* One buffer for icons and one for the rest: the list's icons are fetched
   on a thread of their own, beside the card's pictures, films and sounds.
   An icon is 144x80 and some ten kilobytes; one past 256 KB is not an icon
   worth waiting for, and is refused as it arrives rather than downloaded
   whole. */
#define ICON_MAX (256 * 1024)
struct asset_buf {
    unsigned char *p;
    size_t len, cap;
};
static unsigned char g_media_mem[ASSET_MAX], g_icon_mem[ICON_MAX];
static struct asset_buf g_media = {g_media_mem, 0, ASSET_MAX}, g_icons = {g_icon_mem, 0, ICON_MAX};
static struct asset_buf *buf_for(enum asset_kind kind) {
    return kind == ASSET_ICON ? &g_icons : &g_media;
}
/* The pack is shared by both threads. */
static pthread_mutex_t g_pack_lock = PTHREAD_MUTEX_INITIALIZER;

/* The name a file gets when only the id names it, and the word the log
   uses for the kind. A served name keeps whatever it came with: a film is
   cached as the .pmf or .mp4 it was served as, and the bytes say which it
   is, not the name. */
static const char *EXT[] = { [ASSET_ICON] = "icon.png", [ASSET_SHOT] = "png",
                             [ASSET_VIDEO] = "mp4", [ASSET_SOUND] = "at3" };

static int sink(void *ctx, const void *data, size_t len) {
    struct asset_buf *b = ctx;
    if (b->len + len > b->cap) return -1;
    memcpy(b->p + b->len, data, len);
    b->len += len;
    return 0;
}

/* The cache is keyed by the name the catalog serves the file under. The
   catalog puts a hash of the bytes into that name, so a changed picture
   arrives under a new name and the old one is simply never asked for
   again; keyed by id, a stick kept the first icon it ever saw for good.
   Without a URL -- the rig planting a clip, an entry that links nothing --
   the id names the file, which is also where older sticks kept theirs. */
static void cache_path(enum asset_kind kind, const char *id, const char *url,
                       char *out, size_t size) {
    const char *base = url && url[0] ? strrchr(url, '/') : 0;
    if (base && base[1]) {
        base++;
        /* A name is a name: only what a file on the stick may be called,
           and only as much of it as leaves the whole path within the 255
           characters a path is given, whatever the length of the id. */
        char safe[96];
        size_t n = 0, dir = strlen(CACHE_DIR) + 1 + strlen(id) + 1;
        size_t room = dir < 255 ? 255 - dir : 0;
        if (room > sizeof(safe) - 1)
            room = sizeof(safe) - 1;
        for (const char *p = base; *p && n < room; p++) {
            char c = *p;
            int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
            safe[n++] = ok ? c : '_';
        }
        safe[n] = '\0';
        /* Served names are not unique on their own: at the origin the name
           is Sony's -- every app's picture is called ICON0.PNG -- and a
           catalog keeps each app's files in a directory of its own, where
           they are called icon-<sha8>.png and the like. Either way the id
           goes in front unless the name already carries it, which is what
           the catalog served before it had a directory an app. The hash
           stays in the name, so a changed picture still arrives as a file
           this stick has never seen. */
        if (strncmp(safe, id, strlen(id)) != 0)
            snprintf(out, size, "%s/%s-%s", CACHE_DIR, id, safe);
        else
            snprintf(out, size, "%s/%s", CACHE_DIR, safe);
        return;
    }
    snprintf(out, size, "%s/%s.%s", CACHE_DIR, id, EXT[kind]);
}

static size_t read_file(const char *path, struct asset_buf *b) {
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, b->p, b->cap);
    sceIoClose(fd);
    return n > 0 ? (size_t)n : 0;
}

/* By the served name when there is one, and only that: a file the id
   names is what a stick kept before the names carried hashes, and reading
   it back would be keeping the stale picture this is here to replace.
   Without a URL the id is all there is, and that is where the rig plants
   a clip. */
static size_t cache_read(enum asset_kind kind, const char *id, const char *url,
                         struct asset_buf *b) {
    char path[256];
    cache_path(kind, id, url, path, sizeof(path));
    return read_file(path, b);
}

static void cache_write(enum asset_kind kind, const char *id, const char *url,
                        const struct asset_buf *b) {
    /* The parents belong to the installer and usually exist already; made
       once a run, not before every file. */
    static int made;
    if (!made) {
        sceIoMkdir(storage_path("PSP"), 0777);
        sceIoMkdir(storage_path("PSP/PSPDX"), 0777);
        sceIoMkdir(storage_path("PSP/PSPDX/CACHE"), 0777);
        sceIoMkdir(CACHE_DIR, 0777);
        made = 1;
    }
    char path[256];
    cache_path(kind, id, url, path, sizeof(path));
    /* The limit is held to every 64th file written, not every one: holding
       it reads the whole directory back, and with hundreds of pictures in
       it that took most of three seconds per icon. In between the cache can
       run over by 64 files, a megabyte or two. */
    static unsigned written;
    if (storage_write_cache(path, b->p, b->len) == 0 && ++written % 64 == 0)
        storage_trim_cache(CACHE_DIR, 32u * 1024u * 1024u, path);
}

/* ------------------------------------------------------------- the pack

   Icons and stills are small and many: a file each put hundreds of names
   in one FAT directory, and the console's FAT searches a directory from
   the start to open a file and again to make one -- two seconds to write
   an icon of a kilobyte, a tenth of one to read it back. So they go into
   one file instead, appended one after the other, with a little index
   beside it of which name is where; both in PSP/PSPDX/CACHE, which holds a
   handful of names. The index is read once a run. A record whose bytes run
   past the end of the pack -- a write a power cut stopped -- is dropped
   when it is read. Past PACK_MAX the pack starts over rather than being
   trimmed: a picture fetched again costs a fraction of a second. */
#define PACK_PATH storage_path("PSP/PSPDX/CACHE/media.pak")
#define PACK_INDEX storage_path("PSP/PSPDX/CACHE/media.idx")
#define PACK_MAX (24u * 1024u * 1024u)
struct pack_entry {
    unsigned long long key;
    unsigned off, len;
};
static struct pack_entry *g_pack;
static int g_pack_n, g_pack_cap, g_pack_read;
static unsigned g_pack_size;

static unsigned long long pack_key(const char *name) {
    unsigned long long h = 1469598103934665603ULL;
    for (; *name; name++)
        h = (h ^ (unsigned char)*name) * 1099511628211ULL;
    return h;
}

static int pack_add(unsigned long long key, unsigned off, unsigned len) {
    if (g_pack_n == g_pack_cap) {
        int cap = g_pack_cap ? g_pack_cap * 2 : 256;
        struct pack_entry *grown = realloc(g_pack, (size_t)cap * sizeof(*g_pack));
        if (!grown)
            return -1;
        g_pack = grown;
        g_pack_cap = cap;
    }
    g_pack[g_pack_n++] = (struct pack_entry){key, off, len};
    return 0;
}

static void pack_load(void) {
    if (g_pack_read)
        return;
    g_pack_read = 1;
    int fd = sceIoOpen(PACK_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0)
        return;
    SceOff end = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoClose(fd);
    g_pack_size = end > 0 ? (unsigned)end : 0;
    char *raw = NULL;
    int n = storage_read(PACK_INDEX, &raw, 4 * 1024 * 1024);
    for (int at = 0; raw && at + 16 <= n; at += 16) {
        struct pack_entry e;
        memcpy(&e.key, raw + at, 8);
        memcpy(&e.off, raw + at + 8, 4);
        memcpy(&e.len, raw + at + 12, 4);
        if (e.len && e.len <= ASSET_MAX && e.off <= g_pack_size && e.len <= g_pack_size - e.off)
            pack_add(e.key, e.off, e.len);
    }
    free(raw);
}

/* Written in batches: a picture is kept in memory first, readable from
   there at once, and goes to the stick with the ones after it -- every
   PACK_BATCH pictures, PACK_HOLD bytes, or when asset_flush says the
   thread is idle. A write costs an open and a close of two files whatever
   its size, so twenty pictures cost what one did. A power cut loses what
   was held, which is fetched again. */
#define PACK_BATCH 20
#define PACK_HOLD (384u * 1024u)
static unsigned char *g_hold;
static unsigned g_hold_len;
static unsigned char g_hold_rec[PACK_BATCH * 16];
static int g_hold_n;

static int g_pack_rfd = -1;
static void pack_close_read(void) {
    if (g_pack_rfd >= 0) sceIoClose(g_pack_rfd);
    g_pack_rfd = -1;
}

static void flush_locked(void);
void asset_flush(void) {
    pthread_mutex_lock(&g_pack_lock);
    flush_locked();
    pthread_mutex_unlock(&g_pack_lock);
}
static void flush_locked(void) {
    if (!g_hold_n)
        return;
    pack_close_read();
    int fd = sceIoOpen(PACK_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    int ok = fd >= 0 && sceIoWrite(fd, g_hold, g_hold_len) == (int)g_hold_len;
    if (fd >= 0)
        sceIoClose(fd);
    if (ok) {
        fd = sceIoOpen(PACK_INDEX, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        ok = fd >= 0 && sceIoWrite(fd, g_hold_rec, (unsigned)g_hold_n * 16) == g_hold_n * 16;
        if (fd >= 0)
            sceIoClose(fd);
    }
    if (ok) {
        g_pack_size += g_hold_len;
    } else {
        /* What the stick did not take is forgotten, and fetched again. */
        g_pack_n -= g_hold_n;
        logline("cache: %d pictures could not be written", g_hold_n);
    }
    g_hold_len = 0;
    g_hold_n = 0;
}

/* The newest record of a name wins, so the search runs from the end; one
   past what the pack holds on the stick is still in the batch. */
static int pack_find(const char *name) {
    pack_load();
    unsigned long long key = pack_key(name);
    for (int i = g_pack_n - 1; i >= 0; i--)
        if (g_pack[i].key == key)
            return i;
    return -1;
}

static size_t pack_read_locked(const char *name, unsigned char *out, size_t cap) {
    int i = pack_find(name);
    if (i < 0 || g_pack[i].len > cap)
        return 0;
    if (g_pack[i].off >= g_pack_size) {
        memcpy(out, g_hold + (g_pack[i].off - g_pack_size), g_pack[i].len);
        return g_pack[i].len;
    }
    /* The pack stays open for reading between pictures; a write closes it,
       so that what was appended is seen by the next open. */
    if (g_pack_rfd < 0 && (g_pack_rfd = sceIoOpen(PACK_PATH, PSP_O_RDONLY, 0777)) < 0)
        return 0;
    size_t got = 0;
    if (sceIoLseek(g_pack_rfd, g_pack[i].off, PSP_SEEK_SET) == (SceOff)g_pack[i].off) {
        int r = sceIoRead(g_pack_rfd, out, g_pack[i].len);
        got = r == (int)g_pack[i].len ? (size_t)r : 0;
    }
    return got;
}
static size_t pack_read(const char *name, unsigned char *out, size_t cap) {
    pthread_mutex_lock(&g_pack_lock);
    size_t n = pack_read_locked(name, out, cap);
    pthread_mutex_unlock(&g_pack_lock);
    return n;
}

static void pack_write_locked(const char *name, const void *data, size_t len);
static void pack_write(const char *name, const void *data, size_t len) {
    pthread_mutex_lock(&g_pack_lock);
    pack_write_locked(name, data, len);
    pthread_mutex_unlock(&g_pack_lock);
}
static void pack_write_locked(const char *name, const void *data, size_t len) {
    pack_load();
    if (!g_hold && !(g_hold = malloc(PACK_HOLD)))
        return;
    if (len > PACK_HOLD)
        return;
    if (g_hold_n == PACK_BATCH || g_hold_len + len > PACK_HOLD)
        flush_locked();
    if (g_pack_size + g_hold_len + len > PACK_MAX) {
        flush_locked();
        pack_close_read();
        sceIoRemove(PACK_INDEX);
        sceIoRemove(PACK_PATH);
        g_pack_n = 0;
        g_pack_size = 0;
        logline("cache: the picture pack was full and starts over");
    }
    unsigned off = g_pack_size + g_hold_len;
    unsigned long long key = pack_key(name);
    unsigned l = (unsigned)len;
    if (pack_add(key, off, l) < 0)
        return;
    memcpy(g_hold + g_hold_len, data, len);
    g_hold_len += l;
    unsigned char *rec = g_hold_rec + g_hold_n * 16;
    memcpy(rec, &key, 8);
    memcpy(rec + 8, &off, 4);
    memcpy(rec + 12, &l, 4);
    g_hold_n++;
}

static void thumb_name(const char *id, const char *url, char *out, size_t size) {
    char name[256];
    cache_path(ASSET_ICON, id, url, name, sizeof(name));
    snprintf(out, size, "%s#thumb", name);
}

size_t asset_thumb_get(const char *id, const char *url, void *out, size_t cap) {
    char name[272];
    thumb_name(id, url, name, sizeof(name));
    return pack_read(name, out, cap);
}

void asset_thumb_put(const char *id, const char *url, const void *data, size_t len) {
    char name[272];
    thumb_name(id, url, name, sizeof(name));
    pack_write(name, data, len);
}

int asset_thumb_have(const char *id, const char *url) {
    char name[272];
    thumb_name(id, url, name, sizeof(name));
    pthread_mutex_lock(&g_pack_lock);
    int have = pack_find(name) >= 0;
    pthread_mutex_unlock(&g_pack_lock);
    return have;
}

int asset_have(enum asset_kind kind, const char *id, const char *url) {
    char name[256];
    cache_path(kind, id, url, name, sizeof(name));
    pthread_mutex_lock(&g_pack_lock);
    int have = pack_find(name) >= 0;
    pthread_mutex_unlock(&g_pack_lock);
    return have;
}

void asset_forget(void) {
    pthread_mutex_lock(&g_pack_lock);
    pack_close_read();
    g_hold_len = 0;
    g_hold_n = 0;
    sceIoRemove(PACK_INDEX);
    sceIoRemove(PACK_PATH);
    free(g_pack);
    g_pack = NULL;
    g_pack_n = g_pack_cap = 0;
    g_pack_size = 0;
    g_pack_read = 0;
    pthread_mutex_unlock(&g_pack_lock);
}

static int packed(enum asset_kind kind) { return kind == ASSET_ICON || kind == ASSET_SHOT; }

const void *asset_fetch(enum asset_kind kind, const char *id, const char *url,
                        int cached_only, size_t *len) {
    if (!id) return 0;
    struct asset_buf *b = buf_for(kind);

    char name[256];
    cache_path(kind, id, url, name, sizeof(name));
    b->len = packed(kind) ? pack_read(name, b->p, b->cap) : 0;
    /* A file of its own is what a stick kept before the pack, and what the
       rig plants: read when the pack has nothing, but for a picture only
       without a network to fetch it from -- a miss in a crowded directory
       costs as much as a download. */
    if (!b->len && (!packed(kind) || cached_only))
        b->len = cache_read(kind, id, url, b);
    if (b->len) {
        if (kind != ASSET_ICON)
            logline("%s: %lu bytes cached, %s", EXT[kind], (unsigned long)b->len, id);
        *len = b->len;
        return b->p;
    }
    if (cached_only || !url || !url[0]) return 0;

    b->len = 0;
    struct https_result result;
    unsigned t0 = now_ms();
    int rc = https_get(url, sink, b, 0, 0, &result);
    unsigned t1 = now_ms();
    if (rc != 0 || result.status != 200 || b->len == 0) {
        logline("%s: rc=%d status=%ld%s, %s", EXT[kind], rc, result.status,
                b->len == b->cap ? " (too large)" : "", id);
        return 0;
    }
    /* An icon is kept by its caller, shrunk, as a thumbnail; the PNG it
       came as is not needed again. */
    if (kind == ASSET_ICON)
        ;
    else if (packed(kind))
        pack_write(name, b->p, b->len);
    else
        cache_write(kind, id, url, b);
    logline("%s: %lu bytes fetched in %u ms, %s", EXT[kind], (unsigned long)b->len, t1 - t0, id);
    *len = b->len;
    return b->p;
}
