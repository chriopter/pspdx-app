#include <pspiofilemgr.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "update/catalog.h"
#include "update/sources.h"
#include "update/pspdx.h"
#include "install/install.h"
#include "util/runtime.h"

#ifndef CATALOG_URL   /* a test build may point at a catalog on the host */
#define CATALOG_URL "https://chriopter.github.io/pspdx-catalog/catalog.json"
#endif

static char response[200 * 1024];
static size_t response_len;

/* The cache last taken: the info band names its host, and the bench fetches
   it. Before any source has answered it is the built-in one. */
static char g_catalog_url[SOURCE_URL] = CATALOG_URL;
static char g_progress[48];
static char g_refused_url[SOURCE_URL];
static int g_refused_rc;

static void copy_str(char *dst, size_t size, cJSON *value) {
    if (cJSON_IsString(value)) {
        strncpy(dst, value->valuestring, size - 1);
        dst[size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

/* Entries point at their assets relative to the catalog, so that moving the
   whole thing to another host stays a one-line change. */
static void asset_url(const char *base, const char *rel, char *out, size_t size) {
    if (!rel || !rel[0]) { out[0] = '\0'; return; }
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        snprintf(out, size, "%s", rel);
        return;
    }
    const char *slash = strrchr(base, '/');
    snprintf(out, size, "%.*s%s", (int)(slash - base) + 1, base, rel);
}

/* Which state the stick puts an entry in: unknown until the update check
   has compared revs, or not installed when there is no record at all. */
static void settle_state(struct app_entry *entry) {
    struct installed installed;
    if (db_read(entry->id, &installed) == 0) {
        entry->state = APP_UNKNOWN;
        entry->local_rev = installed.rev;
        snprintf(entry->local_version, sizeof(entry->local_version), "%s", installed.version);
    } else {
        entry->state = APP_NOT_INSTALLED;
    }
}

/* The first source to name an id wins: an entry already there is left
   alone, whatever a later source says about it. */
static int has_id(const struct catalog *catalog, const char *id) {
    for (int i = 0; i < catalog->count; i++)
        if (strcmp(catalog->apps[i].id, id) == 0) return 1;
    return 0;
}

/* A catalog.json in the response buffer, merged into the catalog. base is
   the URL it came from, for the assets it names relative to itself.
   Returns the entries taken, or -1 for something that is not a catalog. */
static int parse(struct catalog *catalog, const char *base) {
    cJSON *root = cJSON_ParseWithLength(response, response_len);
    if (!root) { logline("catalog: not json"); return -1; }
    cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    if (!cJSON_IsArray(apps)) {
        logline("catalog: no apps array");
        cJSON_Delete(root);
        return -1;
    }

    int taken = 0;
    catalog->total += cJSON_GetArraySize(apps);
    cJSON *app;
    cJSON_ArrayForEach(app, apps) {
        if (catalog->count >= MAX_APPS) break;
        struct app_entry *entry = &catalog->apps[catalog->count];
        memset(entry, 0, sizeof(*entry));
        copy_str(entry->id, sizeof(entry->id), cJSON_GetObjectItemCaseSensitive(app, "id"));
        copy_str(entry->name, sizeof(entry->name), cJSON_GetObjectItemCaseSensitive(app, "name"));
        copy_str(entry->author, sizeof(entry->author), cJSON_GetObjectItemCaseSensitive(app, "author"));
        copy_str(entry->summary, sizeof(entry->summary), cJSON_GetObjectItemCaseSensitive(app, "summary"));
        copy_str(entry->category, sizeof(entry->category), cJSON_GetObjectItemCaseSensitive(app, "category"));
        copy_str(entry->license, sizeof(entry->license), cJSON_GetObjectItemCaseSensitive(app, "license"));
        copy_str(entry->repo, sizeof(entry->repo), cJSON_GetObjectItemCaseSensitive(app, "repo"));

        cJSON *release = cJSON_GetObjectItemCaseSensitive(app, "release");
        if (cJSON_IsObject(release)) {
            struct manifest *m = &entry->release;
            memset(m, 0, sizeof(*m));
            strncpy(m->id, entry->id, sizeof(m->id) - 1);
            cJSON *rev = cJSON_GetObjectItemCaseSensitive(release, "rev");
            cJSON *size = cJSON_GetObjectItemCaseSensitive(release, "size");
            cJSON *sha = cJSON_GetObjectItemCaseSensitive(release, "sha256");
            /* The rules install.h states: these fields go straight into
               a download and an unpack. */
            int ok = 1;
            if (cJSON_IsNumber(rev) && manifest_rev_in_range(rev->valuedouble))
                m->rev = (unsigned)rev->valuedouble;
            else ok = 0;
            if (cJSON_IsNumber(size) && manifest_size_in_range(size->valuedouble))
                m->size = (size_t)size->valuedouble;
            else ok = 0;
            copy_str(m->url, sizeof(m->url), cJSON_GetObjectItemCaseSensitive(release, "url"));
            copy_str(m->version, sizeof(m->version), cJSON_GetObjectItemCaseSensitive(release, "version"));
            ok = ok && m->rev && m->url[0];
            /* A cache that downloaded the zip says what it hashed to, and
               the install holds the download to it. One that left the
               hash out leaves the size as the check, as the origin path
               does; one that wrote something that is not a hash is not
               trusted with the rest. */
            if (cJSON_IsString(sha)) {
                ok = ok && strlen(sha->valuestring) == 64;
                for (int k = 0; ok && k < 32; k++) {
                    unsigned byte;
                    if (sscanf(sha->valuestring + 2 * k, "%2x", &byte) != 1) ok = 0;
                    m->sha256[k] = (unsigned char)byte;
                }
            }
            entry->has_release = ok;
            /* The release is installed from as it stands, and the record
               it writes has to say which repository it came from, or the
               package could never be found at its source once the cache
               is gone. */
            memcpy(m->repo, entry->repo, sizeof(entry->repo));
            /* The shape of the zip, as the app's .pspdx stated it and the
               cache copied it over. Checked where it is used, in
               install.c, so that a cache and the origin path are held to
               the same rule by the same code. */
            cJSON *install = cJSON_GetObjectItemCaseSensitive(app, "install");
            if (cJSON_IsObject(install)) {
                copy_str(m->root, sizeof(m->root),
                         cJSON_GetObjectItemCaseSensitive(install, "root"));
                copy_str(m->dir, sizeof(m->dir),
                         cJSON_GetObjectItemCaseSensitive(install, "dir"));
            }
        }

        char shot[256];
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "icon"));
        asset_url(base, shot, entry->icon, sizeof(entry->icon));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "screenshot"));
        asset_url(base, shot, entry->screenshot, sizeof(entry->screenshot));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "video"));
        asset_url(base, shot, entry->video, sizeof(entry->video));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "sound"));
        asset_url(base, shot, entry->sound, sizeof(entry->sound));

        /* An id names a directory on the stick and a file in the cache: one
           that cannot be a path component is not an entry. */
        if (!manifest_id_is_safe(entry->id) || !entry->name[0]) continue;
        if (!entry->has_release) continue;
        if (has_id(catalog, entry->id)) continue;

        settle_state(entry);
        catalog->count++;
        taken++;
    }
    cJSON_Delete(root);
    return taken;
}

