#include "util/storage.h"
#include <pspiofilemgr.h>
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

static unsigned char g_buf[ASSET_MAX];
static size_t g_len;

/* The name a file gets when only the id names it, and the word the log
   uses for the kind. A served name keeps whatever it came with: a film is
   cached as the .pmf or .mp4 it was served as, and the bytes say which it
   is, not the name. */
static const char *EXT[] = { [ASSET_ICON] = "icon.png", [ASSET_SHOT] = "png",
                             [ASSET_VIDEO] = "mp4", [ASSET_SOUND] = "at3" };

static int sink(void *ctx, const void *data, size_t len) {
    (void)ctx;
    if (g_len + len > sizeof(g_buf)) return -1;
    memcpy(g_buf + g_len, data, len);
    g_len += len;
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

static size_t read_file(const char *path) {
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, g_buf, sizeof(g_buf));
    sceIoClose(fd);
    return n > 0 ? (size_t)n : 0;
}

/* By the served name when there is one, and only that: a file the id
   names is what a stick kept before the names carried hashes, and reading
   it back would be keeping the stale picture this is here to replace.
   Without a URL the id is all there is, and that is where the rig plants
   a clip. */
static size_t cache_read(enum asset_kind kind, const char *id, const char *url) {
    char path[256];
    cache_path(kind, id, url, path, sizeof(path));
    return read_file(path);
}

static void cache_write(enum asset_kind kind, const char *id, const char *url) {
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
    if (storage_write_cache(path, g_buf, g_len) == 0 && ++written % 64 == 0)
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

void asset_flush(void) {
    if (!g_hold_n)
        return;
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

static size_t pack_read(const char *name) {
    int i = pack_find(name);
    if (i < 0)
        return 0;
    if (g_pack[i].off >= g_pack_size) {
        memcpy(g_buf, g_hold + (g_pack[i].off - g_pack_size), g_pack[i].len);
        return g_pack[i].len;
    }
    int fd = sceIoOpen(PACK_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0)
        return 0;
    size_t got = 0;
    if (sceIoLseek(fd, g_pack[i].off, PSP_SEEK_SET) == (SceOff)g_pack[i].off) {
        int r = sceIoRead(fd, g_buf, g_pack[i].len);
        got = r == (int)g_pack[i].len ? (size_t)r : 0;
    }
    sceIoClose(fd);
    return got;
}

static void pack_write(const char *name, const void *data, size_t len) {
    pack_load();
    if (!g_hold && !(g_hold = malloc(PACK_HOLD)))
        return;
    if (len > PACK_HOLD)
        return;
    if (g_hold_n == PACK_BATCH || g_hold_len + len > PACK_HOLD)
        asset_flush();
    if (g_pack_size + g_hold_len + len > PACK_MAX) {
        asset_flush();
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

int asset_have(enum asset_kind kind, const char *id, const char *url) {
    char name[256];
    cache_path(kind, id, url, name, sizeof(name));
    return pack_find(name) >= 0;
}

void asset_forget(void) {
    g_hold_len = 0;
    g_hold_n = 0;
    sceIoRemove(PACK_INDEX);
    sceIoRemove(PACK_PATH);
    free(g_pack);
    g_pack = NULL;
    g_pack_n = g_pack_cap = 0;
    g_pack_size = 0;
    g_pack_read = 0;
}

static int packed(enum asset_kind kind) { return kind == ASSET_ICON || kind == ASSET_SHOT; }

const void *asset_fetch(enum asset_kind kind, const char *id, const char *url,
                        int cached_only, size_t *len) {
    if (!id) return 0;

    char name[256];
    cache_path(kind, id, url, name, sizeof(name));
    g_len = packed(kind) ? pack_read(name) : 0;
    /* A file of its own is what a stick kept before the pack, and what the
       rig plants: read when the pack has nothing, but for a picture only
       without a network to fetch it from -- a miss in a crowded directory
       costs as much as a download. */
    if (!g_len && (!packed(kind) || cached_only))
        g_len = cache_read(kind, id, url);
    if (g_len) {
        if (kind != ASSET_ICON)
            logline("%s: %lu bytes cached, %s", EXT[kind], (unsigned long)g_len, id);
        *len = g_len;
        return g_buf;
    }
    if (cached_only || !url || !url[0]) return 0;

    g_len = 0;
    struct https_result result;
    unsigned t0 = now_ms();
    int rc = https_get(url, sink, 0, 0, 0, &result);
    unsigned t1 = now_ms();
    if (rc != 0 || result.status != 200 || g_len == 0) {
        logline("%s: rc=%d status=%ld, %s", EXT[kind], rc, result.status, id);
        return 0;
    }
    if (packed(kind))
        pack_write(name, g_buf, g_len);
    else
        cache_write(kind, id, url);
    logline("%s: %lu bytes fetched in %u ms, cached in %u ms, %s", EXT[kind], (unsigned long)g_len,
            t1 - t0, now_ms() - t1, id);
    *len = g_len;
    return g_buf;
}
