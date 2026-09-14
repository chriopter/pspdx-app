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
        if (!cJSON_IsObject(in) || !pspdx_install_dir(str(in, "installdir")) ||
            !manifest_rev_in_range(number(in, "published_at")))
            return 0;
        if (strlen(str(in, "version")) >= 32)
            return 0;
        struct source_repo repo;
        if (!sources_parse_repo(str(v, "source"), &repo))
            return 0;
        char id[96];
        sources_repo_id(&repo, id, sizeof(id));
        if (strcmp(id, v->string))
            return 0;
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
        char id[96], path[256];
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
    struct source_repo repo;
    char expected[96];
    sources_parse_repo(f->source, &repo);
    sources_repo_id(&repo, expected, sizeof(expected));
    struct installed installed;
    if (strcmp(id, expected) || (db_read(id,&installed)==0 && !sources_same_repo(installed.repo,f->source))) {
        free(*raw);
        *raw = NULL;
        return -1;
    }
    return n;
}
int db_read(const char *id, struct installed *out) {
    if (!healthy)
        return -1;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(records, id);
    if (!r)
        return -1;
    const cJSON *in = cJSON_GetObjectItemCaseSensitive(r, "installed");
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->dir, sizeof(out->dir), "%s", str(in, "installdir") + 9);
    snprintf(out->version, sizeof(out->version), "%s", str(in, "version"));
    out->rev = number(in, "published_at");
    snprintf(out->repo, sizeof(out->repo), "%s", str(r, "source"));
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
    return r;
}
int state_commit(const struct manifest *m, const char *dir) {
    if (!healthy || !manifest_dir_is_safe(dir))
        return -1;
    cJSON *next = state_snapshot();
    if (!next)
        return -1;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(next, m->id);
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
    return state_commit(&m, r->dir);
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
int state_target_owner(const char *dir, const char *id) {
    if (!healthy)
        return -1;
    const cJSON *r;
    cJSON_ArrayForEach(r, records) {
        const cJSON *in = cJSON_GetObjectItemCaseSensitive(r, "installed");
        if (strcasecmp(r->string, id) && !strcasecmp(str(in, "installdir") + 9, dir))
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
    const char *hex = str(l, "sha256");
    if (*hex) {
        if (strlen(hex) != 64)
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
