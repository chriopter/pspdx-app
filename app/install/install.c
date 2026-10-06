#include "text.h"
#include "install/state.h"
#include "update/pspdx.h"
#include "update/sources.h"
#include "util/storage.h"
#include <stdarg.h>
#include <strings.h>
/*
 * Installing a package: download, check, unpack, rename.
 *
 * FAT32 has no transactions and PSP users pull the battery, so nothing is
 * written over anything: a first install is unpacked into a staging
 * directory under PSP/GAME and renamed into place, and an update is unpacked
 * beside the files of the app's folder, each new file under a name of its
 * own, and then swapped in file by file, the file it replaces stepping aside
 * until the journal says committed. What the release does not ship --
 * settings, saves a homebrew keeps next to its EBOOT -- is never touched. A
 * removal renames the folder to <dir>.old and deletes that once committed.
 *
 * Only the PSP/GAME/<dir>/ subtree of the archive is installed. Everything
 * beside it -- PSP/SYSTEM configs, LICENSES/, a build.json -- is the user's or
 * nobody's, and is never written.
 *
 * A plugin is the one exception to PSP/GAME: its one .prx goes to
 * seplugins/ on the device, the same way -- under a name of its own, then
 * renamed -- and, once it is turned on, one line of the PLUGINS.TXT there
 * is its own. Nothing else under seplugins/ is written, removed or renamed,
 * and the list is only ever written in place: see "plugin" below.
 */

#include <cjson/cJSON.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "install/install.h"
#include "install/pluginlist.h"
#include "install/zipread.h"
#include "update/catalog.h"
#include "util/runtime.h"

#define TMP_DIR storage_path("PSP/PSPDX/TMP")
/* Only the serialized installer uses this context; browser/data paths never change. */
static char target_device[5];
static char game_dir[32], stage_dir[48];
static const char *target(void) { return *target_device ? target_device : storage_device(); }
static void target_set(const char *dev) {
    snprintf(target_device, sizeof(target_device), "%s", dev);
    snprintf(game_dir, sizeof(game_dir), "%s/PSP/GAME", dev);
    snprintf(stage_dir, sizeof(stage_dir), "%s/PSP/GAME/.pspdx-stage", dev);
}
#define GAME_DIR (*game_dir ? game_dir : storage_path("PSP/GAME"))
#define ARCHIVE storage_path("PSP/PSPDX/TMP/download.zip")
#define STAGE (*stage_dir ? stage_dir : storage_path("PSP/GAME/.pspdx-stage"))
#define JOURNAL storage_path("PSP/PSPDX/TMP/transaction.json")
#define CLEANUP storage_path("PSP/PSPDX/TMP/cleanup.txt")

/* A Memory Stick tops out at 32 GB and no homebrew is anywhere near this.
   The number exists so that a size field cannot ask for something absurd. */
/* -------------------------------------------------------------- release */

/* An id becomes a file name, so it may not carry a path. Reverse-DNS letters,
   digits, dot, dash and underscore only. */
int manifest_id_is_safe(const char *id) {
    if (!id || !*id || strlen(id) >= PSPDX_ID_SIZE)
        return 0;
    for (const char *p = id; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                 *p == '.' || *p == '-' || *p == '_';
        if (!ok)
            return 0;
    }
    if (strstr(id, ".."))
        return 0;
    return 1;
}

/* Attacker-controlled doubles: out of range, the conversion to an integer
   is undefined rather than merely wrong. */
int manifest_rev_in_range(double rev) {
    return rev >= 0 && rev <= 4294967295.0 && rev == (unsigned)rev;
}

int manifest_size_in_range(double size) {
    return size > 0 && size <= MAX_PACKAGE_BYTES && size == (size_t)size;
}

int manifest_has_sha256(const struct manifest *m) {
    for (int i = 0; i < 32; i++)
        if (m->sha256[i])
            return 1;
    return 0;
}

int manifest_keep_raw(struct manifest *m, const char *text, size_t len) {
    char *copy = malloc(len + 1);
    manifest_forget(m);
    if (!copy)
        return -1;
    memcpy(copy, text, len);
    copy[len] = '\0';
    m->raw = copy;
    return 0;
}

void manifest_forget(struct manifest *m) {
    free(m->raw);
    m->raw = NULL;
}

int manifest_dir_is_safe(const char *dir) {
    char path[64];
    if (!dir || strlen(dir) > 32)
        return 0;
    snprintf(path, sizeof(path), "PSP/GAME/%s", dir);
    return pspdx_install_dir(path);
}

int manifest_plugin_is_safe(const char *name) {
    size_t n = name ? strlen(name) : 0;
    return n > 4 && n <= 32 && name[0] != '.' && !strcasecmp(name + n - 4, ".prx") &&
           strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == n;
}

/* ------------------------------------------------------------- download */

/* Set by install_abort, from any thread: every loop below checks it. */
static volatile int g_abort;

/* A download arrives in pieces of a few kilobytes, and storage written a
   piece at a time is the slowest part of it: a PSP Go's own flash took
   1.1 MB/s that way and 2.6 MB/s in blocks, with the cable delivering 4.8.
   So the pieces are gathered and written a block at a time; where there is
   no memory for the block they are written as they come. */
#define DL_BLOCK (256 * 1024)

struct dl {
    int fd;
    wc_Sha256 sha;
    size_t written, expected;
    unsigned char *block;
    size_t held;
};

static int dl_write(struct dl *d, const void *data, size_t len) {
    while (len) {
        int n = sceIoWrite(d->fd, data, len);
        if (n <= 0) {
            logline("write failed %d", n);
            return -1;
        }
        data = (const char *)data + n;
        len -= (size_t)n;
    }
    return 0;
}

/* What is still held goes to the file: before it is closed. */
static int dl_flush(struct dl *d) {
    size_t held = d->held;
    d->held = 0;
    return held ? dl_write(d, d->block, held) : 0;
}

static int file_sink(void *ctx, const void *data, size_t len) {
    struct dl *d = ctx;
    if (len > d->expected - d->written)
        return -1;
    if (wc_Sha256Update(&d->sha, data, (word32)len) != 0)
        return -1;
    d->written += len;
    if (!d->block)
        return dl_write(d, data, len);
    while (len) {
        size_t k = len < DL_BLOCK - d->held ? len : DL_BLOCK - d->held;
        memcpy(d->block + d->held, data, k);
        d->held += k;
        data = (const char *)data + k;
        len -= k;
        if (d->held == DL_BLOCK && dl_flush(d) < 0)
            return -1;
    }
    return 0;
}

/* got is the SHA-256 of what arrived, whether or not the release named one,
   so that the record can say which zip is on the stick. */
static install_layout_cb g_layout_check;
void install_set_layout_check(install_layout_cb check) { g_layout_check = check; }

/* The first reason an install gave up, for its report: set where the cause
   is known, read out when install_release_to returns. */
static char g_why[64];
static void why(const char *fmt, ...) {
    if (g_why[0])
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_why, sizeof(g_why), fmt, ap);
    va_end(ap);
}

/* One attempt: the archive written afresh from url. The https result says
   how the server answered; the return is https_get's. */
static int fetch_archive(const char *url, struct dl *d, https_progress progress, void *pctx,
                         struct https_result *r, int *close_rc) {
    memset(r, 0, sizeof(*r));
    d->written = 0;
    *close_rc = 0;
    if (wc_InitSha256(&d->sha) != 0)
        return -1;
    d->fd = sceIoOpen(ARCHIVE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (d->fd < 0) {
        logline("cannot create %s", ARCHIVE);
        *close_rc = -1;
        return -1;
    }
    unsigned start = now_ms();
    d->block = malloc(DL_BLOCK);
    d->held = 0;
    int rc = https_get(url, file_sink, d, progress, pctx, r);
    int flushed = dl_flush(d);
    free(d->block);
    d->block = NULL;
    *close_rc = sceIoClose(d->fd);
    if (flushed < 0 && *close_rc >= 0)
        *close_rc = -1;
    unsigned ms = now_ms() - start;
    logline("download: rc=%d status=%ld %lu bytes in %u.%us", rc, r->status,
            (unsigned long)d->written, ms / 1000, (ms % 1000) / 100);
    return rc;
}

/* archive.org answers a download with a redirect to a mirror, and some
   mirrors answer 500 for files the item's own storage servers hold. The
   item's metadata names those servers (d1, d2) and its folder on them
   (dir): the file is then asked of each in turn, under the same name. */
struct text_buf {
    char *p;
    size_t n, cap;
};
static int text_sink(void *ctx, const void *data, size_t len) {
    struct text_buf *b = ctx;
    if (b->n + len + 1 > b->cap)
        return -1;
    memcpy(b->p + b->n, data, len);
    b->n += len;
    b->p[b->n] = 0;
    return 0;
}
#define ARCHIVE_ORG "https://archive.org/download/"
static int archive_mirrors(const char *url, char out[2][512]) {
    out[0][0] = out[1][0] = 0;
    if (strncmp(url, ARCHIVE_ORG, sizeof(ARCHIVE_ORG) - 1))
        return 0;
    const char *item = url + sizeof(ARCHIVE_ORG) - 1;
    const char *file = strchr(item, '/');
    if (!file || file == item)
        return 0;
    char meta[256];
    snprintf(meta, sizeof(meta), "https://archive.org/metadata/%.*s", (int)(file - item), item);
    struct text_buf b = {malloc(512 * 1024), 0, 512 * 1024};
    struct https_result r;
    int found = 0;
    if (b.p && https_get(meta, text_sink, &b, NULL, NULL, &r) == 0 && r.status == 200) {
        cJSON *j = cJSON_Parse(b.p);
        cJSON *dir = cJSON_GetObjectItemCaseSensitive(j, "dir");
        const char *host[2] = {"d1", "d2"};
        for (int i = 0; i < 2 && cJSON_IsString(dir); i++) {
            cJSON *h = cJSON_GetObjectItemCaseSensitive(j, host[i]);
            /* A host name and a path, nothing that could lead anywhere else. */
            if (cJSON_IsString(h) && strspn(h->valuestring, "abcdefghijklmnopqrstuvwxyz0123456789.-") ==
                                         strlen(h->valuestring) &&
                strstr(h->valuestring, ".archive.org") && dir->valuestring[0] == '/' &&
                !strstr(dir->valuestring, "..")) {
                snprintf(out[found], 512, "https://%s%s%s", h->valuestring, dir->valuestring, file);
                found++;
            }
        }
        cJSON_Delete(j);
    }
    free(b.p);
    logline("download: archive.org metadata named %d storage server%s", found, found == 1 ? "" : "s");
    return found;
}

static int download(const struct manifest *m, https_progress progress, void *pctx,
                    unsigned char *got) {
    struct dl d;
    struct https_result r;
    int close_rc;
    d.expected = m->size;
    int rc = fetch_archive(m->url, &d, progress, pctx, &r, &close_rc);
    if (!g_abort && close_rc >= 0 && r.status >= 500 && r.status < 600) {
        char mirror[2][512];
        int n = archive_mirrors(m->url, mirror);
        for (int i = 0; i < n && !g_abort; i++) {
            logline("download: %ld from the mirror; trying %s", r.status, mirror[i]);
            rc = fetch_archive(mirror[i], &d, progress, pctx, &r, &close_rc);
            if (rc == 0 && r.status == 200 && close_rc >= 0)
                break;
        }
    }
    if (close_rc < 0)
        why(T_WHY_STICK);
    else if (rc != 0 && !r.status)
        why(T_WHY_NO_ANSWER);
    else if (r.status != 200)
        why(T_WHY_STATUS, r.status);
    if (rc != 0 || r.status != 200 || close_rc < 0)
        return close_rc < 0 && !r.status ? -2 : -3;
    if (d.written != m->size) {
        logline("download: size %lu, release says %lu", (unsigned long)d.written,
                (unsigned long)m->size);
        why(T_WHY_SIZE);
        return -4;
    }

    unsigned char digest[32];
    wc_Sha256Final(&d.sha, digest);
    memcpy(got, digest, 32);
    /* No hash is what the origin path gives: the zip came from the
       author's own account over TLS and its size agreed, which is the
       trust there is. A cache that hashed the whole zip is held to it. */
    if (!manifest_has_sha256(m)) {
        logline("download: sha256: none, size checked");
        return 0;
    }
    if (memcmp(digest, m->sha256, 32) != 0) {
        logline("download: sha256 MISMATCH");
        why(T_WHY_HASH);
        return -5;
    }
    logline("download: sha256 ok");
    return 0;
}

/* -------------------------------------------------------------- unpack */

static int mkdir_p(const char *path) {
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 5; *p; p++) { /* skip storage_path("") */
        if (*p == '/') {
            *p = '\0';
            sceIoMkdir(tmp, 0777);
            *p = '/';
        }
    }
    sceIoMkdir(tmp, 0777);
    return 0;
}

