#include "install/state.h"
#include "update/pspdx.h"
#include "update/sources.h"
#include "util/storage.h"
#include <strings.h>
/*
 * Installing a package: download, check, unpack, one rename.
 *
 * Nothing is created in place. FAT32 has no transactions and PSP users pull
 * the battery, so the archive is downloaded and unpacked under
 * PSP/PSPDX/tmp/ and only a finished directory is renamed into
 * PSP/GAME/. The rename is the commit.
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
#include "util/runtime.h"

#define TMP_DIR storage_path("PSP/PSPDX/TMP")
#define GAME_DIR storage_path("PSP/GAME")
#define ARCHIVE storage_path("PSP/PSPDX/TMP/download.zip")
#define STAGE storage_path("PSP/GAME/.pspdx-stage")
#define JOURNAL storage_path("PSP/PSPDX/TMP/transaction.json")

/* A Memory Stick tops out at 32 GB and no homebrew is anywhere near this.
   The number exists so that a size field cannot ask for something absurd. */
/* -------------------------------------------------------------- release */

/* An id becomes a file name, so it may not carry a path. Reverse-DNS letters,
   digits, dot, dash and underscore only. */
int manifest_id_is_safe(const char *id) {
    if (!id || !*id || strlen(id) > 80)
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

int manifest_dir_is_safe(const char *dir) {
    char path[64];
    if (!dir || strlen(dir) > 32)
        return 0;
    snprintf(path, sizeof(path), "PSP/GAME/%s", dir);
    return pspdx_install_dir(path);
}

int manifest_root_is_safe(const char *root) {
    if (!root || strlen(root) >= 200)
        return 0;
    if (root[0] == '/' || root[0] == '\\')
        return 0;
    if (strstr(root, "..") || strchr(root, ':'))
        return 0;
    return 1;
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

static int download(const struct manifest *m, https_progress progress, void *pctx) {
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

static int rm_rf(const char *path) {
    SceUID d = sceIoDopen(path);
    if (d < 0)
        return sceIoRemove(path) < 0 ? -1 : 0;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (strcmp(e.d_name, ".") == 0 || strcmp(e.d_name, "..") == 0)
            continue;
        char sub[256];
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

/* Everything under the package root goes into the staging directory;
   what the archive holds beside the package -- a readme at the top, a
   source tree -- stays in the archive. */
static int unpack(struct zipread *z, const char *root, struct install_report *rep,
                  https_progress progress, void *pctx) {
    size_t plen = strlen(root);
    struct zipentry e;
    int rc, files = 0;
    size_t total = 0, done = 0;

    for (rc = zip_first(z, &e); rc > 0; rc = zip_next(z, &e)) {
        slashes(e.name);
        if (strncmp(e.name, root, plen) == 0) {
            if (e.usize > MAX_PACKAGE_BYTES - total)
                return -1;
            total += e.usize;
        }
    }
    if (rc < 0)
        return -1;
    if (progress)
        progress(pctx, 0, total);

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

        char path[256];
        if (snprintf(path, sizeof(path), "%s/%s", STAGE, rel) >= (int)sizeof(path)) {
            logline("unpack: path too long: %s", rel);
            return -1;
        }
        size_t L = strlen(path);
        if (path[L - 1] == '/') {
            path[L - 1] = '\0';
            mkdir_p(path);
            continue;
        }

        char *slash = strrchr(path, '/');
        if (slash) {
            *slash = '\0';
            mkdir_p(path);
            *slash = '/';
        }

        struct out_file o;
        o.done = &done;
        o.fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
        if (o.fd < 0) {
            logline("unpack: cannot create %s", rel);
            return -1;
        }
        int xrc = zip_extract(z, &e, out_sink, &o);
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
        files++;
        if (progress)
            progress(pctx, done, total);
    }
    if (rc < 0)
        return -1;

    rep->files = files;
    rep->bytes = done;
    logline("unpack: %d files, %lu bytes", files, (unsigned long)done);
    return 0;
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
    int rc = storage_write(JOURNAL, raw, strlen(raw));
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
    if (cJSON_IsString(old))
        return storage_write(path, old->valuestring, strlen(old->valuestring));
    return storage_remove(path);
}
static int recover_journal(cJSON *j) {
    const char *id = js(j, "id"), *dir = js(j, "dir"), *prior = js(j, "prior"),
               *phase = js(j, "phase"), *op = js(j, "op");
    if (!manifest_id_is_safe(id) || !manifest_dir_is_safe(dir) ||
        (*prior && !manifest_dir_is_safe(prior)))
        return -1;
    const cJSON *snapshot = cJSON_GetObjectItemCaseSensitive(j, "old_state");
    if (!state_validate(snapshot))
        return -1;
    const cJSON *previous = cJSON_GetObjectItemCaseSensitive(snapshot, id);
    const cJSON *in = cJSON_GetObjectItemCaseSensitive(previous, "installed");
    if ((*prior &&
         strcmp(js(in, "installdir") + (!strncmp(js(in, "installdir"), "PSP/GAME/", 9) ? 9 : 0),
                prior)) ||
        (!*prior && previous))
        return -1;
    const cJSON *other;
    cJSON_ArrayForEach(other, snapshot) {
        const cJSON *oi = cJSON_GetObjectItemCaseSensitive(other, "installed");
        if (strcmp(other->string, id) && !strcasecmp(js(oi, "installdir") + 9, dir))
            return -1;
    }
    if (strcmp(op, "install") && strcmp(op, "remove"))
        return -1;
    if (strcmp(phase, "prepared") && strcmp(phase, "ready") && strcmp(phase, "committed"))
        return -1;
    char dest[256], old[256], priorpath[256];
    snprintf(dest, sizeof(dest), "%s/%s", GAME_DIR, dir);
    snprintf(priorpath, sizeof(priorpath), "%s/%s", GAME_DIR, *prior ? prior : dir);
    snprintf(old, sizeof(old), "%s/%s.old", GAME_DIR, dir);
    int committed = !strcmp(phase, "committed");
    if (!committed) {
        if (strcmp(phase, "prepared")) {
            if (storage_exists(old)) {
                if (strcmp(op, "remove") && remove_tree(dest) < 0)
                    return -1;
                if (sceIoRename(old, *prior ? prior : dir) < 0)
                    return -1;
            } else if (!*prior && !storage_exists(STAGE) && !strcmp(op, "install")) {
                if (remove_tree(dest) < 0)
                    return -1;
            }
        }
        if (restore_manifest(j) < 0 ||
            state_restore_app(cJSON_GetObjectItemCaseSensitive(j, "old_state"), id) < 0)
            return -1;
    }
    if (remove_tree(STAGE) < 0)
        return -1;
    if (committed && remove_tree(old) < 0)
        return -1;
    if (storage_remove(ARCHIVE) < 0)
        return -1;
    return storage_remove(JOURNAL);
}
void install_recover(void) {
    char *raw = NULL;
    int n = storage_read(JOURNAL, &raw, 512 * 1024);
    if (n < 0)
        return;
    cJSON *j = cJSON_ParseWithLengthOpts(raw, n + 1, NULL, 1);
    free(raw);
    if (!j || recover_journal(j) < 0)
        logline("recovery: unfinished transaction; installation blocked");
    cJSON_Delete(j);
}
static cJSON *begin(const char *id, const char *dir, const char *op) {
    install_recover();
    if (!state_ok() || storage_exists(JOURNAL) || storage_exists(STAGE)) {
        logline("install: unresolved state or staging directory");
        return NULL;
    }
    struct installed rec;
    int has = db_read(id, &rec) == 0;
    char dest[256], backup[256];
    snprintf(dest, sizeof(dest), "%s/%s", GAME_DIR, dir);
    snprintf(backup, sizeof(backup), "%s/%s.old", GAME_DIR, dir);
    if (state_target_owner(dir, id) != 0 || storage_exists(backup) ||
        (storage_exists(dest) && (!has || strcasecmp(rec.dir, dir)))) {
        logline("install: target or backup belongs to another installation");
        return NULL;
    }
    cJSON *j = cJSON_CreateObject();
    if (!j)
        return NULL;
    cJSON_AddStringToObject(j, "id", id);
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
static int move_old(cJSON *j) {
    const char *prior = js(j, "prior");
    if (!*prior)
        return 0;
    char path[256], name[80];
    snprintf(path, sizeof(path), "%s/%s", GAME_DIR, prior);
    snprintf(name, sizeof(name), "%s.old", js(j, "dir"));
    if (!storage_exists(path)) {
        logline("install: recorded app directory is missing");
        return -1;
    }
    return sceIoRename(path, name);
}
int uninstall(const char *id) {
    struct installed rec;
    if (!manifest_id_is_safe(id) || db_read(id, &rec) < 0)
        return -1;
    cJSON *j = begin(id, rec.dir, "remove");
    if (!j)
        return -1;
    int rc = -1;
    char path[256];
    storage_app_path(id, path, sizeof(path));
    if (journal_phase(j, "ready") < 0 || move_old(j) < 0)
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
int install_release(const struct manifest *m, struct install_report *rep, install_phase_cb phase,
                    https_progress progress, void *ctx) {
    memset(rep, 0, sizeof(*rep));
    struct pspdx_file spec;
    char why[80];
    if (!manifest_id_is_safe(m->id) || !manifest_dir_is_safe(m->dir) ||
        !manifest_size_in_range(m->size) || !manifest_rev_in_range(m->rev) ||
        strncmp(m->url, "https://", 8))
        return -1;
    if (pspdx_parse(m->raw, strlen(m->raw), &spec, why, sizeof(why)) < 0 ||
        strcmp(spec.installdir + 9, m->dir)) {
        logline("install: valid original manifest required");
        return -1;
    }
    struct source_repo repo;
    char derived[96];
    if (!sources_parse_repo(spec.source, &repo) || !sources_same_url(spec.source, m->repo))
        return -1;
    sources_repo_id(&repo, derived, sizeof(derived));
    if (strcmp(derived, m->id))
        return -1;
    struct installed existing;
    if(db_read(m->id,&existing)==0 && !sources_same_url(existing.repo,m->repo))return -1;
    if (!sources_release_url(m->repo, m->url))
        return -1;
    cJSON *j = begin(m->id, m->dir, "install");
    if (!j)
        return -1;
    int rc = -2;
    if (phase)
        phase(ctx, "download");
    if (download(m, progress, ctx) < 0)
        goto end;
    struct zipread z;
    if (zip_open(&z, ARCHIVE) < 0)
        goto end;
    char root[200], dir[64];
    rc = find_package(&z, m, root, sizeof(root), dir, sizeof(dir));
    if (rc == 0) {
        if (sceIoMkdir(STAGE, 0777) < 0)
            rc = -1;
        else {
            if (phase)
                phase(ctx, "unpack");
            rc = unpack(&z, root, rep, progress, ctx);
        }
    }
    zip_close(&z);
    if (rc < 0)
        goto end;
    rc = -3;
    if (sceIoSync(storage_device(), 0) < 0 || journal_phase(j, "ready") < 0)
        goto end;
    if (phase)
        phase(ctx, "commit");
    if (move_old(j) < 0)
        goto end;
    if (sceIoRename(STAGE, dir) < 0)
        goto end;
    char path[256];
    storage_app_path(m->id, path, sizeof(path));
    if (storage_write(path, m->raw, strlen(m->raw)) < 0 || state_commit(m, dir) < 0)
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
