#include "install/state.h"
#include "update/pspdx.h"
#include "update/sources.h"
#include "util/storage.h"
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

/* ------------------------------------------------------------- download */

struct dl {
    int fd;
    wc_Sha256 sha;
    size_t written, expected;
};

static int file_sink(void *ctx, const void *data, size_t len) {
    struct dl *d = ctx;
    if (len > d->expected - d->written)
        return -1;
    if (wc_Sha256Update(&d->sha, data, (word32)len) != 0)
        return -1;
    while (len) {
        int n = sceIoWrite(d->fd, data, len);
        if (n <= 0) {
            logline("write failed %d", n);
            return -1;
        }
        data = (const char *)data + n;
        len -= (size_t)n;
        d->written += (size_t)n;
    }
    return 0;
}

/* got is the SHA-256 of what arrived, whether or not the release named one,
   so that the record can say which zip is on the stick. */
static int download(const struct manifest *m, https_progress progress, void *pctx,
                    unsigned char *got) {
    struct dl d;
    struct https_result r;
    d.written = 0;
    d.expected = m->size;
    if (wc_InitSha256(&d.sha) != 0)
        return -1;

    d.fd = sceIoOpen(ARCHIVE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (d.fd < 0) {
        logline("cannot create %s", ARCHIVE);
        return -2;
    }

    unsigned start = now_ms();
    int rc = https_get(m->url, file_sink, &d, progress, pctx, &r);
    int close_rc = sceIoClose(d.fd);
    unsigned ms = now_ms() - start;
    logline("download: rc=%d status=%ld %lu bytes in %u.%us", rc, r.status,
            (unsigned long)d.written, ms / 1000, (ms % 1000) / 100);
    if (rc != 0 || r.status != 200 || close_rc < 0)
        return -3;
    if (d.written != m->size) {
        logline("download: size %lu, release says %lu", (unsigned long)d.written,
                (unsigned long)m->size);
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

static int find_package(struct zipread *z, const struct manifest *m, char *root, size_t rootsz,
                        char *dir, size_t dirsz) {
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
    int rc, count = 0;
    root[0] = 0;
    if (!manifest_dir_is_safe(m->dir))
        return -1;
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
        if (!ends_with_eboot(e.name))
            continue;
        if (++count > 1) {
            logline("zip: more than one EBOOT.PBP");
            return -1;
        }
        const char *slash = strrchr(e.name, '/');
        size_t n = slash ? (size_t)(slash - e.name + 1) : 0;
        if (n >= rootsz)
            return -1;
        memcpy(root, e.name, n);
        root[n] = 0;
    }
    if (rc < 0 || count != 1) {
        logline("zip: expected exactly one EBOOT.PBP");
        return -1;
    }
    snprintf(dir, dirsz, "%s", m->dir);
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
        if (*rel == '\0')
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
static volatile int g_abort;

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

static int recover_journal(cJSON *j) {
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
    cJSON *j = cJSON_CreateObject();
    if (!j)
        return NULL;
    cJSON_AddStringToObject(j, "id", id);
    cJSON_AddStringToObject(j, "device", target());
    cJSON_AddStringToObject(j, "dir", dir);
    cJSON_AddStringToObject(j, "prior", has ? rec.dir : "");
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
    if (journal_save(j) < 0) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}
int uninstall(const char *id) {
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
int install_release(const struct manifest *m, struct install_report *rep, install_phase_cb phase,
                    https_progress progress, void *ctx) {
    return install_release_to(m, storage_device(), rep, phase, progress, ctx);
}
int install_release_to(const struct manifest *m, const char *dev, struct install_report *rep,
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
    if (!manifest_id_is_safe(m->id) || !manifest_dir_is_safe(m->dir) ||
        !manifest_size_in_range(m->size) || !manifest_rev_in_range(m->rev) ||
        strncmp(m->url, "https://", 8))
        return -1;
    if (!m->raw || pspdx_parse(m->raw, strlen(m->raw), &spec, why, sizeof(why)) < 0) {
        logline("install: valid original manifest required");
        return -1;
    }
    /* A plugin or an ISO is listed and not installed: neither goes under
       PSP/GAME, and nothing here knows where either does go. */
    if (!pspdx_type_installable(spec.type)) {
        logline("install: type %s cannot be installed yet", spec.type);
        return -1;
    }
    /* The id is the one the file makes, or one a catalog gave the entry;
       either way the file is the release's repository's, and the id is no
       other repository's on the stick. */
    if (!sources_same_repo(spec.source, m->repo))
        return -1;
    struct installed existing;
    int has = db_read(m->id, &existing) == 0;
    if (has && !sources_same_repo(existing.repo, m->repo))
        return -1;
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
    rc = find_package(&z, m, root, sizeof(root), dir, sizeof(dir));
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