static int response_sink(void *ctx, const void *data, size_t len) {
    (void)ctx;
    if (response_len + len >= sizeof(response)) return -1;
    memcpy(response + response_len, data, len);
    response_len += len;
    return 0;
}

/* One text file into the response buffer. Returns 0 when it arrived. */
static int fetch_text(const char *url, struct https_result *out) {
    struct https_result r;
    response_len = 0;
    int rc = https_get(url, response_sink, NULL, NULL, NULL, &r);
    if (out) *out = r;
    if (rc != 0 || r.status != 200) {
        logline("fetch: rc=%d status=%ld %s", rc, r.status, url);
        return -1;
    }
    response[response_len] = '\0';
    return 0;
}

/* A cache fetched and merged in. Returns the entries taken, or -1 when
   the cache did not answer or was not a catalog. */
static int take_cache(struct catalog *catalog, const char *url) {
    struct https_result r;
    if (fetch_text(url, &r) < 0) return -1;
    int taken = parse(catalog, url);
    if (taken < 0) return -1;
    catalog->fetch = r;
    catalog->response_len = response_len;
    snprintf(g_catalog_url, sizeof(g_catalog_url), "%s", url);
    return taken;
}

/* ------------------------------------------------------------- origin */

/* "2026-09-12T08:29:23Z", as GitHub writes published_at, to unix seconds.
   Days from the civil date by Howard Hinnant's arithmetic, which needs no
   table of month lengths. Returns 0 for anything that is not that. */
