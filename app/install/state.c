#include "install/state.h"
#include "update/sources.h"
#include "util/runtime.h"
#include "util/storage.h"
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
static cJSON *records;
static int healthy;
static const char *str(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static double number(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}
/* cJSON keeps both members of a key written twice and finds the first,
   while a commit deletes the first and appends its own: the stale second
   would come to the front and be the record. Each member is named once. */
static int unique_members(const cJSON *o) {
    for (const cJSON *a = o ? o->child : NULL; a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next)
            if (a->string && b->string && !strcmp(a->string, b->string))
                return 0;
    return 1;
}
int state_validate(const cJSON *r) {
    if (!cJSON_IsObject(r))
        return 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, r) {
        if (!manifest_id_is_safe(v->string) || !cJSON_IsObject(v))
            return 0;
        for (const cJSON *other = v->next; other; other = other->next)
            if (!strcasecmp(v->string, other->string))
                return 0;
        const cJSON *in = cJSON_GetObjectItemCaseSensitive(v, "installed");
        if (!unique_members(v) || !unique_members(in) ||
            !unique_members(cJSON_GetObjectItemCaseSensitive(v, "latest")))
            return 0;
        if (!cJSON_IsObject(in) || !pspdx_install_dir(str(in, "installdir")) ||
            !manifest_rev_in_range(number(in, "published_at")))
            return 0;
        if (strlen(str(in, "version")) >= VERSION_SIZE)
            return 0;
        /* The folder the .pspdx named at the install, where the record keeps
           one, is a folder like any other. */
        const cJSON *device = cJSON_GetObjectItemCaseSensitive(in, "device");
        if (device && (!cJSON_IsString(device) || !storage_device_valid(device->valuestring)))
            return 0;
        const cJSON *named = cJSON_GetObjectItemCaseSensitive(in, "pspdx_installdir");
        if (named && (!cJSON_IsString(named) || !pspdx_install_dir(named->valuestring)))
            return 0;
        /* A hash of nothing but zeros is no hash anyone computed: a record
           that says so has been written by hand, and is not believed about
           the rest either. */
        const char *sha = str(in, "sha256");
        if (*sha && strspn(sha, "0") == strlen(sha))
            return 0;
        /* A record written while a list's .pspdx could stand in for a
           repository's may still name it in manifest_url. Nothing reads that
           any more, so it is left as it is and never held against the rest. */
        /* A GitHub record is the only one of its repository, under the id
           the repository makes or one a catalog gave it; an io.github. id is
           only ever the repository's own. One from anywhere else has its id
           out of its list and its name, which the record does not carry, so
           an https:// source is all it is asked for: such a record is
           updated through a catalog and never at the origin, and it must not
           make every other record unreadable. */
        struct source_repo repo;
        const char *source = str(v, "source");
        /* db_read keeps the source in PSPDX_URL_SIZE; a longer one would be
           compared cut, as another repository. */
        if (strlen(source) >= PSPDX_URL_SIZE)
            return 0;
        if (sources_parse_repo(source, &repo)) {
            char id[PSPDX_ID_SIZE];
            sources_repo_id(&repo, id, sizeof(id));
            if (!strncmp(v->string, "io.github.", 10) && strcmp(id, v->string))
                return 0;
            for (const cJSON *other = v->next; other; other = other->next)
                if (sources_same_repo(source, str(other, "source")))
                    return 0;
        } else if (strncmp(source, "https://", 8) || !source[8] ||
                   !strncmp(source, "https://github.com/", 19)) {
            return 0;
        }
        const cJSON *check = cJSON_GetObjectItemCaseSensitive(v, "update_check");
        if (check && (!cJSON_IsString(check) ||
            (strcmp(check->valuestring, "auto") && strcmp(check->valuestring, "source"))))
            return 0;
    }
    return 1;
}
static void record_path(const char *id, char *path, size_t size) {
    snprintf(path, size, "%s/%s.state.json", storage_path("PSP/PSPDX/INSTALLED"), id);
}
static cJSON *read_json(const char *path, size_t limit) {
    char *raw = NULL;
    int n = storage_read(path, &raw, limit);
    if (n < 0)
        return NULL;
    /* A NUL inside a string would end it early and leave the record saying
       less than it says: such a record is a damaged one, as in a .pspdx. */
    if (pspdx_json_mark_nul(raw, (size_t)n)) {
        free(raw);
        return NULL;
    }
    cJSON *value = cJSON_ParseWithLengthOpts(raw, n + 1, NULL, 1);
    free(raw);
    return value;
}
static int write_record(const char *id, const cJSON *value) {
    char path[256];
    record_path(id, path, sizeof(path));
    if (!value)
        return storage_remove(path);
    char *raw = cJSON_PrintUnformatted(value);
    if (!raw)
        return -1;
    int rc = storage_write(path, raw, strlen(raw));
    free(raw);
    return rc;
}
int state_load(void) {
    cJSON_Delete(records);
    records = cJSON_CreateObject();
    healthy = 0;
    if (!records)
        goto fail;
    SceUID d = sceIoDopen(storage_path("PSP/PSPDX/INSTALLED"));
    if (d < 0)
        goto fail;
    int rc;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while ((rc = sceIoDread(d, &e)) > 0) {
        char id[PSPDX_ID_SIZE], path[256];
        size_t n = strlen(e.d_name);
        if (n > 4 && !strcmp(e.d_name + n - 4, ".bak"))
            n -= 4;
        const char suffix[] = ".state.json";
        size_t tail = sizeof(suffix) - 1;
        if (n <= tail || strncmp(e.d_name + n - tail, suffix, tail))
            continue;
        if (n - tail >= sizeof(id) || FIO_S_ISDIR(e.d_stat.st_mode)) {
            rc = -1; break;
        }
        memcpy(id, e.d_name, n - tail); id[n - tail] = 0;
        if (!manifest_id_is_safe(id)) { rc = -1; break; }
        /* A .bak and its committed file can both occur in one directory scan. */
        if (cJSON_GetObjectItemCaseSensitive(records, id))
            continue;
        record_path(id, path, sizeof(path));
        cJSON *value = read_json(path, 64 * 1024);
        if (!value || !cJSON_AddItemToObject(records, id, value)) {
            cJSON_Delete(value); rc = -1; break;
        }
        memset(&e, 0, sizeof(e));
    }
    int closed = sceIoDclose(d);
    if (rc < 0 || closed < 0 || !state_validate(records))
        goto fail;
    healthy = 1;
    return 0;
fail:
    logline("state: unreadable or invalid record; writes blocked");
    return -1;
}
int state_ok(void) { return healthy; }
cJSON *state_snapshot(void) { return healthy ? cJSON_Duplicate(records, 1) : NULL; }
int state_restore(const cJSON *snapshot) {
    if (!healthy || !state_validate(snapshot))
        return -1;
    cJSON *next = cJSON_Duplicate(snapshot, 1);
    if (!next)
        return -1;
    const cJSON *r;
    cJSON_ArrayForEach(r, next) {
        const cJSON *old = cJSON_GetObjectItemCaseSensitive(records, r->string);
        if (!cJSON_Compare(old, r, 1) && write_record(r->string, r) < 0)
            goto fail;
    }
    cJSON_ArrayForEach(r, records) {
        if (!cJSON_GetObjectItemCaseSensitive(next, r->string) && write_record(r->string, NULL) < 0)
            goto fail;
    }
    cJSON_Delete(records);
    records = next;
    return 0;
fail:
    cJSON_Delete(next);
    healthy = 0;
    return -1;
}
/* Recovery owns one app only, even for older journals containing a full snapshot. */
int state_restore_app(const cJSON *snapshot, const char *id) {
    if (!state_validate(snapshot) || !manifest_id_is_safe(id))
        return -1;
    cJSON *next = state_snapshot();
    if (!next)
        return -1;
    cJSON_DeleteItemFromObjectCaseSensitive(next, id);
    const cJSON *old = cJSON_GetObjectItemCaseSensitive(snapshot, id);
    if (old) {
        cJSON *copy = cJSON_Duplicate(old, 1);
        if (!copy || !cJSON_AddItemToObject(next, id, copy)) {
            cJSON_Delete(copy); cJSON_Delete(next); return -1;
        }
    }
    int rc = state_restore(next);
    cJSON_Delete(next);
    return rc;
}
int state_count(void) { return healthy ? cJSON_GetArraySize(records) : 0; }
const char *state_id(int i) {
    const cJSON *v = healthy ? cJSON_GetArrayItem(records, i) : NULL;
    return v ? v->string : NULL;
}
int state_read_manifest(const char *id, char **raw, struct pspdx_file *f) {
    *raw = NULL;
    if (!manifest_id_is_safe(id))
        return -1;
    char path[256], why[80];
    storage_app_path(id, path, sizeof(path));
    int n = storage_read(path, raw, PSPDX_FILE_MAX);
    if (n < 0)
        return -1;
    if (pspdx_parse(*raw, n, f, why, sizeof(why)) < 0) {
        free(*raw);
        *raw = NULL;
        return -1;
    }
    /* A file kept under a record is that record's repository's, whatever id a
       catalog gave it; one with no record has to make its id itself. */
    struct installed installed;
    if (db_read(id, &installed) == 0 ? !sources_same_repo(installed.repo, f->source)
                                     : strcmp(id, f->id) != 0) {
        free(*raw);
        *raw = NULL;
        return -1;
    }
    return n;
}
/* 64 hex digits into 32 bytes; left all zeros for anything else. */
static void hex_bytes(const char *hex, unsigned char *out) {
    memset(out, 0, 32);
    if (strlen(hex) != 64 || strspn(hex, "0123456789abcdefABCDEF") != 64)
        return;
    for (int i = 0; i < 32; i++) {
        unsigned byte;
        char pair[3] = {hex[2 * i], hex[2 * i + 1], 0};
        sscanf(pair, "%x", &byte);
        out[i] = (unsigned char)byte;
    }
}
int db_read(const char *id, struct installed *out) {
    if (!healthy)
        return -1;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(records, id);
    if (!r)
        return -1;
    const cJSON *in = cJSON_GetObjectItemCaseSensitive(r, "installed");
    memset(out, 0, sizeof(*out));
    snprintf(out->device, sizeof(out->device), "%s",
             *str(in, "device") ? str(in, "device") : storage_device());
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->dir, sizeof(out->dir), "%s", str(in, "installdir") + 9);
    snprintf(out->version, sizeof(out->version), "%s", str(in, "version"));
    out->rev = number(in, "published_at");
    hex_bytes(str(in, "sha256"), out->sha256);
    snprintf(out->repo, sizeof(out->repo), "%s", str(r, "source"));
    const char *named = str(in, "pspdx_installdir");
    snprintf(out->file_dir, sizeof(out->file_dir), "%s", *named ? named + 9 : "");
    return 0;
}
static cJSON *latest_json(const struct manifest *m) {
    cJSON *r = cJSON_CreateObject();
    if (!r)
        return NULL;
    cJSON_AddStringToObject(r, "version", m->version);
    cJSON_AddNumberToObject(r, "published_at", m->rev);
    cJSON_AddStringToObject(r, "download_url", m->url);
    cJSON_AddNumberToObject(r, "size", m->size);
    if (manifest_has_sha256(m)) {
        char hex[65];
        for (int i = 0; i < 32; i++)
            sprintf(hex + 2 * i, "%02x", m->sha256[i]);
        cJSON_AddStringToObject(r, "sha256", hex);
    }
    cJSON_AddNumberToObject(r, "checked_at", m->checked_at);
    cJSON_AddStringToObject(r, "checked_from", m->checked_from);
    if (m->pinned)
        cJSON_AddTrueToObject(r, "pinned");
    return r;
}
int state_commit(const struct manifest *m, const char *dir, const unsigned char *sha256,
                 const char *file_dir) {
    struct installed rec;
    const char *dev = db_read(m->id, &rec) == 0 ? rec.device : storage_device();
    return state_commit_on(m, dir, sha256, file_dir, dev);
}
int state_commit_on(const struct manifest *m, const char *dir, const unsigned char *sha256,
                    const char *file_dir, const char *dev) {
    if (!storage_device_valid(dev) || !healthy || !manifest_dir_is_safe(dir) || (file_dir && !manifest_dir_is_safe(file_dir)))
        return -1;
    cJSON *next = state_snapshot();
    if (!next)
        return -1;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(next, m->id);
    /* What the record said the .pspdx named, for a caller that does not
       know: the record PSPDX rewrites of itself. */
    char kept[64] = "";
    const char *was = str(cJSON_GetObjectItemCaseSensitive(r, "installed"), "pspdx_installdir");
    if (!file_dir && *was)
        snprintf(kept, sizeof(kept), "%s", was + 9);
    if (!r) {
        r = cJSON_AddObjectToObject(next, m->id);
        if (!r) {
            cJSON_Delete(next);
            return -1;
        }
        cJSON_AddStringToObject(r, "added_from", m->added_from[0] ? m->added_from : m->repo);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(r, "source");
    cJSON_AddStringToObject(r, "source", m->repo);
    cJSON_DeleteItemFromObjectCaseSensitive(r, "installed");
    cJSON *in = cJSON_AddObjectToObject(r, "installed");
    char target[64];
    snprintf(target, sizeof(target), "PSP/GAME/%s", dir);
    cJSON_AddStringToObject(in, "version", m->version);
    cJSON_AddNumberToObject(in, "published_at", m->rev);
    cJSON_AddStringToObject(in, "installdir", target);
    cJSON_AddStringToObject(in, "device", dev);
    if (file_dir || kept[0]) {
        char named[64];
        snprintf(named, sizeof(named), "PSP/GAME/%.32s", file_dir ? file_dir : kept);
        cJSON_AddStringToObject(in, "pspdx_installdir", named);
    }
    /* The zip that is on the stick now, by its hash: an update is a zip
       with another one, whatever date it was published on. */
    int hashed = 0;
    for (int i = 0; sha256 && i < 32; i++)
        hashed |= sha256[i];
    if (hashed) {
        char hex[65];
        for (int i = 0; i < 32; i++)
            sprintf(hex + 2 * i, "%02x", sha256[i]);
        cJSON_AddStringToObject(in, "sha256", hex);
    }
    if (m->url[0]) {
        cJSON_DeleteItemFromObjectCaseSensitive(r, "latest");
        cJSON_AddItemToObject(r, "latest", latest_json(m));
    }
    int rc = state_restore(next);
    cJSON_Delete(next);
    return rc;
}
int db_write_record(const struct installed *r) {
    struct manifest m = {0};
    snprintf(m.id, sizeof(m.id), "%s", r->id);
    snprintf(m.repo, sizeof(m.repo), "%s", r->repo);
    snprintf(m.version, sizeof(m.version), "%s", r->version);
    m.rev = r->rev;
    return state_commit_on(&m, r->dir, r->sha256, NULL,
                           r->device[0] ? r->device : storage_device());
}
int state_note_latest(const struct manifest *m) {
    if (!healthy)
        return -1;
    if (!m->checked_at)
        return 0;
    cJSON *next = state_snapshot();
    if (!next)
        return -1;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(next, m->id);
    if (!r) {
        cJSON_Delete(next);
        return 0;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(r, "latest");
    cJSON_AddItemToObject(r, "latest", latest_json(m));
    int rc = state_restore(next);
    cJSON_Delete(next);
    return rc;
}
int state_forget(const char *id) {
    cJSON *next = state_snapshot();
    if (!next)
        return -1;
    cJSON_DeleteItemFromObjectCaseSensitive(next, id);
    int rc = state_restore(next);
    cJSON_Delete(next);
    return rc;
}
int state_note_file_dir(const char *id, const char *file_dir) {
    struct installed rec;
    if (!healthy || db_read(id, &rec) < 0 || rec.file_dir[0] || !manifest_dir_is_safe(file_dir))
        return !healthy ? -1 : 0;
    cJSON *next = state_snapshot();
    if (!next)
        return -1;
    cJSON *in = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(next, id), "installed");
    char named[64];
    snprintf(named, sizeof(named), "PSP/GAME/%.32s", file_dir);
    int rc = cJSON_AddStringToObject(in, "pspdx_installdir", named) ? state_restore(next) : -1;
    cJSON_Delete(next);
    return rc;
}
int state_retire_legacy(const char *id, const char *source, const char *dir) {
    struct installed rec;
    if (!healthy || db_read(id, &rec) < 0 || !sources_same_repo(rec.repo, source) ||
        strcasecmp(rec.dir, dir))
        return 0;
    char path[256];
    storage_app_path(id, path, sizeof(path));
    if (state_forget(id) < 0 || storage_remove(path) < 0)
        return -1;
    return 1;
}
int state_target_owner(const char *dir, const char *id) {
    struct installed rec;
    return state_target_owner_on(dir, id, db_read(id, &rec) == 0 ? rec.device : storage_device());
}
int state_target_owner_on(const char *dir, const char *id, const char *dev) {
    if (!healthy)
        return -1;
    const cJSON *r;
    cJSON_ArrayForEach(r, records) {
        const cJSON *in = cJSON_GetObjectItemCaseSensitive(r, "installed");
        const char *owner_device = *str(in, "device") ? str(in, "device") : storage_device();
        if (!strcmp(owner_device, dev) && strcasecmp(r->string, id) && !strcasecmp(str(in, "installdir") + 9, dir))
            return 1;
    }
    return 0;
}
int state_latest(const char *id, struct manifest *m) {
    if (!healthy)
        return -1;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(records, id);
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(r, "latest");
    if (!cJSON_IsObject(l))
        return -1;
    memset(m, 0, sizeof(*m));
    /* A field longer than its place is refused, not cut: a cut address is
       another address, and a cut version can end inside a character. */
    if (strlen(str(r, "source")) >= sizeof(m->repo) ||
        strlen(str(r, "added_from")) >= sizeof(m->added_from) ||
        strlen(str(l, "version")) >= sizeof(m->version) ||
        strlen(str(l, "download_url")) >= sizeof(m->url) ||
        strlen(str(l, "checked_from")) >= sizeof(m->checked_from))
        return -1;
    snprintf(m->id, sizeof(m->id), "%s", id);
    snprintf(m->repo, sizeof(m->repo), "%s", str(r, "source"));
    snprintf(m->added_from, sizeof(m->added_from), "%s", str(r, "added_from"));
    if (!manifest_rev_in_range(number(l, "published_at")) ||
        !manifest_size_in_range(number(l, "size")))
        return -1;
    m->rev = number(l, "published_at");
    m->size = number(l, "size");
    snprintf(m->version, sizeof(m->version), "%s", str(l, "version"));
    snprintf(m->url, sizeof(m->url), "%s", str(l, "download_url"));
    if (strncmp(m->url, "https://", 8))
        return -1;
    snprintf(m->checked_from, sizeof(m->checked_from), "%s", str(l, "checked_from"));
    double checked = number(l, "checked_at");
    m->checked_at = manifest_rev_in_range(checked) ? checked : 0;
    m->pinned = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(l, "pinned"));
    const char *hex = str(l, "sha256");
    if (*hex) {
        if (strlen(hex) != 64 || strspn(hex, "0") == 64)
            return -1;
        for (int i = 0; i < 32; i++) {
            unsigned byte;
            char pair[3] = {hex[2 * i], hex[2 * i + 1], 0};
            if (strspn(pair, "0123456789abcdefABCDEF") != 2 || sscanf(pair, "%x", &byte) != 1)
                return -1;
            m->sha256[i] = byte;
        }
    }
    return 0;
}