/* One size for every path built under PSP/GAME here: what unpack writes
   under the stage and what rm_rf has to name again later under
   "<dir>.old/", the longest form a package's path takes on the stick. */
#define PATH_BUF 256

/* An update lays the release over the app's folder file by file: each file
   the ZIP ships is written beside the one it replaces under this name, and
   that one steps aside under the other until the update is committed. Both
   are PSPDX's alone, so a package may not ship a name that ends in either. */
#define SUFFIX_NEW ".pspdx-new"
#define SUFFIX_OLD ".pspdx-old"

/* Not storage_remove_tree: that one goes on past a file it cannot delete
   and reports at the end, which is right for a reset. An install stops at
   the first, so what it was clearing is left whole enough to put back. */
static int rm_rf(const char *path) {
    SceUID d = sceIoDopen(path);
    if (d < 0)
        return sceIoRemove(path) < 0 ? -1 : 0;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (strcmp(e.d_name, ".") == 0 || strcmp(e.d_name, "..") == 0)
            continue;
        char sub[PATH_BUF];
        if (snprintf(sub, sizeof(sub), "%s/%s", path, e.d_name) >= (int)sizeof(sub)) {
            sceIoDclose(d);
            return -1;
        }
        int rc = FIO_S_ISDIR(e.d_stat.st_mode) ? rm_rf(sub) : sceIoRemove(sub);
        if (rc < 0) {
            sceIoDclose(d);
            return -1;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    return sceIoRmdir(path) < 0 ? -1 : 0;
}

static int safe_relative(const char *rel);

/* Where the package sits inside the archive: install.root out of the
   .pspdx when the author named one, and otherwise the directory of the
   shallowest EBOOT.PBP, which is the rule the catalog's scanner applies
   too. Of sixteen surveyed release archives, ten put the EBOOT one
   directory down, three at the root and two under PSP/GAME/; the root case
   has no directory name of its own and takes the last part of the id.
   root comes back with its trailing slash, or empty; dir is what the
   directory under PSP/GAME will be called -- install.dir when the file
   named one. */
static void slashes(char *name) {
    for (char *p = name; *p; p++)
        if (*p == '\\')
            *p = '/';
}

static int ends_with_eboot(const char *name) {
    size_t n = strlen(name);
    if (n < 9)
        return 0;
    const char *tail = name + n - 9;
    static const char want[] = "EBOOT.PBP";
    for (int i = 0; i < 9; i++) {
        char c = tail[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c != want[i])
            return 0;
    }
    return n == 9 || name[n - 10] == '/';
}

/* A name, a directory's with its slash or not, that ends in one of the two
   an update keeps its own copies under. */
static int ends_with_ours(const char *name) {
    size_t n = strlen(name);
    while (n && name[n - 1] == '/')
        n--;
    size_t k = sizeof(SUFFIX_NEW) - 1;
    return n >= k && (!strncasecmp(name + n - k, SUFFIX_NEW, k) ||
                      !strncasecmp(name + n - k, SUFFIX_OLD, k));
}

/* A Mac's zip carries a shadow of every file under __MACOSX/: never the
   package, never unpacked. */
static int mac_shadow(const char *name) { return !strncmp(name, "__MACOSX/", 9); }

static int find_package(struct zipread *z, const struct manifest *m, char *root, size_t rootsz,
                        char *dir, size_t dirsz, struct install_layout *layout) {
    struct zipentry e;
    if (z->entries > 8192)
        return -1;
    unsigned long long *names = calloc(z->entries ? z->entries : 1, sizeof(*names));
    if (!names || z->entries > 8192) {
        free(names);
        return -1;
    }
    /* The validation pass below uses a hash only to reject ambiguous names;
       collisions reject a package, never permit an overwrite. */
    int used = 0;
    for (int step = zip_first(z, &e); step > 0; step = zip_next(z, &e)) {
        unsigned long long h = 1469598103934665603ULL;
        for (const unsigned char *q = (const unsigned char *)e.name; *q; q++) {
            unsigned c = *q;
            if (c >= 'A' && c <= 'Z')
                c += 32;
            if (c == '\\')
                c = '/';
            h = (h ^ c) * 1099511628211ULL;
        }
        for (int k = 0; k < used; k++)
            if (names[k] == h) {
                free(names);
                return -1;
            }
        if (used >= (int)z->entries) {
            free(names);
            return -1;
        }
        names[used++] = h;
    }
    free(names);
    int rc, count = 0, depth = -1, level = 0;
    root[0] = 0;
    memset(layout, 0, sizeof(*layout));
    if (!manifest_dir_is_safe(m->dir))
        return -1;
    /* The package is the EBOOT.PBP nearest the top; another as near makes
       two apps of equal standing, and which one was meant is not for the
       installer to guess. */
    for (rc = zip_first(z, &e); rc > 0; rc = zip_next(z, &e)) {
        if (e.name_truncated)
            return -1;
        slashes(e.name);
        if (!safe_relative(e.name))
            return -1;
        if (ends_with_ours(e.name)) {
            logline("zip: %s ends in a name PSPDX keeps for itself", e.name);
            return -1;
        }
        if (mac_shadow(e.name) || !ends_with_eboot(e.name))
            continue;
        count++;
        int d = 0;
        for (const char *p = e.name; *p; p++)
            d += *p == '/';
        if (depth >= 0 && d > depth)
            continue;
        if (d == depth) {
            level++;
            continue;
        }
        depth = d;
        level = 1;
        const char *slash = strrchr(e.name, '/');
        size_t n = slash ? (size_t)(slash - e.name + 1) : 0;
        if (n >= rootsz)
            return -1;
        memcpy(root, e.name, n);
        root[n] = 0;
    }
    if (rc < 0 || count == 0) {
        logline("zip: expected an EBOOT.PBP");
        why(rc < 0 ? T_WHY_ZIP : T_WHY_NO_EBOOT);
        return -1;
    }
    if (level > 1) {
        logline("zip: %d EBOOT.PBP, %d of them side by side at the top", count, level);
        why(T_WHY_EBOOTS);
        return -1;
    }
    snprintf(dir, dirsz, "%s", m->dir);
    layout->eboots = count;
    {
        size_t plen = strlen(root);
        for (rc = zip_first(z, &e); rc > 0; rc = zip_next(z, &e)) {
            slashes(e.name);
            int folder = e.name[0] && e.name[strlen(e.name) - 1] == '/';
            if (mac_shadow(e.name)) {
                layout->mac_files += !folder;
                continue;
            }
            if (!strncmp(e.name, root, plen)) {
                if (folder)
                    continue;
                layout->files++;
                layout->bytes += e.usize;
                if (e.name_converted) {
                    if (layout->renamed < 2)
                        snprintf(layout->renamed_path[layout->renamed], 64, "%s", e.name + plen);
                    layout->renamed++;
                    layout->sjis += e.name_converted == CP_SJIS;
                }
                if (ends_with_eboot(e.name) && strlen(e.name) > plen + 9 &&
                    layout->nested++ < LAYOUT_SHOWN)
                    snprintf(layout->nested_path[layout->nested - 1], 64, "%s", e.name + plen);
                continue;
            }
            /* Outside root: grouped by the first part of its path that root
               does not share -- a folder beside the package, or a file. */
            size_t common = 0;
            for (size_t i = 0; root[i] && e.name[i] == root[i]; i++)
                if (root[i] == '/')
                    common = i + 1;
            size_t end = strcspn(e.name + common, "/");
            int whole = e.name[common + end] == '/';
            char group[64];
            snprintf(group, sizeof(group), "%.*s%s", (int)(common + end < 62 ? common + end : 62),
                     e.name, whole ? "/" : "");
            layout->left += ends_with_eboot(e.name);
            if (folder)
                continue;
            layout->left_files++;
            layout->left_bytes += e.usize;
            int g = 0, kept = layout->left_groups < LAYOUT_GROUPS ? layout->left_groups : LAYOUT_GROUPS;
            while (g < kept && strcmp(layout->left_path[g], group))
                g++;
            if (g == kept) {
                layout->left_groups++;
                if (g == LAYOUT_GROUPS)
                    continue;
                snprintf(layout->left_path[g], 64, "%s", group);
            }
            layout->left_count[g]++;
        }
        snprintf(layout->root, sizeof(layout->root), "%s", root);
        layout->review = count > 1 || layout->renamed > 0;
        snprintf(layout->dir, sizeof(layout->dir), "%s", dir);
        if (count > 1)
            logline("zip: %d EBOOT.PBP; %s goes to PSP/GAME/%s, %d under it, %d left out",
                    count, root[0] ? root : "the top", dir, layout->nested, layout->left);
        if (layout->renamed)
            logline("zip: %d names in a code page, %d of them Shift-JIS, e.g. %s", layout->renamed,
                    layout->sjis, layout->renamed_path[0]);
    }
    return 0;
}

/* A path component that walks anywhere but down is refused. */
static int safe_relative(const char *rel) {
    if (!rel || *rel == '/' || strchr(rel, ':') || strchr(rel, '\\'))
        return 0;
    const char *p = rel;
    while (*p) {
        size_t n = strcspn(p, "/");
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.') ||
            p[n - 1] == ' ' || p[n - 1] == '.')
            return 0;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)p[i] < 32 || strchr("<>\"|?*", p[i]))
                return 0;
        p += n;
        if (*p)
            p++;
    }
    return 1;
}

/* Every entry of the package -- what is under its root in the archive -- by
   its path below the root, into fn; a directory's path ends in its slash.
   The walk stops at the first fn that fails and says so. */
typedef int (*package_fn)(void *ctx, const char *rel, const struct zipentry *e);
static int walk_package(struct zipread *z, const char *root, package_fn fn, void *ctx) {
    size_t plen = strlen(root);
    struct zipentry e;
    int rc;
    for (rc = zip_first(z, &e); rc > 0; rc = zip_next(z, &e)) {
        slashes(e.name);
        if (strncmp(e.name, root, plen) != 0)
            continue; /* not ours */
        const char *rel = e.name + plen;
        if (*rel == '\0' || mac_shadow(e.name))
            continue;
        if (e.name_truncated || !safe_relative(rel)) {
            logline("unpack: refusing %s", e.name);
            return -1;
        }
        if (fn(ctx, rel, &e) < 0)
            return -1;
    }
    return rc < 0 ? -1 : 0;
}

/* base/rel with suffix after it, a directory's slash left off: 0 when that
   does not fit a path. */
static int package_path(char *out, const char *base, const char *rel, const char *suffix) {
    size_t n = strlen(rel);
    while (n && rel[n - 1] == '/')
        n--;
    return snprintf(out, PATH_BUF, "%s/%.*s%s", base, (int)n, rel, suffix) < PATH_BUF;
}

static int is_directory(const char *rel) { return rel[strlen(rel) - 1] == '/'; }

/* What a name is called in its own directory, which is all sceIoRename takes
   for where a file goes. */
static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* What the package takes on the stick: every file rounded up to the cluster
   it fills, a cluster for each directory, and room for the records written
   after it. */
#define ROOM_FOR_RECORDS (256u * 1024u)
struct need {
    unsigned cluster;
    unsigned long long bytes;
};
static int count_need(void *ctx, const char *rel, const struct zipentry *e) {
    struct need *n = ctx;
    unsigned long long size = is_directory(rel) ? 1 : e->usize;
    n->bytes += (size + n->cluster - 1) / n->cluster * n->cluster;
    return 0;
}

/* Whether the stick has room for bytes more; where it cannot say, it is let
   try, and a write that fails is what stops the install. */
static int room_on(const char *dev, unsigned long long bytes, struct install_report *rep) {
    unsigned cluster = 1;
    long long left = storage_free_bytes_on(dev, &cluster);
    if (left < 0 || (unsigned long long)left >= bytes)
        return 1;
    rep->needed = bytes;
    logline("install: %lu KB needed on the Memory Stick, %lu KB free", (unsigned long)(bytes / 1024),
            (unsigned long)(left / 1024));
    return 0;
}

struct out_file {
    int fd;
    size_t *done;
};

static int out_sink(void *ctx, const void *data, size_t len) {
    struct out_file *o = ctx;
    int n = sceIoWrite(o->fd, data, len);
    if (n != (int)len)
        return -1;
    *o->done += len;
    return 0;
}

/* Everything under the package root goes into base, each file under its
   name with suffix after it: the staging directory of a first install with
   nothing after the name, the app's own folder of an update with SUFFIX_NEW.
   What the archive holds beside the package -- a readme at the top, a
   source tree -- stays in the archive. */

void install_abort(void) {
    g_abort = 1;
    https_abort();
}