static unsigned iso8601(const char *text) {
    int y, mo, d, h, mi, sec;
    if (!text || sscanf(text, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || y < 1970) return 0;
    y -= mo <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (unsigned)(days * 86400 + h * 3600 + mi * 60 + sec);
}

/* The one wildcard a list's asset= needs: "*" for any run of characters. */
static int glob_match(const char *pattern, const char *name) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;
            for (const char *p = name; *p; p++)
                if (glob_match(pattern, p)) return 1;
            return 0;
        }
        if (*pattern != *name) return 0;
        pattern++;
        name++;
    }
    return !*name;
}

static int ends_with_zip(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".zip") == 0;
}

/* A description into the summary: the first 59 bytes, backed off so that
   a UTF-8 sequence is not cut in the middle. */
static void cut_summary(char *dst, size_t size, const char *text) {
    size_t n = strlen(text);
    if (n >= size) {
        n = size - 1;
        while (n && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, text, n);
    dst[n] = '\0';
}

/* One text file from GitHub's API into the response buffer, parsed.
   Returns the JSON, or NULL when it did not arrive or was not JSON. */
static cJSON *fetch_json(const char *url) {
    if (fetch_text(url, NULL) < 0) return NULL;
    cJSON *root = cJSON_ParseWithLength(response, response_len);
    if (!root) logline("origin: not json: %s", url);
    return root;
}

static void refuse(const char *url, int why) {
    snprintf(g_refused_url, sizeof(g_refused_url), "%s", url);
    g_refused_rc = why;
}

/* One of the media directory's four files, at raw.githubusercontent.com.
   Sony's own spelling and nothing else is tried: the cache can list the
   directory and match without regard to case, while a console can only ask
   for a name, and sixteen guesses a picture is sixteen handshakes. An
   author who wants the console to see the files calls them ICON0.PNG,
   PIC1.PNG, ICON1.PMF and SND0.AT3. */
static void media_url(const struct source_repo *repo, const char *media,
                      const char *file, char *out, size_t size) {
    char url[512];
    int n = snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/%s/%s%s%s",
                     repo->owner, repo->name, repo->ref, media,
                     media[0] ? "/" : "", file);
    /* A URL too long for the field is no URL at all: a cut one would ask
       for something else and be answered. */
    if (n < 0 || (size_t)n >= size) { out[0] = '\0'; return; }
    memcpy(out, url, (size_t)n + 1);
}

/* One repository asked at the origin and made into an entry: its .pspdx
   at raw.githubusercontent.com first, since that file is the consent and a
   repository without one is not an app; then api.github.com for the
   release, and for the repository too unless the file already said
   everything that would come from it. The pictures are four more URLs
   under the media directory, not fetched here -- the card and the row ask
   for them when the cursor arrives, exactly as they do from a cache.
   Returns 1 taken, 0 already there, -1 refused, with why kept for the
   gear tab. */
