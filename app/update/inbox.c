#include "update/inbox.h"
#include "update/pspdx.h"
#include "update/sources.h"
#include "util/runtime.h"
#include "util/storage.h"
#include <pspiofilemgr.h>
#include <psputils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
/* The .pspdx files in INBOX, read in two passes: the first reads every file
   there, up to INBOX_FILES, and keeps its id and a digest, so that two files
   that say different things of one id are both held back; the second asks
   for each id's app, and only a file that turns out installable takes one of
   the MAX_APPS places, with its text. A folder of files for repositories that
   are gone does not use the places up. */
#define INBOX_FILES 256
struct item {
    char name[256], id[PSPDX_ID_SIZE];
    unsigned char digest[20];
    int conflict;
};
struct queued {
    int item, index;
    char *raw;
};
static struct item items[INBOX_FILES];
static struct queued queue[MAX_APPS];
static int files, count;
static char summary[96];
int inbox_count(void) { return count; }
int inbox_index(int n) { return n >= 0 && n < count ? queue[n].index : -1; }
const char *inbox_summary(void) { return summary; }
static void path_of(const struct item *it, char *out, size_t size) {
    snprintf(out, size, "%.250s/%.255s", storage_path("PSP/PSPDX/INBOX"), it->name);
}
void inbox_installed(int n) {
    if (n < 0 || n >= count)
        return;
    const struct queued *one = &queue[n];
    for (int i = 0; i < files; i++)
        if (!items[i].conflict && !strcmp(items[i].id, items[one->item].id)) {
            /* Do not remove a file changed on the stick after confirmation. */
            char path[512], *current = NULL;
            path_of(&items[i], path, sizeof(path));
            int len = storage_read(path, &current, PSPDX_FILE_MAX);
            if (len >= 0 && !strcmp(current, one->raw))
                storage_remove(path);
            free(current);
        }
}
/* The file's text into raw, held to v1 and to what this version installs.
   0, or -1 with raw freed. */