struct unpack {
    struct zipread *z;
    const char *base, *suffix;
    size_t total, done, room;
    int files;
    https_progress progress;
    void *pctx;
};

static int count_total(void *ctx, const char *rel, const struct zipentry *e) {
    struct unpack *u = ctx;
    (void)rel;
    if (e->usize > MAX_PACKAGE_BYTES - u->total)
        return -1;
    u->total += e->usize;
    return 0;
}

static int unpack_one(void *ctx, const char *rel, const struct zipentry *e) {
    struct unpack *u = ctx;
    char path[PATH_BUF];
    if (strlen(rel) > u->room || ends_with_ours(rel) ||
        !package_path(path, u->base, rel, is_directory(rel) ? "" : u->suffix)) {
        logline("unpack: path too long: %s", rel);
        return -1;
    }
    if (is_directory(rel)) {
        mkdir_p(path);
        return 0;
    }
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(path);
        *slash = '/';
    }
    struct out_file o;
    o.done = &u->done;
    o.fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    if (o.fd < 0) {
        logline("unpack: cannot create %s", rel);
        /* A name in a code page other than UTF-8 -- Shift-JIS, from a zip
           made on a Japanese system -- is one the stick takes no file by. */
        why(pspdx_characters(rel, 0) < 0 ? T_WHY_NAME : T_WHY_STICK);
        return -1;
    }
    int xrc = zip_extract(u->z, e, out_sink, &o);
    /* On FAT32 the write that fails is often the close: the stick fills up
       while earlier writes were still buffered. */
    if (sceIoClose(o.fd) < 0) {
        logline("unpack: close failed on %s", rel);
        return -1;
    }
    if (xrc < 0) {
        logline("unpack: failed on %s", rel);
        return -1;
    }
    u->files++;
    if (u->progress)
        u->progress(u->pctx, u->done, u->total);
    if (g_abort) {
        logline("unpack: cancelled after %d files", u->files);
        return -1;
    }
    return 0;
}

static int unpack(struct zipread *z, const char *root, const char *base, const char *suffix,
                  struct install_report *rep, https_progress progress, void *pctx) {
    struct unpack u = {z, base, suffix, 0, 0, 0, 0, progress, pctx};
    /* A package that goes in has to come out again: the same file is later
       named under PSP/GAME/<dir>.old/ by the removal, and beside itself with
       SUFFIX_OLD by an update, with the directory at its full 32 characters;
       an entry that would not fit either name into rm_rf's buffer is refused
       here rather than left behind there. */
    u.room = PATH_BUF - 1 - strlen(GAME_DIR) - 1 - 32 - 1 - strlen(SUFFIX_OLD);
    if (walk_package(z, root, count_total, &u) < 0)
        return -1;
    if (progress)
        progress(pctx, 0, u.total);
    if (walk_package(z, root, unpack_one, &u) < 0)
        return -1;
    rep->files = u.files;
    rep->bytes = u.done;
    logline("unpack: %d files, %lu bytes", u.files, (unsigned long)u.done);
    return 0;
}

/* ------------------------------------------------------------- overlay */

/* An update, file by file in the app's folder, base: what each step does to
   one file the package ships, and what recovery does to undo it. A file the
   package does not ship is never named, so a save or a setting a homebrew
   keeps beside its EBOOT stays where it is. */
struct names {
    char cur[PATH_BUF], fresh[PATH_BUF], old[PATH_BUF];
};
static int names_of(struct names *n, const char *base, const char *rel) {
    return package_path(n->cur, base, rel, "") && package_path(n->fresh, base, rel, SUFFIX_NEW) &&
           package_path(n->old, base, rel, SUFFIX_OLD);
}

/* Before the unpack: what an earlier update left beside a file under the two
   names goes, or the app's own update says it is in the way. */
static int clear_one(void *ctx, const char *rel, const struct zipentry *e) {
    struct names n;
    (void)e;
    if (is_directory(rel))
        return 0;
    if (!names_of(&n, ctx, rel))
        return -1;
    if ((storage_exists(n.fresh) && rm_rf(n.fresh) < 0) || (storage_exists(n.old) && rm_rf(n.old) < 0))
        return logline("install: %s%s is in the way, delete it", n.cur + strlen(GAME_DIR) + 1,
                       SUFFIX_OLD), -1;
    return 0;
}

/* The commit: the file there steps aside, the new one takes its name. */
static int swap_one(void *ctx, const char *rel, const struct zipentry *e) {
    struct names n;
    (void)e;
    if (is_directory(rel))
        return 0;
    if (!names_of(&n, ctx, rel))
        return -1;
    if (storage_exists(n.cur) && sceIoRename(n.cur, base_name(n.old)) < 0)
        return logline("install: %s could not step aside", rel), -1;
    if (sceIoRename(n.fresh, base_name(n.cur)) < 0)
        return logline("install: %s could not take its place", rel), -1;
    return 0;
}

/* Recovery of an update cut after "placed", first of two passes: a file
   under its own name with neither of the two beside it was not there before
   -- every file had its new copy beside it when the swaps began, and a file
   that was there steps aside before the new one takes the name -- so it goes.
   Only this pass may tell such a file from the one put back by the next,
   which is why the journal says when it is through. */
static int drop_added(void *ctx, const char *rel, const struct zipentry *e) {
    struct names n;
    (void)e;
    if (is_directory(rel))
        return 0;
    if (!names_of(&n, ctx, rel))
        return -1;
    if (storage_exists(n.cur) && !storage_exists(n.fresh) && !storage_exists(n.old) &&
        rm_rf(n.cur) < 0)
        return -1;
    return 0;
}

/* Before "placed" nothing stepped aside: only the new copies go. An old copy
   beside a file then is what an earlier update could not clear, and is never
   put back over the file. */
static int drop_fresh(void *ctx, const char *rel, const struct zipentry *e) {
    struct names n;
    (void)e;
    if (is_directory(rel))
        return 0;
    if (!names_of(&n, ctx, rel))
        return -1;
    return storage_exists(n.fresh) && rm_rf(n.fresh) < 0 ? -1 : 0;
}

/* The second: the file that stepped aside takes its name back, and the new
   copy that never took it goes. */
static int put_back(void *ctx, const char *rel, const struct zipentry *e) {
    struct names n;
    (void)e;
    if (is_directory(rel))
        return 0;
    if (!names_of(&n, ctx, rel))
        return -1;
    if (storage_exists(n.old)) {
        if (storage_exists(n.cur) && rm_rf(n.cur) < 0)
            return -1;
        if (sceIoRename(n.old, base_name(n.cur)) < 0)
            return -1;
    }
    if (storage_exists(n.fresh) && rm_rf(n.fresh) < 0)
        return -1;
    return 0;
}

/* Once committed: what stepped aside goes. A file that will not is left and
   said, and the app's folder is swept again at the next start. */
static int drop_old(void *ctx, const char *rel, const struct zipentry *e) {
    struct names n;
    int *failed = ((void **)ctx)[1];
    (void)e;
    if (is_directory(rel) || !names_of(&n, ((void **)ctx)[0], rel))
        return 0;
    if ((storage_exists(n.old) && rm_rf(n.old) < 0) || (storage_exists(n.fresh) && rm_rf(n.fresh) < 0))
        *failed = 1;
    return 0;
}

/* The same by the names alone, for when the archive that lists the package
   is gone: every file under path that ends in one of the two. SWEEP_ALL
   takes both, SWEEP_NEW the new copies alone, and SWEEP_RESTORE puts an old
   copy back under its name. A file the update added cannot be told this way
   and stays. */
enum sweep { SWEEP_ALL, SWEEP_NEW, SWEEP_RESTORE };
static int sweep_ours(const char *path, enum sweep restore) {
    char *hit = malloc(PATH_BUF);
    if (!hit)
        return -1;
    int rc = 0;
    for (int again = 1; again && rc == 0;) {
        again = 0;
        SceUID d = sceIoDopen(path);
        if (d < 0)
            break;
        SceIoDirent e;
        memset(&e, 0, sizeof(e));
        while (!again && sceIoDread(d, &e) > 0) {
            char sub[PATH_BUF];
            if (strcmp(e.d_name, ".") && strcmp(e.d_name, "..") &&
                snprintf(sub, sizeof(sub), "%s/%s", path, e.d_name) < (int)sizeof(sub)) {
                size_t len = strlen(e.d_name), k = sizeof(SUFFIX_OLD) - 1;
                int old_copy = len >= k && !strncasecmp(e.d_name + len - k, SUFFIX_OLD, k);
                if (ends_with_ours(e.d_name) && !(restore == SWEEP_NEW && old_copy)) {
                    memcpy(hit, sub, sizeof(sub));
                    again = 1;
                } else if (FIO_S_ISDIR(e.d_stat.st_mode) && sweep_ours(sub, restore) < 0) {
                    rc = -1;
                }
            }
            memset(&e, 0, sizeof(e));
        }
        sceIoDclose(d);
        if (!again)
            break;
        size_t n = strlen(hit), k = sizeof(SUFFIX_OLD) - 1;
        if (restore == SWEEP_RESTORE && !strncasecmp(hit + n - k, SUFFIX_OLD, k)) {
            char cur[PATH_BUF];
            snprintf(cur, sizeof(cur), "%.*s", (int)(n - k), hit);
            if ((storage_exists(cur) && rm_rf(cur) < 0) || sceIoRename(hit, base_name(cur)) < 0)
                rc = -1;
        } else if (rm_rf(hit) < 0) {
            rc = -1;
        }
    }
    free(hit);
    return rc;
}

/* ------------------------------------------------------------------- db */

/* The journal owns exactly one operation. Until committed, recovery restores
   its previous app and complete metadata snapshot. No directory scanning. */
