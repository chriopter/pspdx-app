#include "util/storage.h"
#include <pspiofilemgr.h>
#include <stdio.h>
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
    /* The parents belong to the installer and usually exist already; making
       them again is cheaper than asking. */
    sceIoMkdir(storage_path("PSP"), 0777);
    sceIoMkdir(storage_path("PSP/PSPDX"), 0777);
    sceIoMkdir(CACHE_DIR, 0777);
    char path[256];
    cache_path(kind, id, url, path, sizeof(path));
    if(storage_write(path,g_buf,g_len)==0)
        storage_trim_cache(CACHE_DIR,32u*1024u*1024u,path);
}

const void *asset_fetch(enum asset_kind kind, const char *id, const char *url,
                        int cached_only, size_t *len) {
    if (!id) return 0;

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
    int rc = https_get(url, sink, 0, 0, 0, &result);
    if (rc != 0 || result.status != 200 || g_len == 0) {
        logline("%s: rc=%d status=%ld, %s", EXT[kind], rc, result.status, id);
        return 0;
    }
    cache_write(kind, id, url);
    logline("%s: %lu bytes fetched, %s", EXT[kind], (unsigned long)g_len, id);
    *len = g_len;
    return g_buf;
}