static int take_origin(struct catalog *catalog, const struct source_repo *repo) {
    char id[96], url[SOURCE_URL], api[SOURCE_URL];
    sources_repo_id(repo, id, sizeof(id));
    sources_repo_url(repo, url, sizeof(url));
    if (catalog->count >= MAX_APPS) return -1;
    if (has_id(catalog, id)) return 0;

    struct pspdx_file file;
    char reason[64];
    snprintf(api, sizeof(api), "https://raw.githubusercontent.com/%s/%s/%s/.pspdx",
             repo->owner, repo->name, repo->ref);
    if (fetch_text(api, NULL) < 0) {
        logline("origin: %s/%s has no .pspdx at %s, not listed",
                repo->owner, repo->name, repo->ref);
        refuse(url, REFUSED_PSPDX);
        return -1;
    }
    if (pspdx_parse(response, response_len, &file, reason, sizeof(reason)) < 0) {
        logline("origin: %s/%s .pspdx refused: %s", repo->owner, repo->name, reason);
        refuse(url, REFUSED_PSPDX);
        return -1;
    }
    logline("origin: %s/%s .pspdx: %s, %s", repo->owner, repo->name,
            file.name, file.category);

    struct app_entry *entry = &catalog->apps[catalog->count];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->id, sizeof(entry->id), "%s", id);
    snprintf(entry->repo, sizeof(entry->repo), "%s", url);
    snprintf(entry->name, sizeof(entry->name), "%s", file.name);
    snprintf(entry->category, sizeof(entry->category), "%s", file.category);
    snprintf(entry->author, sizeof(entry->author), "%s",
             file.author[0] ? file.author : repo->owner);
    snprintf(entry->summary, sizeof(entry->summary), "%s", file.summary);
    snprintf(entry->license, sizeof(entry->license), "%s", file.license);

    /* The repository itself is one more request, and sixty an hour are
       allowed from one address: it is asked only for what the file left
       out. A file that names its author, its summary and its licence is
       the whole answer, and the request is not made at all. */
    if (!file.author[0] || !file.summary[0] || !file.license[0]) {
        snprintf(api, sizeof(api), "https://api.github.com/repos/%s/%s",
                 repo->owner, repo->name);
        cJSON *root = fetch_json(api);
        if (!root) {
            /* The file said the repository is an app, and it is. GitHub
               being unreachable or out of requests costs the summary and
               the licence, not the entry. */
            logline("origin: %s/%s: no repository answer, the file stands alone",
                    repo->owner, repo->name);
        } else {
            cJSON *description = cJSON_GetObjectItemCaseSensitive(root, "description");
            if (!entry->summary[0] && cJSON_IsString(description))
                cut_summary(entry->summary, sizeof(entry->summary), description->valuestring);
            cJSON *license = cJSON_GetObjectItemCaseSensitive(root, "license");
            if (!entry->license[0] && cJSON_IsObject(license))
                copy_str(entry->license, sizeof(entry->license),
                         cJSON_GetObjectItemCaseSensitive(license, "spdx_id"));
            /* What GitHub says when it saw a licence file it could not name. */
            if (strcmp(entry->license, "NOASSERTION") == 0) entry->license[0] = '\0';
            cJSON_Delete(root);
            logline("origin: %s/%s: repository asked for%s%s%s", repo->owner, repo->name,
                    file.author[0] ? "" : " author", file.summary[0] ? "" : " summary",
                    file.license[0] ? "" : " licence");
        }
    } else {
        logline("origin: %s/%s: the file says all three, no repository request",
                repo->owner, repo->name);
    }

    /* Which release: the list's @tag pins hardest, then the file's own
       release, and with neither it is whatever GitHub calls latest. */
    const char *pinned = strcmp(repo->ref, "HEAD") != 0 ? repo->ref
                       : file.release[0] ? file.release : NULL;
    if (pinned)
        snprintf(api, sizeof(api), "https://api.github.com/repos/%s/%s/releases/tags/%s",
                 repo->owner, repo->name, pinned);
    else
        snprintf(api, sizeof(api), "https://api.github.com/repos/%s/%s/releases/latest",
                 repo->owner, repo->name);
    cJSON *root = fetch_json(api);
    if (!root) {
        logline("origin: %s/%s has no release %s", repo->owner, repo->name,
                pinned ? pinned : "(latest)");
        refuse(url, REFUSED_RELEASE);
        return -1;
    }
    struct manifest *m = &entry->release;
    memset(m, 0, sizeof(*m));
    snprintf(m->id, sizeof(m->id), "%s", id);
    snprintf(m->repo, sizeof(m->repo), "%s", url);
    char tag[40];
    copy_str(tag, sizeof(tag), cJSON_GetObjectItemCaseSensitive(root, "tag_name"));
    snprintf(m->version, sizeof(m->version), "%.31s", tag[0] == 'v' ? tag + 1 : tag);
    cJSON *published = cJSON_GetObjectItemCaseSensitive(root, "published_at");
    m->rev = iso8601(cJSON_IsString(published) ? published->valuestring : NULL);
    /* The zip: the only one on the release, or the one the file's asset
       glob names when there are several. */
    const char *glob = file.asset[0] ? file.asset : NULL;
    int zips = 0;
    cJSON *asset;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(root, "assets")) {
        cJSON *aname = cJSON_GetObjectItemCaseSensitive(asset, "name");
        cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
        if (!cJSON_IsString(aname) || !cJSON_IsNumber(size)) continue;
        if (glob ? !glob_match(glob, aname->valuestring) : !ends_with_zip(aname->valuestring)) continue;
        zips++;
        if (manifest_size_in_range(size->valuedouble)) m->size = (size_t)size->valuedouble;
        copy_str(m->url, sizeof(m->url),
                 cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url"));
    }
    cJSON_Delete(root);
    if (zips != 1 || !m->rev || !m->url[0] || !m->size) {
        logline("origin: %s/%s %s: %d zip%s%s, rev %u", repo->owner, repo->name,
                tag[0] ? tag : "no tag", zips, zips == 1 ? "" : "s",
                glob ? " matching the file's asset" : "", m->rev);
        refuse(url, REFUSED_RELEASE);
        return -1;
    }
    entry->has_release = 1;
    /* The shape of the zip, as the author stated it; install.c holds both
       to their rules, the same ones a cache entry's are held to. */
    snprintf(m->root, sizeof(m->root), "%s", file.root);
    snprintf(m->dir, sizeof(m->dir), "%s", file.dir);

    /* The four pictures under the media directory. Nothing is fetched now:
       a 404 is simply no picture and is logged where it happens, once, by
       whoever went looking -- the row for the icon, the card for the rest. */
    media_url(repo, file.media, "ICON0.PNG", entry->icon, sizeof(entry->icon));
    media_url(repo, file.media, "PIC1.PNG", entry->screenshot, sizeof(entry->screenshot));
    media_url(repo, file.media, "ICON1.PMF", entry->video, sizeof(entry->video));
    media_url(repo, file.media, "SND0.AT3", entry->sound, sizeof(entry->sound));

    logline("origin: %s/%s %s rev %u, %lu bytes, media %s/", repo->owner, repo->name,
            tag, m->rev, (unsigned long)m->size,
            file.media[0] ? file.media : "(root)");
    settle_state(entry);
    catalog->count++;
    catalog->total++;
    return 1;
}