static const char *js(const cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static int journal_save(cJSON *j) {
    char *raw = cJSON_PrintUnformatted(j);
    if (!raw)
        return -1;
    int rc = sceIoSync(target(), 0) < 0 ? -1 : storage_write(JOURNAL, raw, strlen(raw));
    free(raw);
    return rc;
}
static int journal_phase(cJSON *j, const char *phase) {
    cJSON_DeleteItemFromObjectCaseSensitive(j, "phase");
    if (!cJSON_AddStringToObject(j, "phase", phase))
        return -1;
    return journal_save(j);
}
static int remove_tree(const char *path) { return storage_exists(path) ? rm_rf(path) : 0; }
static int restore_manifest(cJSON *j) {
    char path[256];
    storage_app_path(js(j, "id"), path, sizeof(path));
    cJSON *old = cJSON_GetObjectItemCaseSensitive(j, "old_manifest");
    if (cJSON_IsString(old) && strlen(old->valuestring) <= PSPDX_FILE_MAX)
        return storage_write(path, old->valuestring, strlen(old->valuestring));
    /* A file larger than a .pspdx can be is one begin() could not read
       back, and the app would be refused for as long as it sat there. */
    if (cJSON_IsString(old))
        logline("recovery: the saved manifest is larger than a .pspdx can be, not restored");
    return storage_remove(path);
}

/* What a committed transaction could not clear away -- a <dir>.old with a
   read-only file in it, the old copies an update left in an app's folder --
   is written down here and tried again at every start until it goes. It
   never holds the journal, and so never any other install; only the app it
   belongs to still finds it in its way. One line each, "old <dir>" or
   "sweep <dir> <device>". Older lines without a device name belong to
   the startup device. */
static void cleanup_note(const char *kind, const char *dir) {
    char *raw = NULL, line[80];
    snprintf(line, sizeof(line), "%s %s %s\n", kind, dir, target());
    int n = storage_read(CLEANUP, &raw, 64 * 1024);
    if (n < 0 || !strstr(raw, line)) {
        size_t had = n > 0 ? (size_t)n : 0, add = strlen(line);
        char *next = malloc(had + add + 1);
        if (next) {
            memcpy(next, n > 0 ? raw : "", had);
            memcpy(next + had, line, add + 1);
            storage_write(CLEANUP, next, had + add);
            free(next);
        }
    }
    free(raw);
}
static void cleanup_retry(void) {
    char *raw = NULL;
    int n = storage_read(CLEANUP, &raw, 64 * 1024);
    if (n < 0)
        return;
    char *keep = malloc((size_t)n + 1);
    size_t kept = 0;
    for (char *line = raw; keep && line && *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        char kind[8], dir[64], path[256], dev[8] = "";
        int done = 1;
        if (sscanf(line, "%7s %63s %7s", kind, dir, dev) >= 2 && manifest_dir_is_safe(dir)) {
            if (!*dev) snprintf(dev, sizeof(dev), "%s", storage_device());
            if (!storage_device_valid(dev) || !storage_device_available(dev)) {
                done = 0;
            } else {
                if (!strcmp(kind, "old")) {
                    snprintf(path, sizeof(path), "%s/PSP/GAME/%s.old", dev, dir);
                    done = remove_tree(path) == 0;
                } else if (!strcmp(kind, "sweep")) {
                    snprintf(path, sizeof(path), "%s/PSP/GAME/%s", dev, dir);
                    done = sweep_ours(path, SWEEP_ALL) == 0;
                }
            }
            logline("cleanup: PSP/GAME/%s%s %s", dir, !strcmp(kind, "old") ? ".old" : "",
                    done ? "cleared" : "still will not go");
        }
        if (!done) {
            kept += (size_t)snprintf(keep + kept, (size_t)n + 1 - kept, "%s\n", line);
        }
        line = end ? end + 1 : NULL;
    }
    if (keep && kept)
        storage_write(CLEANUP, keep, kept);
    else if (keep)
        storage_remove(CLEANUP);
    free(keep);
    free(raw);
}

/* The folder PSPDX runs from is PSPDX's own: no transaction of another id
   writes or removes it, and none removes it at all. */
static int self_folder(const char *dir) {
    return !strcmp(target(), storage_device()) && dir[0] && !strcasecmp(dir, storage_self_dir());
}

static const char *record_device(const cJSON *in) {
    const char *dev = js(in, "device");
    return *dev ? dev : storage_device();
}

static int recover_plugin(cJSON *j);
static int line_settle(const char *id, int say);

static int recover_journal(cJSON *j) {
    if (cJSON_GetObjectItemCaseSensitive(j, "plugin"))
        return recover_plugin(j);
    const char *id = js(j, "id"), *dir = js(j, "dir"), *prior = js(j, "prior"),
               *phase = js(j, "phase"), *op = js(j, "op"), *mode = js(j, "mode");
    if (!manifest_id_is_safe(id) || !manifest_dir_is_safe(dir) ||
        (*prior && !manifest_dir_is_safe(prior)))
        return logline("recovery: bad id or directory in the journal"), -1;
    /* Recovery writes old_manifest back, or removes the saved file when the
       journal has none; one that is there and is not text is a damaged
       journal, not the word that there was no file. */
    const cJSON *manifest = cJSON_GetObjectItemCaseSensitive(j, "old_manifest");
    if (manifest && !cJSON_IsString(manifest))
        return logline("recovery: the saved manifest in the journal is not text"), -1;
    const cJSON *snapshot = cJSON_GetObjectItemCaseSensitive(j, "old_state");
    if (!state_validate(snapshot))
        return logline("recovery: the saved state does not validate"), -1;
    const cJSON *previous = cJSON_GetObjectItemCaseSensitive(snapshot, id);
    const cJSON *in = cJSON_GetObjectItemCaseSensitive(previous, "installed");
    if ((*prior && strcmp(record_device(in), target())) || (*prior &&
         strcmp(js(in, "installdir") + (!strncmp(js(in, "installdir"), "PSP/GAME/", 9) ? 9 : 0),
                prior)) ||
        (!*prior && previous))
        return logline("recovery: the prior directory is not the saved one"), -1;
    /* Whose the name is matters to an install; a removal takes the app's
       own recorded directory, and two records that claim one name on a
       case-blind stick would otherwise hold each other's removal up. */
    const cJSON *other;
    cJSON_ArrayForEach(other, snapshot) {
        const cJSON *oi = cJSON_GetObjectItemCaseSensitive(other, "installed");
        if (!strcmp(record_device(oi), target()) && strcmp(op, "remove") && strcmp(other->string, id) &&
            !strcasecmp(js(oi, "installdir") + 9, dir))
            return logline("recovery: the target belongs to another app"), -1;
    }
    /* The snapshot is the journal's word only. A folder the records on the
       stick give to another app, and this app's own record does not name,
       is that app's whatever the journal says, and so is the folder PSPDX
       runs from: recovery deletes and renames neither. */
    struct installed own;
    int ours = (db_read(id, &own) == 0 && !strcmp(own.device, target()) && !strcasecmp(own.dir, dir)) ||
               (in && !strcmp(record_device(in), target()) && !strcasecmp(js(in, "installdir") + 9, dir));
    int self_id = !strcmp(id, PSPDX_SELF_ID);
    if ((!ours && state_target_owner_on(dir, id, target()) > 0) ||
        ((self_folder(dir) || self_folder(prior)) && (!self_id || !strcmp(op, "remove"))))
        return logline("recovery: PSP/GAME/%s is another app's on the stick", dir), -1;
    if (strcmp(op, "install") && strcmp(op, "remove"))
        return logline("recovery: unknown operation"), -1;
    int overlay = !strcmp(mode, "overlay"), fresh = !strcmp(mode, "fresh");
    if ((*mode && ((!overlay && !fresh) || strcmp(op, "install") || (overlay && !*prior))) ||
        (strcmp(phase, "prepared") && strcmp(phase, "ready") && strcmp(phase, "placed") &&
         strcmp(phase, "committed") && !(overlay && !strcmp(phase, "restoring"))))
        return logline("recovery: unknown phase"), -1;
    char dest[256], old[256], priorpath[256];
    snprintf(dest, sizeof(dest), "%s/%s", GAME_DIR, dir);
    snprintf(priorpath, sizeof(priorpath), "%s/%s", GAME_DIR, *prior ? prior : dir);
    snprintf(old, sizeof(old), "%s/%s.old", GAME_DIR, dir);
    int committed = !strcmp(phase, "committed"), placed = !strcmp(phase, "placed");
    /* An update's archive lists the files it laid over the folder, once the
       journal names the package's root in it. */
    const cJSON *root = cJSON_GetObjectItemCaseSensitive(j, "root");
    struct zipread z;
    int listed = overlay && cJSON_IsString(root) && zip_open(&z, ARCHIVE) == 0;
    int rc = -1;
    if (!committed && overlay) {
        /* A folder that moved went first; it comes back before the files. */
        if ((placed || !strcmp(phase, "restoring")) && strcmp(prior, dir) &&
            storage_exists(dest) && !storage_exists(priorpath) && sceIoRename(dest, prior) < 0) {
            logline("recovery: PSP/GAME/%s could not go back to %s", dir, prior);
            goto done;
        }
        if (placed && listed && walk_package(&z, root->valuestring, drop_added, priorpath) < 0) {
            logline("recovery: a file the update added to PSP/GAME/%s would not go", prior);
            goto done;
        }
        if (placed && journal_phase(j, "restoring") < 0)
            goto done;
        int swapping = placed || !strcmp(phase, "restoring");
        if (cJSON_IsString(root) &&
            (listed ? walk_package(&z, root->valuestring, swapping ? put_back : drop_fresh, priorpath)
                    : sweep_ours(priorpath, swapping ? SWEEP_RESTORE : SWEEP_NEW)) < 0) {
            logline("recovery: the files of PSP/GAME/%s could not be put back", prior);
            goto done;
        }
        if (cJSON_IsString(root) && !listed)
            logline("recovery: no archive to list the update by; PSP/GAME/%s put back by name", prior);
    } else if (!committed && strcmp(phase, "prepared")) {
        /* PSP/GAME/<dir> is this install's only when the journal says
           "placed" and the stage is gone: that word is written before
           the rename out of the stage, so the stage can only have gone
           by that rename. Anything under the name before the word, or
           beside a stage still there, is somebody else's and stays --
           and then a backup cannot go back, which is said and left.
           An update's backup still under .old is part of the proof: a
           recovery that already moved it back and was cut before the
           journal went leaves the app under the name, not the stage. A
           first install, "fresh", had nothing under the name to begin
           with. */
        if (placed && !storage_exists(STAGE) && (fresh || !*prior || storage_exists(old)) &&
            remove_tree(dest) < 0) {
            logline("recovery: the placed directory could not be cleared");
            goto done;
        }
        if (!fresh && storage_exists(old) && sceIoRename(old, *prior ? prior : dir) < 0) {
            logline("recovery: PSP/GAME/%s is in the way of its backup", dir);
            goto done;
        }
    }
    if (committed && overlay) {
        int failed = 0;
        void *ctx[2] = {dest, &failed};
        if (listed ? walk_package(&z, root->valuestring, drop_old, ctx) < 0 || failed
                   : sweep_ours(dest, SWEEP_ALL) < 0) {
            logline("recovery: old copies in PSP/GAME/%s would not go; tried again at the next start",
                    dir);
            cleanup_note("sweep", dir);
        }
    }
    if (listed) {
        zip_close(&z);
        listed = 0;
    }
    /* Room first: on a full stick the restore writes a .new beside each
       record, and the stick is full of the download and the stage. */
    if (remove_tree(STAGE) < 0 || storage_remove(ARCHIVE) < 0) {
        logline("recovery: the staging directory or the archive could not be removed");
        goto done;
    }
    if (!committed && (restore_manifest(j) < 0 ||
                       state_restore_app(cJSON_GetObjectItemCaseSensitive(j, "old_state"), id) < 0)) {
        logline("recovery: the saved manifest or state could not be restored");
        goto done;
    }
    /* Committed is done: a backup that will not go -- a read-only file in
       it -- is left, and tried again at the next start. Holding the journal
       for it would block every install. */
    if (committed && !overlay && remove_tree(old) < 0) {
        logline("recovery: PSP/GAME/%s.old could not be removed; tried again at the next start", dir);
        cleanup_note("old", dir);
    }
    if (storage_remove(JOURNAL) < 0) {
        logline("recovery: the journal could not be removed");
        goto done;
    }
    rc = 0;
done:
    if (listed)
        zip_close(&z);
    return rc;
}
static void recover_pending(void) {
    char *raw = NULL;
    int n = storage_read(JOURNAL, &raw, 512 * 1024);
    if (n < 0) {
        if (storage_exists(JOURNAL))
            logline("recovery: a journal is there and cannot be read");
        return;
    }
    cJSON *j = cJSON_ParseWithLengthOpts(raw, n + 1, NULL, 1);
    free(raw);
    /* Every install and removal ends here with its own journal saying
       "committed": clearing that away is the normal end, not a recovery. */
    const cJSON *phase = cJSON_GetObjectItemCaseSensitive(j, "phase");
    int finished = cJSON_IsString(phase) && !strcmp(phase->valuestring, "committed");
    char saved[5];
    snprintf(saved, sizeof(saved), "%s", target());
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(j, "device");
    const char *dev = device && cJSON_IsString(device) ? device->valuestring : storage_device();
    int valid = (!device || cJSON_IsString(device)) && storage_device_valid(dev) &&
                storage_device_available(dev);
    if (valid) target_set(dev);
    if (!j || !valid || recover_journal(j) < 0)
        logline("recovery: unfinished transaction; installation blocked");
    else if (!finished)
        logline("recovery: an unfinished transaction was put back");
    target_set(saved);
    cJSON_Delete(j);
}
void install_recover(void) {
    recover_pending();
    /* What a committed transaction could not clear, while none is open. */
    if (!storage_exists(JOURNAL))
        cleanup_retry();
    /* A cut in the middle of a write leaves its .new, or the .bak of the
       step before, beside the file; nothing reads them once the file is
       there, and a transaction has just settled, so this is where they go. */
    storage_sweep(storage_path("PSP/PSPDX/INSTALLED"));
    storage_sweep(storage_path("PSP/PSPDX/TMP"));
    storage_sweep(storage_path("PSP/PSPDX"));
    /* And a write to a PLUGINS.TXT that a cut left half done. */
    for (int i = 0, n = state_count(); i < n; i++) {
        char id[PSPDX_ID_SIZE];
        const char *at = state_id(i);
        if (at && snprintf(id, sizeof(id), "%s", at) > 0)
            line_settle(id, 0);
    }
}
/* The way out when recovery cannot finish what it found: the journal, the
   archive and the staging directory go. What the transaction may have put
   under PSP/GAME -- a backup under <dir>.old, the target itself -- stays,
   since which of the two is the app is the user's call now, and both are
   named in line. Returns 0 when the journal is gone. */
int install_discard(char *line, size_t size) {
    char *raw = NULL, dir[64] = "", dest[256], old[256];
    char saved[5];
    snprintf(saved, sizeof(saved), "%s", target());
    int known = 0;
    int n = storage_read(JOURNAL, &raw, 512 * 1024);
    if (n >= 0) {
        cJSON *j = cJSON_ParseWithLengthOpts(raw, n + 1, NULL, 1);
        const cJSON *dv = cJSON_GetObjectItemCaseSensitive(j, "device");
        const char *dev = dv ? js(j, "device") : storage_device();
        if (j && manifest_dir_is_safe(js(j, "dir")) && storage_device_valid(dev)) {
            snprintf(dir, sizeof(dir), "%s", js(j, "dir"));
            target_set(dev);
            known = 1;
        }
        cJSON_Delete(j);
    }
    free(raw);
    if (known && remove_tree(STAGE) < 0)
        logline("discard: the staging directory would not go");
    if (storage_remove(ARCHIVE) < 0)
        logline("discard: the archive would not go");
    if (storage_remove(JOURNAL) < 0)
        logline("discard: the journal would not go");
    snprintf(dest, sizeof(dest), "%s/%s", GAME_DIR, dir);
    snprintf(old, sizeof(old), "%s/%s.old", GAME_DIR, dir);
    int has_dest = *dir && storage_exists(dest), has_old = *dir && storage_exists(old);
    snprintf(line, size, "Unfinished install discarded%s%s%s%s%s%s",
             has_dest || has_old ? "; " : "", has_dest ? "PSP/GAME/" : "", has_dest ? dir : "",
             has_dest && has_old ? " and " : "", has_old ? dir : "",
             has_old ? ".old stay" : has_dest ? " stays" : "");
    logline("discard: %s", line);
    target_set(saved);
    return storage_exists(JOURNAL) ? -1 : 0;
}

/* A journal for one operation on id, not on the stick yet: what the app's
   record and its saved file were, for recovery to put back. */
static cJSON *journal_new(const char *id, const char *op) {
    cJSON *j = cJSON_CreateObject();
    if (!j)
        return NULL;
    cJSON_AddStringToObject(j, "id", id);
    cJSON_AddStringToObject(j, "device", target());
    cJSON_AddStringToObject(j, "op", op);
    cJSON_AddStringToObject(j, "phase", "prepared");
    cJSON *snapshot = state_snapshot();
    if (!snapshot) {
        cJSON_Delete(j);
        return NULL;
    }
    cJSON_AddItemToObject(j, "old_state", snapshot);
    char path[256], *raw = NULL;
    storage_app_path(id, path, sizeof(path));
    int n = storage_read(path, &raw, PSPDX_FILE_MAX);
    if (n >= 0)
        cJSON_AddStringToObject(j, "old_manifest", raw);
    else if (storage_exists(path)) {
        free(raw);
        cJSON_Delete(j);
        return NULL;
    }
    free(raw);
    return j;
}
static cJSON *begin(const char *id, const char *dir, const char *op) {
    install_recover();
    if (!state_ok() || storage_exists(JOURNAL) || storage_exists(STAGE)) {
        logline("install: unresolved state or staging directory (state %d, journal %d, stage %d)",
                state_ok(), storage_exists(JOURNAL), storage_exists(STAGE));
        return NULL;
    }
    struct installed rec;
    int has = db_read(id, &rec) == 0;
    char dest[256], backup[256];
    snprintf(dest, sizeof(dest), "%s/%s", GAME_DIR, dir);
    snprintf(backup, sizeof(backup), "%s/%s.old", GAME_DIR, dir);
    /* The folder the running EBOOT came out of is written by PSPDX's own
       update alone, and removed by nothing. */
    if ((self_folder(dir) || (has && self_folder(rec.dir))) &&
        (strcmp(id, PSPDX_SELF_ID) || !strcmp(op, "remove")))
        return logline("install: PSP/GAME/%s is the folder PSPDX runs from", has ? rec.dir : dir),
               NULL;
    /* Three things can sit under the name the release wants, and each is
       a different sentence on screen: the shell shows the last log line
       when an install fails, so the reason is spelled out here. */
    if (strcmp(op, "remove") && state_target_owner_on(dir, id, target()) != 0)
        return logline("install: PSP/GAME/%s is another app's, remove that app first", dir), NULL;
    if (storage_exists(backup))
        return logline("install: PSP/GAME/%s.old is in the way, delete or rename it", dir), NULL;
    if (storage_exists(dest) && (!has || strcasecmp(rec.dir, dir)))
        return logline("install: PSP/GAME/%s exists and is not this app's", dir), NULL;
    cJSON *j = journal_new(id, op);
    if (!j || !cJSON_AddStringToObject(j, "dir", dir) ||
        !cJSON_AddStringToObject(j, "prior", has ? rec.dir : "") || journal_save(j) < 0) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}

/* --------------------------------------------------------------- plugin */

/* A plugin is one file where a homebrew is a folder: the one .prx at the
   top of its zip, under seplugins/ on the device. Whether the custom
   firmware loads it is another matter, and another step: a line in the
   PLUGINS.TXT there, which is ARK-4's and its owner's, written only when
   the plugin is turned on.

   What is PSPDX's under seplugins/ is what it can prove is: the .prx by the
   SHA-256 the record took of it when it went in, a copy beside it by the
   journal that says PSPDX made it, and one line of the list by its bytes
   and the record's word that PSPDX added it. Everything else there is
   somebody's -- a file of the same name copied over the plugin, a file
   that only looks like one of PSPDX's copies, every other line -- and is
   neither written nor removed, whatever a record or a journal says.

   The list is never written anew: no copy of it is made, it is never
   renamed, cut short or deleted. A line is put after its end, the on or
   off of PSPDX's own line is written over itself, and the line is taken
   out by writing spaces over it, each of them the bytes there already
   read first and read back afterwards. */
#define SEPLUGINS "seplugins"
#define PLUGIN_LIST "PLUGINS.TXT"
#define PLUGIN_LIST_MAX (64 * 1024)
#define PLUGIN_PATH 112

/* name among the entries of dir, without regard to case, as the stick tells
   names apart; an emulator's stick may not. 1 with the name as it is
   spelled there in found, 0 when there is none, -1 when the folder cannot
   be read or holds two: only a listing says that a name is free, a failed
   stat says nothing. */
static int seen(const char *dir, const char *name, char found[64]) {
    SceUID d = sceIoDopen(dir);
    if (d < 0)
        return -1;
    SceIoDirent e;
    int rc, hits = 0;
    memset(&e, 0, sizeof(e));
    while ((rc = sceIoDread(d, &e)) > 0) {
        if (!strcasecmp(e.d_name, name) && strlen(e.d_name) < 64 && !hits++)
            strcpy(found, e.d_name);
        memset(&e, 0, sizeof(e));
    }
    if (sceIoDclose(d) < 0 || rc < 0 || hits > 1)
        return -1;
    return hits;
}

/* seplugins/ on dev, under whatever case it has there: 1 with its path,
   0 with the path it would be made under, -1 when the device cannot say. */
static int plugin_dir(const char *dev, char dir[32]) {
    char root[8], found[64];
    snprintf(root, sizeof(root), "%s/", dev);
    int there = seen(root, SEPLUGINS, found);
    if (there >= 0)
        snprintf(dir, 32, "%s/%.16s", dev, there ? found : SEPLUGINS);
    return there;
}

/* The same for a file in it, and for one more name: name with suffix. */
static int plugin_file(const char *dev, const char *name, const char *suffix, char path[PLUGIN_PATH]) {
    char dir[32], want[64], found[64];
    int there = plugin_dir(dev, dir);
    snprintf(want, sizeof(want), "%.40s%s", name, suffix);
    if (there > 0)
        there = seen(dir, want, found);
    if (there >= 0)
        snprintf(path, PLUGIN_PATH, "%s/%s", dir, there ? found : want);
    return there;
}

/* A file's bytes, up to limit of them, on the heap: its length, or -1. A
   plain read: nothing beside the file is looked at or put in its place. */
static int slurp(const char *path, char **out, size_t limit) {
    *out = NULL;
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0)
        return -1;
    SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    char *buf = size < 0 || (unsigned long long)size > limit ? NULL : malloc((size_t)size + 1);
    size_t at = 0;
    int n = buf && sceIoLseek(fd, 0, PSP_SEEK_SET) == 0 ? 1 : -1;
    while (n > 0 && at < (size_t)size) {
        n = sceIoRead(fd, buf + at, (size_t)size - at);
        at += n > 0 ? (size_t)n : 0;
    }
    if (sceIoClose(fd) < 0 || n <= 0 || at != (size_t)size) {
        free(buf);
        return -1;
    }
    *out = buf;
    return (int)at;
}

