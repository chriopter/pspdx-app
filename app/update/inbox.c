#include "update/inbox.h"
#include "update/pspdx.h"
#include "update/sources.h"
#include "util/runtime.h"
#include "util/storage.h"
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
struct item {
    char path[512], id[PSPDX_ID_SIZE];
    char *raw;
    int index, conflict;
};
static struct item items[MAX_APPS];
static int files, queue[MAX_APPS], count;
static char summary[96];
int inbox_count(void) { return count; }
int inbox_index(int n) { return n >= 0 && n < count ? items[queue[n]].index : -1; }
const char *inbox_summary(void) { return summary; }
void inbox_installed(int n) {
    if (n < 0 || n >= count)
        return;
    struct item *one = &items[queue[n]];
    for (int i = 0; i < files; i++)
        if (!items[i].conflict && !strcmp(items[i].id, one->id)) {
            /* Do not remove a file changed on the stick after confirmation. */
            char *current = NULL;
            int len = storage_read(items[i].path, &current, PSPDX_FILE_MAX);
            if (len >= 0 && !strcmp(current, items[i].raw))
                storage_remove(items[i].path);
            free(current);
        }
}
int inbox_scan(struct catalog *catalog) {
    for (int i = 0; i < files; i++)
        free(items[i].raw);
    memset(items, 0, sizeof(items));
    files = count = 0;
    summary[0] = 0;
    SceUID d = sceIoDopen(storage_path("PSP/PSPDX/INBOX"));
    if (d < 0)
        return -1;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        size_t len = strlen(e.d_name);
        if (FIO_S_ISDIR(e.d_stat.st_mode) || len < 6 || strcasecmp(e.d_name + len - 6, ".pspdx"))
            goto next;
        if (files >= MAX_APPS) {
            logline("INBOX: capacity reached; remaining files kept");
            break;
        }
        struct item *it = &items[files];
        snprintf(it->path, sizeof(it->path), "%s/%s", storage_path("PSP/PSPDX/INBOX"), e.d_name);
        int n = storage_read(it->path, &it->raw, PSPDX_FILE_MAX);
        struct pspdx_file spec;
        char why[80];
        if (n < 0 || pspdx_parse(it->raw, n, &spec, why, sizeof(why)) < 0) {
            logline("INBOX: invalid %s", e.d_name);
            free(it->raw);
            it->raw = NULL;
            goto next;
        }
        /* Only what this version can install is queued. The file stays in
           INBOX for a version that can. */
        if (!pspdx_type_installable(spec.type) || strncmp(spec.source, "https://github.com/", 19)) {
            logline("INBOX: %s is %s, which this version cannot install yet; kept", e.d_name,
                    pspdx_type_installable(spec.type) ? "from outside GitHub" : spec.type);
            free(it->raw);
            it->raw = NULL;
            goto next;
        }
        snprintf(it->id, sizeof(it->id), "%s", spec.id);
        for (int i = 0; i < files; i++)
            if (!strcmp(items[i].id, it->id) && strcmp(items[i].raw, it->raw)) {
                items[i].conflict = it->conflict = 1;
            }
        files++;
    next:
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    for (int i = 0; i < files; i++) {
        struct item *it = &items[i];
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (!strcmp(items[j].id, it->id))
                dup = 1;
        if (it->conflict || dup) {
            if (it->conflict)
                logline("INBOX: conflicting manifests for %s", it->id);
            continue;
        }
        struct pspdx_file spec;
        char why[80];
        pspdx_parse(it->raw, strlen(it->raw), &spec, why, sizeof(why));
        int at = catalog_add_repo(catalog, spec.source);
        if (at < 0) {
            logline("INBOX: source unavailable for %s", it->id);
            continue;
        }
        struct app_entry *entry = &catalog->apps[at];
        if (!sources_same_repo(entry->repo, spec.source)) {
            logline("INBOX: identity collision");
            continue;
        }
        /* A file that pins a release is installed from that release or not at
           all: the source's word on which zip is current does not stand in. */
        const char *pinned = spec.release_tag + (spec.release_tag[0] == 'v' && spec.release_tag[1]);
        if (spec.release_tag[0] &&
            (strcmp(entry->release.version, pinned) ||
             (spec.release_url[0] && strcmp(entry->release.url, spec.release_url)))) {
            logline("INBOX: %s pins release %s, which its source does not offer; kept", it->id,
                    spec.release_tag);
            continue;
        }
        /* A supplied manifest may have a different target from the cache. */
        if (manifest_keep_raw(&entry->release, it->raw, strlen(it->raw)) < 0) {
            logline("INBOX: no memory for %s", it->id);
            continue;
        }
        free(entry->description);
        entry->description = pspdx_description(it->raw, strlen(it->raw));
        snprintf(entry->release.dir, sizeof(entry->release.dir), "%.32s", spec.installdir + 9);
        snprintf(entry->release.added_from, sizeof(entry->release.added_from), "INBOX");
        snprintf(entry->name, sizeof(entry->name), "%s", spec.name);
        it->index = at;
        queue[count++] = i;
    }
    for (int i = 0; i < count - 1; i++)
        if (!strcmp(items[queue[i]].id, PSPDX_SELF_ID)) {
            int self = queue[i];
            memmove(queue + i, queue + i + 1, (count - i - 1) * sizeof(int));
            queue[count - 1] = self;
            break;
        }
    for (int i = 0; i < count; i++) {
        size_t n = strlen(summary);
        const char *name = catalog->apps[items[queue[i]].index].name;
        /* Room is kept for the ellipsis after every name that has another
           after it, so it is never cut inside its three bytes. */
        size_t need = (i ? 2 : 0) + strlen(name) + (i + 1 < count ? sizeof(" …") - 1 : 0);
        if (n + need >= sizeof(summary)) {
            memcpy(summary + n, " …", sizeof(" …"));
            break;
        }
        if (i) {
            memcpy(summary + n, ", ", 2);
            n += 2;
        }
        memcpy(summary + n, name, strlen(name) + 1);
    }
    return count;
}