/* Every repository of a list that no entry so far came from -- the ones
   the list's own cache left out, or all of them without a cache -- asked
   at the origin, the status line counting along. Two requests each, and
   sixty an hour are allowed from one address, which is why the covered
   ones are not asked again. Returns the entries taken; asked and refused
   say how many were tried and how many GitHub had nothing usable for. */
static int walk_list(struct catalog *catalog, const struct source_list *list,
                     int *asked, int *refused) {
    int taken = 0;
    *asked = *refused = 0;
    for (int i = 0; i < list->count; i++) {
        char url[SOURCE_URL];
        sources_repo_url(&list->repo[i], url, sizeof(url));
        if (catalog_find_repo(catalog, url) >= 0) continue;
        snprintf(g_progress, sizeof(g_progress), "origin: %d of %d", i + 1, list->count);
        (*asked)++;
        int rc = take_origin(catalog, &list->repo[i]);
        if (rc > 0) taken++;
        else if (rc < 0) (*refused)++;
    }
    g_progress[0] = '\0';
    return taken;
}

/* One source, whatever kind it is. Returns the entries taken, or -1 when
   nothing behind it answered. */
static int fetch_source(struct catalog *catalog, int at, const char *url) {
    struct source_list list;
    int asked = 0, refused = 0, taken, cached = -1;

    switch (sources_kind(url)) {
    case SOURCE_CATALOG:
        taken = take_cache(catalog, url);
        if (taken < 0) logline("source %d: catalog unreachable", at);
        else logline("source %d: catalog, %d apps", at, taken);
        return taken;

    case SOURCE_REPO:
        memset(&list, 0, sizeof(list));
        sources_parse_repo(url, &list.repo[0]);
        list.count = 1;
        taken = walk_list(catalog, &list, &asked, &refused);
        logline("source %d: repository %s/%s, %d app%s", at,
                list.repo[0].owner, list.repo[0].name, taken,
                refused ? ", refused" : "");
        return refused && !taken ? -1 : taken;

    default:
        break;
    }

    if (fetch_text(url, NULL) < 0) {
        /* The built-in list names the built-in cache, and a console that
           cannot read the one still knows where the other is. */
        if (strcmp(url, SOURCES_DEFAULT) != 0) {
            logline("source %d: list unreachable", at);
            return -1;
        }
        taken = take_cache(catalog, CATALOG_URL);
        logline("source %d: list unreachable, built-in cache %s", at,
                taken < 0 ? "unreachable too" : "taken");
        return taken;
    }
    sources_parse_list(response, &list);
    /* The built-in list is the built-in cache's own list, so that cache is
       taken whether or not the file names it: a `cache` line dropped over
       there would otherwise cost every console on it a walk of the whole
       list at the origin, where there are no pictures and no hashes. A list
       somebody else keeps is trusted only for what it says. */
    const char *cache = list.cache[0] ? list.cache
                      : strcmp(url, SOURCES_DEFAULT) == 0 ? CATALOG_URL : 0;
    if (cache) cached = take_cache(catalog, cache);
    taken = walk_list(catalog, &list, &asked, &refused);
    logline("source %d: list of %d, cache %s%d apps, origin %d asked, %d apps, %d refused",
            at, list.count,
            !cache ? "none, " : cached < 0 ? "unreachable, " : "",
            cached > 0 ? cached : 0, asked, taken, refused);
    return (cached > 0 ? cached : 0) + taken;
}