static int read_one(const char *name, char **raw, struct pspdx_file *spec, int quiet) {
    char path[512], why[80];
    snprintf(path, sizeof(path), "%.250s/%.255s", storage_path("PSP/PSPDX/INBOX"), name);
    int n = storage_read(path, raw, PSPDX_FILE_MAX);
    if (n < 0 || pspdx_parse(*raw, n, spec, why, sizeof(why)) < 0) {
        if (!quiet)
            logline("INBOX: invalid %s", name);
        goto bad;
    }
    /* Only what this version can install is queued. The file stays in
       INBOX for a version that can. */
    if (!pspdx_type_installable(spec->type) || strncmp(spec->source, "https://github.com/", 19)) {
        if (!quiet)
            logline("INBOX: %s is %s, which this version cannot install yet; kept", name,
                    pspdx_type_installable(spec->type) ? "from outside GitHub" : spec->type);
        goto bad;
    }
    return 0;
bad:
    free(*raw);
    *raw = NULL;
    return -1;
}
int inbox_scan(struct catalog *catalog) {
    for (int i = 0; i < count; i++)
        free(queue[i].raw);
    memset(items, 0, sizeof(items));
    memset(queue, 0, sizeof(queue));
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
        if (files >= INBOX_FILES) {
            logline("INBOX: more than %d files; the rest are kept for later", INBOX_FILES);
            break;
        }
        struct item *it = &items[files];
        char *raw = NULL;
        struct pspdx_file spec;
        if (len >= sizeof(it->name) || read_one(e.d_name, &raw, &spec, 0) < 0)
            goto next;
        snprintf(it->name, sizeof(it->name), "%s", e.d_name);
        snprintf(it->id, sizeof(it->id), "%s", spec.id);
        sceKernelUtilsSha1Digest((unsigned char *)raw, strlen(raw), it->digest);
        free(raw);
        for (int i = 0; i < files; i++)
            if (!strcmp(items[i].id, it->id) && memcmp(items[i].digest, it->digest, 20))
                items[i].conflict = it->conflict = 1;
        files++;
    next:
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    int i;
    for (i = 0; i < files && count < MAX_APPS; i++) {
        struct item *it = &items[i];
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (!strcmp(items[j].id, it->id))
                dup = 1;
        if (it->conflict || dup) {
            if (it->conflict && !dup)
                logline("INBOX: conflicting manifests for %s", it->id);
            continue;
        }
        char *raw = NULL;
        unsigned char digest[20];
        struct pspdx_file spec;
        if (read_one(it->name, &raw, &spec, 1) < 0)
            continue;
        /* Changed on the stick between the two passes: read again next time. */
        sceKernelUtilsSha1Digest((unsigned char *)raw, strlen(raw), digest);
        if (memcmp(digest, it->digest, 20) || strcmp(spec.id, it->id)) {
            free(raw);
            continue;
        }
        int at = catalog_add_repo(catalog, spec.source, 0);
        /* A repository without a .pspdx of its own: the user's file is the
           app, a way to install a release nobody lists. */
        if (at < 0 && catalog_refused(spec.source) == REFUSED_NO_PSPDX) {
            at = catalog_add_file(catalog, raw);
            if (at >= 0)
                logline("INBOX: %s: the repository has no .pspdx; installed from your file", it->id);
        }
        if (at < 0) {
            logline("INBOX: source unavailable for %s", it->id);
            free(raw);
            continue;
        }
        struct app_entry *entry = &catalog->apps[at];
        if (!sources_same_repo(entry->repo, spec.source)) {
            logline("INBOX: identity collision");
            free(raw);
            continue;
        }
        /* A file that pins a release is installed from that release or not at
           all, and the tag is the tag: 0.1.3 is not v0.1.3. */
        if (spec.release_tag[0] && (strcmp(entry->tag, spec.release_tag) ||
                                    (spec.release_url[0] && strcmp(entry->release.url, spec.release_url)))) {
            logline("INBOX: %s pins release %s, which its source does not offer; kept", it->id,
                    spec.release_tag);
            free(raw);
            continue;
        }
        /* The file stands in the release until catalog_prepare has asked the
           repository for one of its own, which wins; an entry made of this
           very file needs no asking. */
        if (entry->no_pspdx != PSPDX_FROM_FILE) {
            if (manifest_keep_raw(&entry->release, raw, strlen(raw)) < 0) {
                logline("INBOX: no memory for %s", it->id);
                free(raw);
                continue;
            }
            entry->from_inbox = 1;
        }
        free(entry->description);
        entry->description = pspdx_description(raw, strlen(raw));
        snprintf(entry->release.dir, sizeof(entry->release.dir), "%.32s", spec.installdir + 9);
        snprintf(entry->release.added_from, sizeof(entry->release.added_from), "INBOX");
        snprintf(entry->name, sizeof(entry->name), "%s", spec.name);
        queue[count].item = i;
        queue[count].index = at;
        queue[count].raw = raw;
        count++;
    }
    if (i < files)
        logline("INBOX: capacity reached; remaining files kept");
    for (int k = 0; k < count - 1; k++)
        if (!strcmp(items[queue[k].item].id, PSPDX_SELF_ID)) {
            struct queued self = queue[k];
            memmove(queue + k, queue + k + 1, (count - k - 1) * sizeof(*queue));
            queue[count - 1] = self;
            break;
        }
    for (int k = 0; k < count; k++) {
        size_t n = strlen(summary);
        const char *name = catalog->apps[queue[k].index].name;
        /* Room is kept for the ellipsis after every name that has another
           after it, so it is never cut inside its three bytes. */
        size_t need = (k ? 2 : 0) + strlen(name) + (k + 1 < count ? sizeof(" …") - 1 : 0);
        if (n + need >= sizeof(summary)) {
            memcpy(summary + n, " …", sizeof(" …"));
            break;
        }
        if (k) {
            memcpy(summary + n, ", ", 2);
            n += 2;
        }
        memcpy(summary + n, name, strlen(name) + 1);
    }
    return count;
}