/* Whether a file that is there may be written: 0 for one the stick marks
   read-only, which is its owner's word that it is to be left alone. */
static int writable(const char *path) {
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0)
        return -1;
    return (st.st_mode & 0222) != 0;
}

/* The SHA-256 and the size of a file, which is how a .prx is known to be
   the one PSPDX installed. */
static int file_sha(const char *path, unsigned char sha[32], size_t *size) {
    static unsigned char block[4096];
    wc_Sha256 h;
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777), n = -1;
    if (fd < 0)
        return -1;
    *size = 0;
    if (wc_InitSha256(&h) == 0)
        while ((n = sceIoRead(fd, block, sizeof(block))) > 0) {
            wc_Sha256Update(&h, block, (word32)n);
            *size += (size_t)n;
        }
    if (sceIoClose(fd) < 0 || n < 0)
        return -1;
    wc_Sha256Final(&h, sha);
    return 0;
}

/* Whether the file at path is the one with this SHA-256. */
static int file_is(const char *path, const unsigned char want[32]) {
    unsigned char sha[32];
    size_t size;
    return file_sha(path, sha, &size) == 0 && !memcmp(sha, want, 32);
}

/* The list as it is on the stick. */
struct list {
    const char *dev;
    char path[PLUGIN_PATH];
    int there;
    char *text;
    size_t len;
};

/* Reads the list on dev; one that is not there is an empty one. Below 0
   for a list that is not to be written to at all: LIST_UNREAD one that
   cannot be read, LIST_LARGE one larger than any list, LIST_BINARY one
   with a NUL in it, past which ARK reads nothing. */
enum { LIST_UNREAD = -1, LIST_LARGE = -2, LIST_BINARY = -3 };
static int list_open(const char *dev, struct list *l) {
    memset(l, 0, sizeof(*l));
    l->dev = dev;
    l->there = plugin_file(dev, PLUGIN_LIST, "", l->path);
    int n = l->there > 0 ? slurp(l->path, &l->text, PLUGIN_LIST_MAX) : 0;
    if (!l->there)
        l->text = calloc(1, 1);
    if (l->there < 0 || n < 0 || !l->text) {
        SceIoStat st;
        memset(&st, 0, sizeof(st));
        return l->there > 0 && sceIoGetstat(l->path, &st) >= 0 && st.st_size > PLUGIN_LIST_MAX
                   ? LIST_LARGE : LIST_UNREAD;
    }
    l->len = (size_t)n;
    if (l->len && memchr(l->text, 0, l->len)) {
        free(l->text);
        l->text = NULL;
        return LIST_BINARY;
    }
    return 0;
}
/* The same for an operation that then stops, with the reason to give. */
static int list_open_or_say(const char *dev, struct list *l) {
    int rc = list_open(dev, l);
    if (rc < 0)
        why(rc == LIST_LARGE ? T_WHY_LIST_LARGE : rc == LIST_BINARY ? T_WHY_LIST_TEXT : T_WHY_LIST_READ);
    return rc;
}

/* The one way the list is written: w's bytes at w's place. First the list
   is read again and has to be, byte for byte, the one the write was worked
   out for; afterwards it is read back and has to be that list with those
   bytes there and no other byte different. A list that is not there is
   made, and only if no file of its name is. Never opened to be cut short,
   never renamed. */