const char *catalog_url(void) { return g_catalog_url; }
const char *catalog_progress(void) { return g_progress; }

int catalog_refused(const char *url) {
    return sources_same_url(g_refused_url, url) ? g_refused_rc : 0;
}

int catalog_find_repo(const struct catalog *catalog, const char *url) {
    for (int i = 0; i < catalog->count; i++)
        if (sources_same_url(catalog->apps[i].repo, url)) return i;
    return -1;
}

int catalog_add_repo(struct catalog *catalog, const char *url) {
    struct source_repo repo;
    if (!sources_parse_repo(url, &repo)) return -1;
    char canonical[SOURCE_URL];
    sources_repo_url(&repo, canonical, sizeof(canonical));
    int at = catalog_find_repo(catalog, canonical);
    if (at >= 0) return at;
    if (take_origin(catalog, &repo) <= 0) return -1;
    return catalog->count - 1;
}

int catalog_fetch(struct catalog *catalog) {
    struct sources sources;
    memset(catalog, 0, sizeof(*catalog));
    g_progress[0] = '\0';
    g_refused_url[0] = '\0';
    sources_load(&sources);
    int answered = 0;
    for (int i = 0; i < sources.count; i++)
        if (fetch_source(catalog, i + 1, sources.url[i]) >= 0) answered++;
    logline("catalog: %d of %d sources, %d apps, %d usable", answered,
            sources.count, catalog->total, catalog->count);
    return answered ? catalog->count : -1;
}

int catalog_check_updates(struct catalog *catalog) {
    int updates = 0;
    for (int i = 0; i < catalog->count; i++) {
        struct app_entry *entry = &catalog->apps[i];
        if (entry->state == APP_NOT_INSTALLED) continue;
        struct manifest manifest = entry->release;
        entry->remote_rev = manifest.rev;
        snprintf(entry->remote_version, sizeof(entry->remote_version), "%s", manifest.version);
        /* Every other record carries the rev of the release it came from,
           because an install had the catalog in front of it. PSPDX's own was
           written by its first start out of nothing but the build, and a rev
           is the moment GitHub published the release -- which a build cannot
           know. So that record holds rev 0 and the version string the build
           was made from, and this once the comparison is made on the version
           instead. The same version means the stick is running the published
           release: the catalog's rev goes into the record, and from the next
           run on it is an ordinary record compared like any other. A
           different version is an update, whichever way the strings sort. */
        if (strcmp(entry->id, PSPDX_SELF_ID) == 0 && entry->local_rev == 0) {
            /* A build made past the tag -- "0.1.0-5-gabc", as git describes
               it -- is the release and then some, not an older one: it
               counts as current, or every desk build would offer itself
               the release it was built after. */
            size_t n = strlen(manifest.version);
            int same = strncmp(entry->local_version, manifest.version, n) == 0 &&
                       (entry->local_version[n] == '\0' || entry->local_version[n] == '-');
            if (same) {
                struct installed self;
                if (db_read(entry->id, &self) == 0) {
                    self.rev = manifest.rev;
                    if (db_write_record(&self) == 0) entry->local_rev = manifest.rev;
                }
                entry->state = APP_CURRENT;
                logline("self: %s is the published release, rev %u noted",
                        entry->local_version, manifest.rev);
            } else {
                entry->state = APP_UPDATE;
                updates++;
                logline("self: %s installed, %s published",
                        entry->local_version, manifest.version);
            }
            continue;
        }
        if (manifest.rev > entry->local_rev) {
            entry->state = APP_UPDATE;
            updates++;
        } else {
            entry->state = APP_CURRENT;
        }
    }
    logline("updates: %d waiting, %d apps", updates, catalog->count);
    return updates;
}

void catalog_dump_http(void) {
    if (!response_len) return;
    int fd = sceIoOpen("ms0:/PSPDX.HTTP", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, response, response_len);
    sceIoClose(fd);
}