static int list_put(const struct list *l, const struct pluginlist_write *w) {
    char *now = NULL;
    int fd, n, ok;
    if (l->there) {
        if (writable(l->path) != 1)
            return why(T_WHY_LIST_READONLY), -1;
        n = slurp(l->path, &now, PLUGIN_LIST_MAX);
        ok = n >= 0 && (size_t)n == l->len && !memcmp(now, l->text, l->len) && w->at <= l->len;
        free(now);
        if (!ok)
            return why(T_WHY_LIST_WRITE), -1;
        fd = sceIoOpen(l->path, PSP_O_WRONLY, 0777);
    } else {
        if (w->at != 0)
            return -1;
        fd = sceIoOpen(l->path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    }
    if (fd < 0)
        return why(T_WHY_LIST_WRITE), -1;
    ok = sceIoLseek(fd, (SceOff)w->at, PSP_SEEK_SET) == (SceOff)w->at &&
         sceIoWrite(fd, w->bytes, w->n) == (int)w->n;
    ok = sceIoClose(fd) >= 0 && ok && sceIoSync(l->dev, 0) >= 0;
    size_t end = w->at + w->n > l->len ? w->at + w->n : l->len;
    n = slurp(l->path, &now, PLUGIN_LIST_MAX + PLUGINLIST_BYTES);
    ok = ok && n >= 0 && (size_t)n == end && !memcmp(now, l->text, w->at) &&
         !memcmp(now + w->at, w->bytes, w->n) &&
         !memcmp(now + w->at + w->n, l->text + w->at + w->n, end - w->at - w->n);
    free(now);
    if (!ok) {
        logline("plugins: %s is not what was written to it; left as it is", l->path);
        why(T_WHY_LIST_WRITE);
    }
    return ok ? 0 : -1;
}

/* The path a plugin's line names: always <device>/seplugins/<file>,
   whatever case the folder has on the stick, since ARK and the stick tell
   none apart. */
static void line_path(const struct installed *rec, char out[64]) {
    snprintf(out, 64, "%s/" SEPLUGINS "/%s", rec->device, rec->plugin);
}

/* Whether the record says PSPDX added a line for this plugin: the path it
   wrote, which has to be the plugin's own. A record that names another is
   not believed about any line. */
static int line_owned(const struct installed *rec) {
    char path[64];
    line_path(rec, path);
    return !strcmp(rec->plugin_line, path);
}

int plugin_enabled(const char *id) {
    struct installed rec;
    struct list l;
    char path[64];
    if (db_read(id, &rec) < 0 || !rec.plugin[0])
        return -1;
    line_path(&rec, path);
    int on = list_open(rec.device, &l) == 0 && pluginlist_state(l.text, l.len, path) == 1;
    free(l.text);
    return on;
}

const char *plugin_refused(void) { return g_why; }

/* A write to the list, with the record saying so before it and after it.
   Everything that can refuse is asked first, so that the record never says
   a write was begun that was not: the list not read-only, and no longer
   than a list may be afterwards. Before the bytes go down the record holds
   what they are and what was there; once they are read back it holds what
   they come to -- then, the line PSPDX owns from now on, "" for none -- and
   the write no longer. A cut between the two is what line_settle is for. */
static int list_write(const char *id, const struct list *l, const struct pluginlist_write *w,
                      const char *then) {
    struct plugin_write begun = {1, w->at, l->len, "", "", ""};
    if (l->there && writable(l->path) != 1)
        return why(T_WHY_LIST_READONLY), -1;
    if (w->at + w->n > PLUGIN_LIST_MAX || w->at > l->len || w->n >= sizeof(begun.now))
        return why(T_WHY_LIST_LARGE), -1;
    memcpy(begun.now, w->bytes, w->n);
    memcpy(begun.was, l->text + w->at, w->at + w->n <= l->len ? w->n : 0);
    snprintf(begun.then, sizeof(begun.then), "%s", then);
    if (state_set_plugin(id, &begun, NULL) < 0)
        return why(T_WHY_STICK), -1;
    if (list_put(l, w) < 0)
        return -1;
    return state_set_plugin(id, NULL, then);
}

/* What the record says of the list, held against the list, before anything
   else is done with either. A write that was begun is seen through: where
   its bytes are there it is done, where none are it never happened, and
   where a cut left the first of them over what was there before -- with a
   line put after the end, the first of them at the end -- the rest are
   written, the same way and read back. Bytes there that are none of these
   are somebody's: nothing is written, and it is said. Then a record that
   says PSPDX owns a line the list does not have stops saying so, or a line
   typed later with the same bytes would be taken for PSPDX's.
   say: an operation asks, and is to stop with the reason; at a start
   nothing is said and such a write is left for the operation that will. */
static int line_settle(const char *id, int say) {
    struct installed rec;
    struct list l;
    struct pluginlist_write w;
    char path[64];
    if (db_read(id, &rec) < 0 || !rec.plugin[0] || (!rec.plugin_write.pending && !rec.plugin_line[0]))
        return 0;
    line_path(&rec, path);
    if (list_open(rec.device, &l) < 0)
        return 0;
    const struct plugin_write *b = &rec.plugin_write;
    int rc = 0;
    if (b->pending) {
        size_t n = strlen(b->now), k = 0;
        int after = !b->was[0];
        enum { NEVER, DONE, TORN, THEIRS } found = THEIRS;
        while (b->at + k < l.len && k < n && l.text[b->at + k] == b->now[k])
            k++;
        if (after) {
            found = l.len == b->at ? NEVER : l.len < b->at ? THEIRS : k == n ? DONE
                    : b->at + k == l.len ? TORN : THEIRS;
            w.at = l.len;
            w.n = n - k;
            memcpy(w.bytes, b->now + k, w.n);
        } else if (l.len == b->len && b->at + n <= l.len && strlen(b->was) == n) {
            found = !memcmp(l.text + b->at, b->was, n) ? NEVER : k == n ? DONE
                    : !memcmp(l.text + b->at + k, b->was + k, n - k) ? TORN : THEIRS;
            w.at = b->at;
            w.n = n;
            memcpy(w.bytes, b->now, n);
        }
        if (found == TORN && (writable(l.path) != 1 || list_put(&l, &w) < 0))
            rc = -1; /* still torn, still in the record: the next start tries again */
        else if (found == THEIRS) {
            logline("plugins: %s is not as PSPDX left it at byte %lu; nothing written", l.path,
                    (unsigned long)b->at);
            rc = say ? (why(T_WHY_LIST_CHANGED), state_set_plugin(id, NULL, NULL), -1) : 0;
        } else {
            if (found == TORN)
                logline("plugins: a write to %s that was cut short is finished", l.path);
            rc = state_set_plugin(id, NULL, found == NEVER ? NULL : b->then);
        }
    }
    free(l.text);
    if (rc == 0 && db_read(id, &rec) == 0 && rec.plugin_line[0] && !rec.plugin_write.pending &&
        list_open(rec.device, &l) == 0) {
        if (!line_owned(&rec) || pluginlist_blank(l.text, l.len, path, &w) <= 0)
            rc = state_set_plugin(id, NULL, "");
        free(l.text);
    }
    return rc;
}

int plugin_switch(const char *id, int on) {
    struct installed rec;
    struct list l;
    struct pluginlist_write w;
    char path[64];
    g_why[0] = 0;
    if (line_settle(id, 1) < 0 || db_read(id, &rec) < 0 || !rec.plugin[0] ||
        !storage_device_available(rec.device))
        return -1;
    line_path(&rec, path);
    if (list_open_or_say(rec.device, &l) < 0)
        return -1;
    int rc = 0, state = pluginlist_state(l.text, l.len, path);
    int own = line_owned(&rec) ? pluginlist_switch(l.text, l.len, path, on, &w) : -1;
    if (own > 0)
        rc = list_write(id, &l, &w, path);
    else if (own < 0 && on && state < 0)
        /* No line names it: PSPDX's own is put after the list's end, and is
           PSPDX's once it is read back from there. */
        rc = pluginlist_add(l.text, l.len, path, &w) < 0 ? (why(T_WHY_LIST_WRITE), -1)
                                                        : list_write(id, &l, &w, path);
    free(l.text);
    if (rc < 0 || list_open_or_say(rec.device, &l) < 0)
        return -1;
    state = pluginlist_state(l.text, l.len, path) == 1;
    free(l.text);
    return state;
}

/* PSPDX's own line taken out of the list, where the record says there is
   one and the list has it; the record then owns none. 0 when the list holds
   no line of PSPDX's afterwards, -1 when it could not be written. *left:
   lines that name the plugin and are not PSPDX's stay. */
static int line_blank(const char *id, int *left) {
    struct installed rec;
    struct list l;
    struct pluginlist_write w;
    char path[64];
    *left = 0;
    if (line_settle(id, 1) < 0 || db_read(id, &rec) < 0)
        return -1;
    line_path(&rec, path);
    /* A list that cannot be read holds PSPDX's line or does not: where the
       record says it may, nothing is removed until somebody has seen to
       the list. */
    int owned = line_owned(&rec);
    if (owned ? list_open_or_say(rec.device, &l) < 0 : list_open(rec.device, &l) < 0)
        return owned ? -1 : 0;
    int own = owned && pluginlist_blank(l.text, l.len, path, &w) > 0;
    int rc = own ? list_write(id, &l, &w, "") : 0;
    if (rc == 0) {
        if (own)
            memset(l.text + w.at, ' ', w.n);
        *left = pluginlist_state(l.text, l.len, path) >= 0;
    }
    free(l.text);
    return rc;
}

static int journal_sha(const cJSON *j, const char *key, unsigned char out[32]) {
    const char *hex = js(j, key);
    return strlen(hex) == 64 && pspdx_hex(hex, out, 32) == 0;
}
static int journal_add_sha(cJSON *j, const char *key, const unsigned char sha[32]) {
    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(hex + 2 * i, "%02x", sha[i]);
    cJSON_DeleteItemFromObjectCaseSensitive(j, key);
    return cJSON_AddStringToObject(j, key, hex) != NULL;
}

/* The bytes of a zip's entry into a hash, written nowhere. */
static int hash_sink(void *ctx, const void *data, size_t len) {
    struct dl *d = ctx;
    return len > d->expected - d->written || wc_Sha256Update(&d->sha, data, (word32)len) != 0
               ? -1 : (d->written += len, 0);
}

/* Recovery of a plugin's transaction, which touches a file only where the
   journal proves it is PSPDX's: the new copy by the journal's word that
   PSPDX was about to make one where none was, the plugin's file by the
   SHA-256 the journal has of it, and never a file whose bytes are another's.
   The journal's phases, for an install: "prepared", nothing on the stick;
   "staging", the new copy being written beside the plugin; "placed", the
   two renames under way; "committed". A removal has no copies: its file
   goes once it is committed. */
static int recover_plugin(cJSON *j) {
    const char *id = js(j, "id"), *name = js(j, "plugin"), *phase = js(j, "phase");
    int staging = !strcmp(phase, "staging"), placed = !strcmp(phase, "placed"),
        committed = !strcmp(phase, "committed"), removal = !strcmp(js(j, "op"), "remove");
    if (!manifest_id_is_safe(id) || (*name ? !manifest_plugin_is_safe(name) : staging || placed) ||
        (!staging && !placed && !committed && strcmp(phase, "prepared")) ||
        (!removal && strcmp(js(j, "op"), "install")))
        return logline("recovery: bad id, plugin or phase in the journal"), -1;
    const cJSON *manifest = cJSON_GetObjectItemCaseSensitive(j, "old_manifest");
    if (manifest && !cJSON_IsString(manifest))
        return logline("recovery: the saved manifest in the journal is not text"), -1;
    const cJSON *snapshot = cJSON_GetObjectItemCaseSensitive(j, "old_state");
    if (!state_validate(snapshot))
        return logline("recovery: the saved state does not validate"), -1;
    unsigned char sha[32], was[32];
    int has_sha = journal_sha(j, "sha", sha), has_was = journal_sha(j, "was", was);
    char cur[PLUGIN_PATH], fresh[PLUGIN_PATH], old[PLUGIN_PATH];
    int c = *name ? plugin_file(target(), name, "", cur) : 0;
    int f = *name ? plugin_file(target(), name, SUFFIX_NEW, fresh) : 0;
    int o = *name ? plugin_file(target(), name, SUFFIX_OLD, old) : 0;
    if (c < 0 || f < 0 || o < 0)
        return logline("recovery: " SEPLUGINS " cannot be read"), -1;
    if (removal) {
        /* The plugin's file goes with the record, and only as the file the
           record knew. */
        if (committed && c && has_was && file_is(cur, was) && sceIoRemove(cur) < 0)
            logline("recovery: " SEPLUGINS "/%s would not go", name);
    } else if (staging || placed) {
        /* Under way: what stepped aside comes back, once the new file that
           took its name is gone; a first install's file goes. Each only as
           the file the journal has the hash of. */
        if (placed && c && has_sha && file_is(cur, sha) && (o || !has_was)) {
            if (sceIoRemove(cur) < 0)
                return logline("recovery: " SEPLUGINS "/%s could not be cleared", name), -1;
            c = 0;
        }
        if (placed && o && !c && has_was && file_is(old, was) && sceIoRename(old, base_name(cur)) < 0)
            return logline("recovery: " SEPLUGINS "/%s could not be put back", name), -1;
        /* The copy PSPDX was making: whole, by its hash, or cut short, and
           then shorter than it was going to be. Any other file under that
           name is not it and stays. */
        SceIoStat st;
        memset(&st, 0, sizeof(st));
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(j, "size");
        if (f && has_sha && (file_is(fresh, sha) || (cJSON_IsNumber(size) && sceIoGetstat(fresh, &st) >= 0 &&
                                                     (double)st.st_size < size->valuedouble))) {
            if (sceIoRemove(fresh) < 0)
                return logline("recovery: " SEPLUGINS "/%s%s would not go", name, SUFFIX_NEW), -1;
        } else if (f) {
            logline("recovery: " SEPLUGINS "/%s%s is not the copy PSPDX was making; left", name,
                    SUFFIX_NEW);
        }
    } else if (committed && o && has_was && file_is(old, was) && sceIoRemove(old) < 0) {
        logline("recovery: " SEPLUGINS "/%s%s would not go; delete it", name, SUFFIX_OLD);
    }
    if (storage_remove(ARCHIVE) < 0)
        return logline("recovery: the archive could not be removed"), -1;
    if (!committed && (restore_manifest(j) < 0 || state_restore_app(snapshot, id) < 0))
        return logline("recovery: the saved manifest or state could not be restored"), -1;
    if (storage_remove(JOURNAL) < 0)
        return logline("recovery: the journal could not be removed"), -1;
    return 0;
}

/* A plugin's journal, on the stick: "plugin" is what tells it from a
   folder's, and is empty until the zip has named the file. */
static cJSON *plugin_begin(const char *id, const char *op, const char *name) {
    install_recover();
    if (!state_ok() || storage_exists(JOURNAL)) {
        logline("install: unresolved state (state %d, journal %d)", state_ok(),
                storage_exists(JOURNAL));
        return NULL;
    }
    cJSON *j = journal_new(id, op);
    if (!j || !cJSON_AddStringToObject(j, "plugin", name) || journal_save(j) < 0) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}

/* The plugin in the archive: the one .prx at its top. A licence or a readme
   beside it, and anything in a folder, is not installed. */
static int find_plugin(struct zipread *z, struct zipentry *out) {
    struct zipentry e;
    int rc, count = 0;
    for (rc = zip_first(z, &e); rc > 0; rc = zip_next(z, &e)) {
        size_t n = strlen(e.name);
        if (strchr(e.name, '/') || strchr(e.name, '\\') || n < 4 ||
            strcasecmp(e.name + n - 4, ".prx"))
            continue;
        if (!count++)
            *out = e;
    }
    if (rc < 0 || count != 1) {
        logline("zip: expected one .prx at the top, found %d", count);
        why(rc < 0 ? T_WHY_ZIP : count ? T_WHY_PRXS : T_WHY_NO_PRX);
        return -1;
    }
    if (out->name_truncated || !manifest_plugin_is_safe(out->name)) {
        logline("zip: %s is no name for a plugin's file", out->name);
        why(T_WHY_PRX_NAME);
        return -1;
    }
    return 0;
}

/* Whether another app's record has seplugins/<name> on this device. */
static int plugin_taken(const char *name, const char *id) {
    struct installed rec;
    for (int i = 0, n = state_count(); i < n; i++) {
        const char *other = state_id(i);
        if (other && strcmp(other, id) && db_read(other, &rec) == 0 &&
            !strcmp(rec.device, target()) && !strcasecmp(rec.plugin, name))
            return 1;
    }
    return 0;
}

/* What the record's plugin is on the stick: PLUGIN_OURS the file PSPDX
   installed, by its SHA-256; PLUGIN_GONE no file of its name;
   PLUGIN_THEIRS a file of its name that is not the one installed, which is
   whoever put it there's; -1 when the stick cannot say. path is where. */
enum { PLUGIN_GONE, PLUGIN_OURS, PLUGIN_THEIRS };
static int plugin_owned(const struct installed *rec, char path[PLUGIN_PATH]) {
    int there = plugin_file(rec->device, rec->plugin, "", path);
    if (there <= 0)
        return there < 0 ? -1 : PLUGIN_GONE;
    struct manifest hashed = {0};
    memcpy(hashed.sha256, rec->plugin_sha256, 32);
    return manifest_has_sha256(&hashed) && file_is(path, rec->plugin_sha256) ? PLUGIN_OURS
                                                                            : PLUGIN_THEIRS;
}

/* Forgets a plugin and takes what is PSPDX's of it off the stick: its own
   line out of the list first, then the record, then -- once that is
   committed -- the file. The line goes before the journal is begun, so that
   a removal that stops later leaves a plugin that is installed, turned off
   and owns no line, which is what the stick then holds. A file under the
   plugin's name that is not the one PSPDX installed is somebody's own
   build: it stays, and so does the line that loads it; only the record
   goes. Returns UNINSTALL_ flags, or -1. */
static int plugin_remove(const struct installed *rec) {
    char cur[PLUGIN_PATH], path[256];
    int owned = plugin_owned(rec, cur), left = 0;
    if (owned < 0)
        return -1;
    if (owned == PLUGIN_OURS && writable(cur) != 1)
        return why(T_WHY_PRX_READONLY), -1;
    if (owned != PLUGIN_THEIRS && line_blank(rec->id, &left) < 0)
        return -1;
    cJSON *j = plugin_begin(rec->id, "remove", rec->plugin);
    if (!j)
        return -1;
    int rc = -1;
    storage_app_path(rec->id, path, sizeof(path));
    if ((owned == PLUGIN_OURS && !journal_add_sha(j, "was", rec->plugin_sha256)) ||
        journal_save(j) < 0)
        goto end;
    if (state_forget(rec->id) < 0 || storage_remove(path) < 0)
        goto end;
    if (journal_phase(j, "committed") < 0)
        goto end;
    rc = owned == PLUGIN_THEIRS ? UNINSTALL_FILE : left ? UNINSTALL_LINES : 0;
    if (owned != PLUGIN_OURS)
        logline("remove: " SEPLUGINS "/%s is %s", rec->plugin,
                owned == PLUGIN_GONE ? "already gone"
                                     : "not the file PSPDX installed; left, with its line");
end:
    cJSON_Delete(j);
    install_recover();
    if (storage_exists(JOURNAL))
        rc = -1;
    return rc;
}

/* A plugin's .prx into sink: out of the zip that was downloaded, or, for a
   plugin the release carries, out of the file beside the EBOOT. */
static int plugin_feed(struct zipread *z, const struct zipentry *e, const char *bundled,
                       int (*sink)(void *, const void *, size_t), void *ctx) {
    static unsigned char block[4096];
    if (!bundled)
        return zip_extract(z, e, sink, ctx);
    int fd = sceIoOpen(bundled, PSP_O_RDONLY, 0777), n = -1;
    if (fd < 0)
        return -1;
    while ((n = sceIoRead(fd, block, sizeof(block))) > 0)
        if (sink(ctx, block, (size_t)n) < 0)
            n = -1;
    return sceIoClose(fd) < 0 || n < 0 ? -1 : 0;
}

/* bundled: the .prx itself, where the plugin is not fetched but carried;
   the release's zip is then known by m's hash alone. */
static int plugin_release(const struct manifest *m, const struct installed *existing,
                          const char *bundled, struct install_report *rep,
                          install_phase_cb phase, https_progress progress, void *ctx) {
    char cur[PLUGIN_PATH], fresh[PLUGIN_PATH], old[PLUGIN_PATH], dir[32];
    /* Before anything is fetched: a file of the plugin's name that is not
       the one PSPDX installed is somebody's own build. An update leaves it,
       its line and the record exactly as they are, and says why. */
    int owned = existing ? plugin_owned(existing, cur) : PLUGIN_GONE;
    if (owned < 0)
        return why(T_WHY_STICK), -1;
    if (owned == PLUGIN_THEIRS) {
        logline("install: " SEPLUGINS "/%s is not the file PSPDX installed; left", existing->plugin);
        return why(T_WHY_PRX_CHANGED), -1;
    }
    cJSON *j = plugin_begin(m->id, "install", "");
    if (!j)
        return -1;
    g_abort = 0;
    int rc = -2, open = 0;
    unsigned char got[32] = {0}, sha[32], put[32];
    struct zipread z;
    struct zipentry e;
    memset(&e, 0, sizeof(e));
    if (bundled) {
        size_t size = 0;
        rc = -1;
        memcpy(got, m->sha256, 32);
        snprintf(e.name, sizeof(e.name), "%s", base_name(bundled));
        if (!manifest_plugin_is_safe(e.name) || file_sha(bundled, sha, &size) < 0 || !size)
            goto end;
        e.usize = (uint32_t)size;
    } else {
        if (phase)
            phase(ctx, "download");
        if (download(m, progress, ctx, got) < 0) {
            if (g_abort)
                rc = INSTALL_CANCELLED;
            goto end;
        }
        if (zip_open(&z, ARCHIVE) < 0)
            goto end;
        open = 1;
        rc = -1;
        if (find_plugin(&z, &e) < 0)
            goto end;
    }
    /* An installed plugin keeps its file's name: a line in PLUGINS.TXT
       names it, and may be anybody's. */
    if (existing && strcasecmp(existing->plugin, e.name)) {
        logline("install: %s is installed as " SEPLUGINS "/%s and its zip now holds %s", m->id,
                existing->plugin, e.name);
        why(T_WHY_PRX_RENAMED);
        goto end;
    }
    /* Every name this install writes under has to be free, or the
       plugin's own: a file there PSPDX did not put there is never adopted,
       written over or cleared away, a copy from an earlier install
       included, which only its own journal could vouch for. */
    int c = plugin_file(target(), e.name, "", cur), f = plugin_file(target(), e.name, SUFFIX_NEW, fresh),
        o = plugin_file(target(), e.name, SUFFIX_OLD, old);
    if (c < 0 || f < 0 || o < 0) {
        why(T_WHY_STICK);
        goto end;
    }
    if (f || o) {
        logline("install: %s is in the way and not known to be PSPDX's; delete it", f ? fresh : old);
        why(T_WHY_PRX_COPY);
        goto end;
    }
    if (c && (owned != PLUGIN_OURS || plugin_taken(e.name, m->id))) {
        logline("install: %s exists and is not this app's", cur);
        why(T_WHY_PRX_THERE);
        goto end;
    }
    if (!c && plugin_taken(e.name, m->id)) {
        why(T_WHY_PRX_THERE);
        goto end;
    }
    if (c && writable(cur) != 1) {
        why(T_WHY_PRX_READONLY);
        goto end;
    }
    unsigned cluster = 1;
    storage_free_bytes_on(target(), &cluster);
    unsigned long long need = ((unsigned long long)e.usize + cluster - 1) / cluster * cluster;
    if (!room_on(target(), need + ROOM_FOR_RECORDS, rep)) {
        rc = INSTALL_NO_SPACE;
        goto end;
    }
    /* Every check is through: the folder is made where there is none, and
       from here the journal names the file and says a copy is being made
       beside it, which is what lets recovery clear that copy away. */
    int made = plugin_dir(target(), dir);
    if (made < 0 || (!made && sceIoMkdir(dir, 0777) < 0))
        goto end;
    /* The copy about to be made, by the hash and the size it will have:
       what recovery knows it by. */
    struct dl hashed = {0};
    hashed.expected = e.usize;
    if (wc_InitSha256(&hashed.sha) != 0 || plugin_feed(&z, &e, bundled, hash_sink, &hashed) < 0)
        goto end;
    wc_Sha256Final(&hashed.sha, sha);
    if (!cJSON_ReplaceItemInObjectCaseSensitive(j, "plugin", cJSON_CreateString(e.name)) ||
        (c && !journal_add_sha(j, "was", existing->plugin_sha256)) ||
        !journal_add_sha(j, "sha", sha) || !cJSON_AddNumberToObject(j, "size", (double)e.usize) ||
        journal_phase(j, "staging") < 0)
        goto end;
    if (phase)
        phase(ctx, "unpack");
    size_t done = 0, size = 0;
    struct out_file out = {sceIoOpen(fresh, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777), &done};
    if (out.fd < 0) {
        logline("unpack: cannot create %s", fresh);
        why(T_WHY_STICK);
        goto end;
    }
    int xrc = plugin_feed(&z, &e, bundled, out_sink, &out);
    if (sceIoClose(out.fd) < 0 || xrc < 0) {
        logline("unpack: failed on %s", e.name);
        goto end;
    }
    if (progress)
        progress(ctx, done, done);
    if (g_abort) {
        rc = INSTALL_CANCELLED;
        goto end;
    }
    rc = -3;
    /* What went onto the stick, read back from it: the record's proof that
       the file is PSPDX's, and the journal's. */
    if (sceIoSync(target(), 0) < 0 || file_sha(fresh, put, &size) < 0 || size != done ||
        memcmp(put, sha, 32))
        goto end;
    if (phase)
        phase(ctx, "commit");
    if (journal_phase(j, "placed") < 0)
        goto end;
    /* A rename takes no name that is taken: each is looked for again the
       moment before, rather than left to the rename to refuse. */
    char name[64], look[PLUGIN_PATH];
    snprintf(name, sizeof(name), "%s", base_name(cur));
    if (c && (plugin_file(target(), e.name, SUFFIX_OLD, look) != 0 ||
              sceIoRename(cur, base_name(old)) < 0))
        goto end;
    if (plugin_file(target(), e.name, "", look) != 0 || sceIoRename(fresh, name) < 0)
        goto end;
    char path[256];
    storage_app_path(m->id, path, sizeof(path));
    if (storage_write(path, m->raw, strlen(m->raw)) < 0 ||
        state_commit_plugin(m, existing ? existing->plugin : e.name, got, target(), sha,
                            existing ? existing->plugin_line : "") < 0)
        goto end;
    if (journal_phase(j, "committed") < 0)
        goto end;
    snprintf(rep->id, sizeof(rep->id), "%s", m->id);
    snprintf(rep->plugin, sizeof(rep->plugin), "%.32s", e.name);
    snprintf(rep->version, sizeof(rep->version), "%s", m->version);
    rep->rev = m->rev;
    rep->files = 1;
    rep->bytes = done;
    logline("unpack: " SEPLUGINS "/%s, %lu bytes", e.name, (unsigned long)done);
    rc = 0;
end:
    if (open)
        zip_close(&z);
    cJSON_Delete(j);
    install_recover();
    if (storage_exists(JOURNAL))
        return -8;
    /* The list is not an install's business: a plugin goes in turned off
       unless a line names it already, and is turned on when somebody says
       so. */
    if (rc == 0)
        rep->plugin_off = plugin_enabled(m->id) != 1;
    return rc;
}
int uninstall(const char *id) {
    g_why[0] = 0;
    install_recover();
    struct installed rec;
    if (!manifest_id_is_safe(id) || db_read(id, &rec) < 0)
        return -1;
    target_set(rec.device);
    if (!storage_device_available(target())) return -1;
    /* Whatever record names it: what is in RAM would go on running with
       nothing left to start again. */
    if (self_folder(rec.dir)) {
        logline("remove: PSP/GAME/%s is the folder PSPDX runs from; not deleted", rec.dir);
        return INSTALL_SELF;
    }
    if (rec.plugin[0])
        return plugin_remove(&rec);
    cJSON *j = begin(id, rec.dir, "remove");
    if (!j)
        return -1;
    int rc = -1;
    char path[256], from[256], name[80];
    storage_app_path(id, path, sizeof(path));
    snprintf(from, sizeof(from), "%s/%s", GAME_DIR, rec.dir);
    snprintf(name, sizeof(name), "%s.old", rec.dir);
    if (journal_phase(j, "ready") < 0)
        goto end;
    /* A folder deleted by hand has nothing to set aside, and the record
       still goes: an app that can be neither removed nor installed again
       would be worse than one line in the log. */
    if (!storage_exists(from))
        logline("remove: PSP/GAME/%s is already gone", rec.dir);
    else if (sceIoRename(from, name) < 0)
        goto end;
    if (state_forget(id) < 0 || storage_remove(path) < 0)
        goto end;
    if (journal_phase(j, "committed") < 0)
        goto end;
    rc = 0;
end:
    cJSON_Delete(j);
    install_recover();
    if (storage_exists(JOURNAL))
        rc = -1;
    return rc;
}
int install_retire_legacy(void) {
    if (storage_exists(JOURNAL))
        return 0;
    int rc = state_retire_legacy(PSPDX_LEGACY_ID, PSPDX_LEGACY_SOURCE, storage_self_dir());
    if (rc > 0)
        logline("self: retired the record PSPDX 0.5 and before kept of itself");
    return rc;
}
int install_bundled(const struct manifest *m, const char *prx, const char *dev,
                    struct install_report *rep) {
    struct pspdx_file spec;
    struct installed existing;
    char said[80];
    memset(rep, 0, sizeof(*rep));
    g_why[0] = 0;
    install_recover();
    if (!storage_device_valid(dev) || storage_exists(JOURNAL) || !manifest_id_is_safe(m->id) ||
        !manifest_has_sha256(m) || !m->raw ||
        pspdx_parse(m->raw, strlen(m->raw), &spec, said, sizeof(said)) < 0 ||
        strcmp(spec.type, "plugin") || !sources_same_repo(spec.source, m->repo) ||
        db_read(m->id, &existing) == 0)
        return -1;
    target_set(dev);
    int rc = storage_device_available(dev) ? plugin_release(m, NULL, prx, rep, NULL, NULL, NULL) : -1;
    if (rc < 0)
        snprintf(rep->why, sizeof(rep->why), "%s", g_why);
    return rc;
}
int install_release(const struct manifest *m, struct install_report *rep, install_phase_cb phase,
                    https_progress progress, void *ctx) {
    return install_release_to(m, storage_device(), rep, phase, progress, ctx);
}
static int release_to(const struct manifest *m, const char *dev, struct install_report *rep,
                      install_phase_cb phase, https_progress progress, void *ctx);
int install_release_to(const struct manifest *m, const char *dev, struct install_report *rep,
                       install_phase_cb phase, https_progress progress, void *ctx) {
    g_why[0] = 0;
    int rc = release_to(m, dev, rep, phase, progress, ctx);
    if (rc < 0 && rc != INSTALL_CANCELLED && rc != INSTALL_NO_SPACE)
        snprintf(rep->why, sizeof(rep->why), "%s", g_why);
    return rc;
}
static int release_to(const struct manifest *m, const char *dev, struct install_report *rep,
                      install_phase_cb phase, https_progress progress, void *ctx) {
    memset(rep, 0, sizeof(*rep));
    if (!storage_device_valid(dev)) return -1;
    install_recover();
    if (storage_exists(JOURNAL)) return -8;
    struct installed recorded;
    target_set(db_read(m->id, &recorded) == 0 ? recorded.device : dev);
    if (!storage_device_available(target())) {
        logline("install: %s is not available", target());
        return -1;
    }
    struct pspdx_file spec;
    char why[80];
    unsigned char got[32] = {0};
    if (!manifest_id_is_safe(m->id) || !manifest_size_in_range(m->size) ||
        !manifest_rev_in_range(m->rev) || strncmp(m->url, "https://", 8))
        return -1;
    if (!m->raw || pspdx_parse(m->raw, strlen(m->raw), &spec, why, sizeof(why)) < 0) {
        logline("install: valid original manifest required");
        return -1;
    }
    /* An ISO is listed and not installed: it does not go under PSP/GAME,
       and nothing here knows where it does go. */
    if (!pspdx_type_installable(spec.type)) {
        logline("install: type %s cannot be installed yet", spec.type);
        return -1;
    }
    /* A plugin names no folder, and its file is named by its zip. */
    int plugin = !strcmp(spec.type, "plugin");
    if (plugin ? m->dir[0] : !manifest_dir_is_safe(m->dir))
        return -1;
    /* The id is the one the file makes, or one a catalog gave the entry;
       either way the file is the release's repository's, and the id is no
       other repository's on the stick. */
    if (!sources_same_repo(spec.source, m->repo))
        return -1;
    struct installed existing;
    int has = db_read(m->id, &existing) == 0;
    if (has && !sources_same_repo(existing.repo, m->repo))
        return -1;
    if (has && plugin != (existing.plugin[0] != 0)) {
        logline("install: %s is installed as a %s and its file now says %s; delete it first",
                m->id, plugin ? "homebrew" : "plugin", spec.type);
        return -1;
    }
    /* The folder is the one the file names, or the one the app is installed
       in already: an app keeps its folder, whoever renamed it. */
    if (strcmp(spec.installdir + 9, m->dir) && !(has && !strcmp(existing.dir, m->dir))) {
        logline("install: valid original manifest required");
        return -1;
    }
    /* A GitHub release is held to coming out of the repository. Anywhere
       else nothing says where a download may come from, so the catalog's
       hash of it has to, and a release without one has nothing to be held
       to. */
    struct source_repo repo;
    if (sources_parse_repo(spec.source, &repo)) {
        if (!sources_release_url(m->repo, m->url))
            return -1;
    } else if (!manifest_has_sha256(m)) {
        logline("install: %s is from outside GitHub, and no SHA-256 came with it to hold the "
                "download to", m->id);
        return -1;
    }
    /* Before anything is written, the journal included: a stick with no room
       for the download says so rather than failing somewhere in it. What an
       earlier transaction left is cleared first, since it may be what fills
       the stick. */
    install_recover();
    unsigned cluster = 1;
    storage_free_bytes(&cluster);
    if (!room_on(storage_device(), (m->size + cluster - 1) / cluster * cluster + ROOM_FOR_RECORDS, rep))
        return INSTALL_NO_SPACE;
    if (plugin)
        return plugin_release(m, has ? &existing : NULL, NULL, rep, phase, progress, ctx);
    char parent[32];
    snprintf(parent, sizeof(parent), "%s/PSP", target());
    sceIoMkdir(parent, 0777);
    sceIoMkdir(GAME_DIR, 0777);
    cJSON *j = begin(m->id, m->dir, "install");
    if (!j)
        return -1;
    g_abort = 0;
    int rc = -2;
    /* An update lays the release over the folder the app is in; a first
       install, or one whose folder somebody deleted, is unpacked apart and
       renamed into place. */
    char prior[64], priorpath[256];
    snprintf(prior, sizeof(prior), "%s", js(j, "prior"));
    snprintf(priorpath, sizeof(priorpath), "%s/%s", GAME_DIR, prior);
    int overlay = prior[0] && storage_exists(priorpath);
    if (prior[0] && !overlay)
        logline("install: PSP/GAME/%s is gone; installed as new", prior);
    if (!cJSON_AddStringToObject(j, "mode", overlay ? "overlay" : "fresh") || journal_save(j) < 0)
        goto end;
    if (phase)
        phase(ctx, "download");
    if (download(m, progress, ctx, got) < 0) {
        if (g_abort)
            rc = INSTALL_CANCELLED;
        goto end;
    }
    struct zipread z;
    if (zip_open(&z, ARCHIVE) < 0)
        goto end;
    char root[200], dir[64];
    struct install_layout layout;
    rc = find_package(&z, m, root, sizeof(root), dir, sizeof(dir), &layout);
    /* Gone by the rule for more than one EBOOT.PBP: nothing is written until
       whoever is installing has seen what goes where. */
    if (rc == 0 && layout.review && g_layout_check &&
        g_layout_check(ctx, &layout) != 0)
        rc = g_abort ? INSTALL_CANCELLED : INSTALL_DECLINED;
    /* From here the journal names the package, and recovery can list what
       an update laid over the folder. */
    if (rc == 0 && (!cJSON_AddStringToObject(j, "root", root) || journal_save(j) < 0))
        rc = -1;
    cluster = 1;
    storage_free_bytes_on(target(), &cluster);
    struct need need = {cluster, ROOM_FOR_RECORDS};
    if (rc == 0 && walk_package(&z, root, count_need, &need) < 0)
        rc = -1;
    if (rc == 0 && !room_on(target(), need.bytes, rep))
        rc = INSTALL_NO_SPACE;
    if (rc == 0 && overlay && walk_package(&z, root, clear_one, priorpath) < 0)
        rc = -1;
    if (rc == 0 && !overlay && sceIoMkdir(STAGE, 0777) < 0)
        rc = -1;
    if (rc == 0) {
        if (phase)
            phase(ctx, "unpack");
        rc = unpack(&z, root, overlay ? priorpath : STAGE, overlay ? SUFFIX_NEW : "", rep, progress,
                    ctx);
    }
    zip_close(&z);
    if (rc < 0 || g_abort) {
        if (g_abort)
            rc = INSTALL_CANCELLED;
        goto end;
    }
    rc = -3;
    if (sceIoSync(target(), 0) < 0 || journal_phase(j, "ready") < 0)
        goto end;
    if (phase)
        phase(ctx, "commit");
    /* Said before it is done: a directory under the name is the stage
       renamed only from here on, and a file under its own name with neither
       copy beside it is one the update added, which is what recovery goes
       by. */
    if (journal_phase(j, "placed") < 0)
        goto end;
    if (overlay) {
        /* An author who moved the app moves the folder, and everything in
           it with it; then the release is laid over it there. */
        char destpath[256];
        snprintf(destpath, sizeof(destpath), "%s/%s", GAME_DIR, dir);
        if (strcmp(prior, dir) && sceIoRename(priorpath, dir) < 0)
            goto end;
        if (zip_open(&z, ARCHIVE) < 0)
            goto end;
        int swapped = walk_package(&z, root, swap_one, destpath);
        zip_close(&z);
        if (swapped < 0)
            goto end;
    } else if (sceIoRename(STAGE, dir) < 0) {
        goto end;
    }
    char path[256];
    storage_app_path(m->id, path, sizeof(path));
    if (storage_write(path, m->raw, strlen(m->raw)) < 0 ||
        state_commit_on(m, dir, got, spec.installdir + 9, target()) < 0)
        goto end;
    if (journal_phase(j, "committed") < 0)
        goto end;
    snprintf(rep->id, sizeof(rep->id), "%s", m->id);
    snprintf(rep->dir, sizeof(rep->dir), "%s", dir);
    snprintf(rep->version, sizeof(rep->version), "%s", m->version);
    rep->rev = m->rev;
    rc = 0;
end:
    /* If a journal write failed, use the last durable phase, not RAM. */
    cJSON_Delete(j);
    install_recover();
    if (storage_exists(JOURNAL))
        return -8;
    return rc;
}
