#include "install/state.h"
#include "util/storage.h"
#include <cjson/cJSON.h>
#include <ctype.h>
#include <limits.h>
#include <pspiofilemgr.h>
#include <psputils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "install/install.h"
#include "update/catalog.h"
#include "update/pspdx.h"
#include "update/sources.h"
#include "util/runtime.h"

#ifndef CATALOG_URL /* a test build may point at a catalog on the host */
#define CATALOG_URL SOURCES_DEFAULT "catalog.json"
#endif

static char response[200 * 1024];
static size_t response_len;
static int g_offline, g_force, parsing_cached;
/* What the last answer was, for the line that says why nothing came:
   one too big for the buffer above, and one that named no apps at all. */
static int g_too_large, g_parsed_empty;
static char attempted[MAX_APPS + LIST_REPOS][SOURCE_URL];
static int attempted_count;
void catalog_offline(int value) { g_offline = value; }
void catalog_force_sources(void) { g_force = 1; }

/* The cache last taken: the info band names its host, and the bench fetches
   it. Before any source has answered it is the built-in one. */
static char g_catalog_url[SOURCE_URL] = CATALOG_URL;
static char g_progress[48];
static char g_refused_url[SOURCE_URL];
static int g_refused_rc;

static void copy_str(char *dst, size_t size, cJSON *value) {
    if (cJSON_IsString(value) && strlen(value->valuestring) < size) {
        strncpy(dst, value->valuestring, size - 1);
        dst[size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

/* Entries point at their assets relative to the catalog, so that moving the
   whole thing to another host stays a one-line change. */
static void asset_url(const char *base, const char *rel, char *out, size_t size) {
    if (!rel || !rel[0]) {
        out[0] = '\0';
        return;
    }
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        snprintf(out, size, "%s", rel);
        return;
    }
    const char *slash = strrchr(base, '/');
    if (!slash) {
        out[0] = 0;
        return;
    }
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
        if (strcmp(catalog->apps[i].id, id) == 0)
            return 1;
    return 0;
}

static unsigned iso8601(const char *text);

/* A catalog.json in the response buffer, merged into the catalog. base is
   the URL it came from, for the assets it names relative to itself.
   Returns the entries taken, or -1 for something that is not a catalog. */
static int parse(struct catalog *catalog, const char *base) {
    cJSON *root = cJSON_ParseWithLengthOpts(response, response_len + 1, NULL, 1);
    if (!root) {
        logline("catalog: not json");
        return -1;
    }
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsString(schema) || strcmp(schema->valuestring,
            "https://github.com/chriopter/pspdx/blob/master/schema/catalog-v1.json")) {
        logline("catalog: unsupported schema");
        cJSON_Delete(root);
        return -1;
    }
    cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    if (!cJSON_IsArray(apps)) {
        logline("catalog: no apps array");
        cJSON_Delete(root);
        return -1;
    }
    /* When the list was last written, as the list says; the oldest of the
       sources is what the session is told about. */
    cJSON *generated = cJSON_GetObjectItemCaseSensitive(root, "generated_at");
    unsigned when = iso8601(cJSON_IsString(generated) ? generated->valuestring : NULL);
    if (!when) {
        logline("catalog: invalid generated_at");
        cJSON_Delete(root);
        return -1;
    }
    if (!catalog->generated || when < catalog->generated) {
        catalog->generated = when;
        /* The host alone: a line on the console has no room for a URL,
           and the host is what a person calls the list. */
        const char *host = strstr(base, "://");
        host = host ? host + 3 : base;
        size_t n = strcspn(host, "/");
        snprintf(catalog->generated_from, sizeof(catalog->generated_from), "%.*s", (int)n, host);
    }

    int before = catalog->count;
    int taken = 0;
    catalog->total += cJSON_GetArraySize(apps);
    cJSON *app;
    cJSON_ArrayForEach(app, apps) {
        if (catalog->count >= MAX_APPS)
            break;
        struct app_entry *entry = &catalog->apps[catalog->count];
        memset(entry, 0, sizeof(*entry));
        copy_str(entry->id, sizeof(entry->id), cJSON_GetObjectItemCaseSensitive(app, "id"));
        copy_str(entry->name, sizeof(entry->name), cJSON_GetObjectItemCaseSensitive(app, "name"));
        copy_str(entry->author, sizeof(entry->author),
                 cJSON_GetObjectItemCaseSensitive(app, "author"));
        copy_str(entry->summary, sizeof(entry->summary),
                 cJSON_GetObjectItemCaseSensitive(app, "summary"));
        copy_str(entry->category, sizeof(entry->category),
                 cJSON_GetObjectItemCaseSensitive(app, "category"));
        copy_str(entry->license, sizeof(entry->license),
                 cJSON_GetObjectItemCaseSensitive(app, "license"));
        copy_str(entry->repo, sizeof(entry->repo), cJSON_GetObjectItemCaseSensitive(app, "source"));

        cJSON *release = cJSON_GetObjectItemCaseSensitive(app, "release");
        if (cJSON_IsObject(release)) {
            struct manifest *m = &entry->release;
            memset(m, 0, sizeof(*m));
            strncpy(m->id, entry->id, sizeof(m->id) - 1);
            cJSON *published = cJSON_GetObjectItemCaseSensitive(release, "published_at");
            cJSON *tag = cJSON_GetObjectItemCaseSensitive(release, "tag");
            cJSON *download = cJSON_GetObjectItemCaseSensitive(release, "download");
            cJSON *size = cJSON_GetObjectItemCaseSensitive(download, "size");
            cJSON *sha = cJSON_GetObjectItemCaseSensitive(download, "sha256");
            /* The rules install.h states: these fields go straight into
               a download and an unpack. */
            int ok = 1;
            m->rev = iso8601(cJSON_IsString(published) ? published->valuestring : NULL);
            if (!m->rev) ok = 0;
            if (cJSON_IsNumber(size) && manifest_size_in_range(size->valuedouble))
                m->size = (size_t)size->valuedouble;
            else
                ok = 0;
            copy_str(m->url, sizeof(m->url), cJSON_GetObjectItemCaseSensitive(download, "url"));
            if (cJSON_IsString(tag) && tag->valuestring[0]) {
                const char *version = tag->valuestring + (tag->valuestring[0] == 'v');
                if (strlen(version) < sizeof(m->version))
                    snprintf(m->version, sizeof(m->version), "%s", version);
                else ok = 0;
            } else ok = 0;
            ok = ok && m->rev && !strncmp(m->url, "https://", 8) && m->version[0];
            /* A cache that downloaded the zip says what it hashed to, and
               the install holds the download to it. One that left the
               hash out leaves the size as the check, as the origin path
               does; one that wrote something that is not a hash is not
               trusted with the rest. */
            if (cJSON_IsString(sha)) {
                ok = ok && strlen(sha->valuestring) == 64;
                for (int k = 0; ok && k < 32; k++) {
                    unsigned byte = 0;
                    if (!isxdigit((unsigned char)sha->valuestring[2 * k]) ||
                        !isxdigit((unsigned char)sha->valuestring[2 * k + 1])) {
                        ok = 0;
                        break;
                    }
                    if (sscanf(sha->valuestring + 2 * k, "%2x", &byte) != 1)
                        ok = 0;
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
            cJSON *target = cJSON_GetObjectItemCaseSensitive(app, "installdir");
            if (!cJSON_IsString(target) || !pspdx_install_dir(target->valuestring))
                entry->has_release = 0;
            else
                snprintf(m->dir, sizeof(m->dir), "%s", target->valuestring + 9);
            snprintf(m->added_from, sizeof(m->added_from), "%s", base);
            snprintf(m->checked_from, sizeof(m->checked_from), "%s", base);
            m->checked_at = parsing_cached ? 0 : (unsigned)time(NULL);
            entry->fresh = !parsing_cached;
            entry->media_cached_only = parsing_cached;
#ifdef PSPDX_TEST_FIXTURES
            cJSON *fixture = cJSON_GetObjectItemCaseSensitive(app, "_test_manifest");
            if (cJSON_IsObject(fixture)) {
                char *raw = cJSON_PrintUnformatted(fixture);
                if (raw && strlen(raw) <= PSPDX_FILE_MAX)
                    strcpy(m->raw, raw);
                free(raw);
            }
#endif
        }

        char shot[256];
        cJSON *media = cJSON_GetObjectItemCaseSensitive(app, "media");
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(media, "icon"));
        asset_url(base, shot, entry->icon, sizeof(entry->icon));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(media, "screenshot"));
        asset_url(base, shot, entry->screenshot, sizeof(entry->screenshot));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(media, "video"));
        asset_url(base, shot, entry->video, sizeof(entry->video));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(media, "sound"));
        asset_url(base, shot, entry->sound, sizeof(entry->sound));

        /* An id names a directory on the stick and a file in the cache: one
           that cannot be a path component is not an entry. */
        if (!manifest_id_is_safe(entry->id) || !entry->name[0])
            continue;
        if (!entry->has_release)
            continue;
        struct source_repo source;
        char expected[96];
        if (!sources_parse_repo(entry->repo, &source))
            continue;
        sources_repo_id(&source, expected, sizeof(expected));
        if (strcmp(expected, entry->id) || !sources_release_url(entry->repo, entry->release.url))
            continue;
        if (has_id(catalog, entry->id))
            continue;
        struct installed local;
        if (db_read(entry->id, &local) < 0 && catalog->count >= MAX_APPS - state_count())
            continue;
        /* One directory is one app's: a second entry that wants the same
           name would only be refused at install time, so the first keeps it. */
        int held;
        for (held = 0; held < catalog->count; held++)
            if (!strcasecmp(catalog->apps[held].release.dir, entry->release.dir))
                break;
        if (held < catalog->count) {
            logline("catalog: %s wants PSP/GAME/%s, which %s has; dropped", entry->id,
                    entry->release.dir, catalog->apps[held].id);
            continue;
        }

        settle_state(entry);
        catalog->count++;
        taken++;
    }
    int empty = cJSON_GetArraySize(apps) == 0;
    g_parsed_empty = empty;
    cJSON_Delete(root);
    return taken || empty || before > 0 ? taken : -1;
}

static int response_sink(void *ctx, const void *data, size_t len) {
    (void)ctx;
    if (response_len + len >= sizeof(response)) {
        g_too_large = 1;
        return -1;
    }
    memcpy(response + response_len, data, len);
    response_len += len;
    return 0;
}

/* One text file into the response buffer. Returns 0 when it arrived. */
static int fetch_text(const char *url, struct https_result *out) {
    struct https_result r;
    if (g_offline)
        return -1;
    response_len = 0;
    g_too_large = 0;
    int rc = https_get(url, response_sink, NULL, NULL, NULL, &r);
    if (out)
        *out = r;
    if (rc != 0 || r.status != 200) {
        if (g_too_large)
            logline("fetch: %s is larger than the %u KB there is room for", url,
                    (unsigned)(sizeof(response) / 1024));
        else
            logline("fetch: rc=%d status=%ld %s", rc, r.status, url);
        return -1;
    }
    response[response_len] = '\0';
    return 0;
}

/* A cache fetched and merged in. Returns the entries taken, or -1 when
   the cache did not answer or was not a catalog. */
enum cache_mode { CACHE_LIVE, CACHE_LIVE_OR_SAVED, CACHE_SAVED_ONLY };
static int take_cache(struct catalog *catalog, const char *url, enum cache_mode mode) {
    unsigned char digest[20];
    sceKernelUtilsSha1Digest((unsigned char *)url, strlen(url), digest);
    char hex[41], path[256];
    for (int i = 0; i < 20; i++)
        sprintf(hex + 2 * i, "%02x", digest[i]);
    snprintf(path, sizeof(path), "%s/%s.json", storage_path("PSP/PSPDX/CACHE/catalogs"), hex);
    struct https_result r;
    int taken = -1;
    parsing_cached = 0;
    if (mode != CACHE_SAVED_ONLY && fetch_text(url, &r) == 0) {
        taken = parse(catalog, url);
        if (taken >= 0) {
            /* A list that names nothing today is not what the stick should
               remember over what it had. */
            if (g_parsed_empty && storage_exists(path))
                logline("catalog: %s names no apps, the saved one is kept", url);
            else if (storage_write(path, response, response_len) == 0)
                storage_trim_cache(storage_path("PSP/PSPDX/CACHE/catalogs"), 4u * 1024u * 1024u,
                                   path);
            catalog->fetch = r;
            catalog->response_len = response_len;
        }
    }
    if (taken < 0 && mode != CACHE_LIVE) {
        char *raw = NULL;
        int n = storage_read(path, &raw, sizeof(response) - 1);
        if (n >= 0) {
            memcpy(response, raw, n + 1);
            response_len = n;
            parsing_cached = 1;
            taken = parse(catalog, url);
            free(raw);
        }
    }
    parsing_cached = 0;
    if (taken >= 0)
        snprintf(g_catalog_url, sizeof(g_catalog_url), "%s", url);
    return taken;
}

/* ------------------------------------------------------------- origin */

/* "2026-09-12T08:29:23Z", as GitHub writes published_at, to unix seconds.
   Days from the civil date by Howard Hinnant's arithmetic, which needs no
   table of month lengths. Returns 0 for anything that is not that. */
static unsigned iso8601(const char *text) {
    int y, mo, d, h, mi, sec;
    if (!text || strlen(text) != 20 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z')
        return 0;
    for (int i = 0; i < 19; i++)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 &&
            !isdigit((unsigned char)text[i]))
            return 0;
    if (sscanf(text, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6)
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || y < 1970 ||
        h > 23 || mi > 59 || sec > 59)
        return 0;
    y -= mo <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + doe - 719468;
    long long seconds = days * 86400 + h * 3600 + mi * 60 + sec;
    return seconds > 0 && seconds <= UINT_MAX ? (unsigned)seconds : 0;
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
        while (n && ((unsigned char)text[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dst, text, n);
    dst[n] = '\0';
}

/* One text file from GitHub's API into the response buffer, parsed.
   Returns the JSON, or NULL when it did not arrive or was not JSON. */
static cJSON *fetch_json(const char *url) {
    if (fetch_text(url, NULL) < 0)
        return NULL;
    cJSON *root = cJSON_ParseWithLength(response, response_len);
    if (!root)
        logline("origin: not json: %s", url);
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
    if (catalog->count >= MAX_APPS)
        return -1;
    if (has_id(catalog, id))
        return 0;
    struct installed local;
    if (db_read(id, &local) < 0 && catalog->count >= MAX_APPS - state_count())
        return -1;
    for (int i = 0; i < attempted_count; i++)
        if (sources_same_url(attempted[i], url))
            return -1;
    if (attempted_count >= MAX_APPS + LIST_REPOS)
        return -1;
    snprintf(attempted[attempted_count++], SOURCE_URL, "%s", url);

    struct pspdx_file file;
    char reason[64];
    snprintf(api, sizeof(api), "https://raw.githubusercontent.com/%s/%s/%s/.pspdx", repo->owner,
             repo->name, repo->ref);
    if (fetch_text(api, NULL) < 0) {
        logline("origin: %s/%s has no .pspdx at %s, not listed", repo->owner, repo->name,
                repo->ref);
        refuse(url, REFUSED_PSPDX);
        return -1;
    }
    if (pspdx_parse(response, response_len, &file, reason, sizeof(reason)) < 0) {
        logline("origin: %s/%s .pspdx refused: %s", repo->owner, repo->name, reason);
        refuse(url, REFUSED_PSPDX);
        return -1;
    }
    if (!sources_same_url(file.source, url)) {
        refuse(url, REFUSED_PSPDX);
        return -1;
    }
    char original[PSPDX_FILE_MAX + 1];
    memcpy(original, response, response_len + 1);
    logline("origin: %s/%s .pspdx: %s, %s", repo->owner, repo->name, file.name, file.category);

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
        snprintf(api, sizeof(api), "https://api.github.com/repos/%s/%s", repo->owner, repo->name);
        cJSON *root = fetch_json(api);
        if (!root) {
            /* The file said the repository is an app, and it is. GitHub
               being unreachable or out of requests costs the summary and
               the licence, not the entry. */
            logline("origin: %s/%s: no repository answer, the file stands alone", repo->owner,
                    repo->name);
        } else {
            cJSON *description = cJSON_GetObjectItemCaseSensitive(root, "description");
            if (!entry->summary[0] && cJSON_IsString(description))
                cut_summary(entry->summary, sizeof(entry->summary), description->valuestring);
            cJSON *license = cJSON_GetObjectItemCaseSensitive(root, "license");
            if (!entry->license[0] && cJSON_IsObject(license))
                copy_str(entry->license, sizeof(entry->license),
                         cJSON_GetObjectItemCaseSensitive(license, "spdx_id"));
            /* What GitHub says when it saw a licence file it could not name. */
            if (strcmp(entry->license, "NOASSERTION") == 0)
                entry->license[0] = '\0';
            cJSON_Delete(root);
            logline("origin: %s/%s: repository asked for%s%s%s", repo->owner, repo->name,
                    file.author[0] ? "" : " author", file.summary[0] ? "" : " summary",
                    file.license[0] ? "" : " licence");
        }
    } else {
        logline("origin: %s/%s: the file says all three, no repository request", repo->owner,
                repo->name);
    }

    /* Which release: the list's @tag pins hardest, then the file's own
       release, and with neither it is whatever GitHub calls latest. */
    const char *pinned = strcmp(repo->ref, "HEAD") != 0 ? repo->ref : NULL;
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
    const char *glob = NULL;
    int zips = 0;
    cJSON *asset;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(root, "assets")) {
        cJSON *aname = cJSON_GetObjectItemCaseSensitive(asset, "name");
        cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
        if (!cJSON_IsString(aname) || !cJSON_IsNumber(size))
            continue;
        if (!ends_with_zip(aname->valuestring))
            continue;
        zips++;
        if (manifest_size_in_range(size->valuedouble))
            m->size = (size_t)size->valuedouble;
        copy_str(m->url, sizeof(m->url),
                 cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url"));
    }
    cJSON_Delete(root);
    if (zips != 1 || !m->rev || !m->version[0] || !sources_release_url(m->repo, m->url) ||
        !m->size) {
        logline("origin: %s/%s %s: %d zip%s%s, rev %u", repo->owner, repo->name,
                tag[0] ? tag : "no tag", zips, zips == 1 ? "" : "s",
                glob ? " matching the file's asset" : "", m->rev);
        refuse(url, REFUSED_RELEASE);
        return -1;
    }
    entry->has_release = 1;
    /* The shape of the zip, as the author stated it; install.c holds both
       to their rules, the same ones a cache entry's are held to. */
    snprintf(m->dir, sizeof(m->dir), "%.32s", file.installdir + 9);
    memcpy(m->raw, original, strlen(original) + 1);
    snprintf(m->checked_from, sizeof(m->checked_from), "%s", url);
    snprintf(m->added_from, sizeof(m->added_from), "%s", url);
    m->checked_at = (unsigned)time(NULL);
    entry->fresh = 1;
    entry->media_cached_only = 1;
    /* Direct source checks do not fetch preview media. */
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
static int walk_list(struct catalog *catalog, const struct source_list *list, int *asked,
                     int *refused) {
    int taken = 0;
    *asked = *refused = 0;
    for (int i = 0; i < list->count; i++) {
        char url[SOURCE_URL];
        sources_repo_url(&list->repo[i], url, sizeof(url));
        if (catalog_find_repo(catalog, url) >= 0)
            continue;
        snprintf(g_progress, sizeof(g_progress), "origin: %d of %d", i + 1, list->count);
        (*asked)++;
        int rc = take_origin(catalog, &list->repo[i]);
        if (rc > 0)
            taken++;
        else if (rc < 0)
            (*refused)++;
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
        taken = take_cache(catalog, url, CACHE_LIVE_OR_SAVED);
        if (taken < 0)
            logline("source %d: catalog %s", at, g_too_large ? "too large" : "unreachable");
        else
            logline("source %d: catalog, %d apps", at, taken);
        return taken;

    case SOURCE_CATALOG_BASE: {
        char json[SOURCE_URL], txt[SOURCE_URL];
        if (snprintf(json, sizeof(json), "%scatalog.json", url) >= (int)sizeof(json) ||
            snprintf(txt, sizeof(txt), "%scatalog.txt", url) >= (int)sizeof(txt))
            return -1;
        taken = take_cache(catalog, json, CACHE_LIVE);
        if (taken >= 0) return taken;
        if (fetch_text(txt, NULL) == 0) {
            sources_parse_list(response, &list);
            if (list.count > 0) {
                taken = walk_list(catalog, &list, &asked, &refused);
                if (taken > 0) {
                    snprintf(g_catalog_url, sizeof(g_catalog_url), "%s", txt);
                    logline("source %d: catalog.txt, %d repositories asked", at, asked);
                    return taken;
                }
            }
        }
        int too_large = g_too_large;
        taken = take_cache(catalog, json, CACHE_SAVED_ONLY);
        g_too_large = too_large;
        logline("source %d: live catalog %s, saved snapshot %s", at,
                too_large ? "too large" : "unavailable", taken >= 0 ? "used" : "missing");
        return taken;
    }

    case SOURCE_REPO:
        memset(&list, 0, sizeof(list));
        sources_parse_repo(url, &list.repo[0]);
        list.count = 1;
        taken = walk_list(catalog, &list, &asked, &refused);
        logline("source %d: repository %s/%s, %d app%s", at, list.repo[0].owner, list.repo[0].name,
                taken, refused ? ", refused" : "");
        return refused && !taken ? -1 : taken;

    default:
        break;
    }

    if (fetch_text(url, NULL) < 0) {
        logline("source %d: list unreachable", at);
        return -1;
    }
    sources_parse_list(response, &list);
    const char *cache = list.cache[0] ? list.cache : NULL;
    if (cache)
        cached = take_cache(catalog, cache, CACHE_LIVE_OR_SAVED);
    taken = walk_list(catalog, &list, &asked, &refused);
    logline("source %d: list of %d, cache %s%d apps, origin %d asked, %d apps, %d refused", at,
            list.count,
            !cache       ? "none, "
            : cached < 0 ? "unreachable, "
                         : "",
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
        if (sources_same_repo(catalog->apps[i].repo, url))
            return i;
    return -1;
}

int catalog_add_repo(struct catalog *catalog, const char *url) {
    struct source_repo repo;
    attempted_count = 0;
    if (!sources_parse_repo(url, &repo))
        return -1;
    char canonical[SOURCE_URL];
    sources_repo_url(&repo, canonical, sizeof(canonical));
    int at = catalog_find_repo(catalog, canonical);
    if (at >= 0)
        return at;
    if (take_origin(catalog, &repo) <= 0)
        return -1;
    return catalog->count - 1;
}

static void restore_installed(struct catalog *catalog) {
    for (int i = 0; i < state_count(); i++) {
        char id[96];
        snprintf(id, sizeof(id), "%s", state_id(i));
        struct installed rec;
        if (db_read(id, &rec) < 0)
            continue;
        int at = catalog_find_repo(catalog, rec.repo);
        if (at >= 0 && catalog->apps[at].fresh && !g_force && !state_check_direct(id)) {
            state_note_latest(&catalog->apps[at].release);
            continue;
        }
        struct pspdx_file file;
        char *raw = NULL;
        int n = state_read_manifest(id, &raw, &file);
        /* Self registration can precede its first manifest fetch. */
        const char *source = n >= 0 ? file.source : rec.repo;
        struct source_repo repo;
        struct catalog *one = calloc(1, sizeof(*one));
        if (one && !g_offline && sources_parse_repo(source, &repo) && take_origin(one, &repo) > 0) {
            if (at < 0 && catalog->count < MAX_APPS)
                at = catalog->count++;
            if (at >= 0) {
                struct app_entry *dst = &catalog->apps[at];
                if (dst->id[0]) {
                    memcpy(one->apps[0].icon, dst->icon, sizeof(dst->icon));
                    memcpy(one->apps[0].screenshot, dst->screenshot, sizeof(dst->screenshot));
                    memcpy(one->apps[0].video, dst->video, sizeof(dst->video));
                    memcpy(one->apps[0].sound, dst->sound, sizeof(dst->sound));
                }
                *dst = one->apps[0];
                state_note_latest(&dst->release);
                if (n < 0) {
                    char path[256];
                    storage_app_path(id, path, sizeof(path));
                    storage_write(path, dst->release.raw, strlen(dst->release.raw));
                }
            }
        } else if (at < 0 && n >= 0 && catalog->count < MAX_APPS) {
            struct app_entry *e = &catalog->apps[catalog->count++];
            memset(e, 0, sizeof(*e));
            snprintf(e->id, sizeof(e->id), "%s", id);
            snprintf(e->repo, sizeof(e->repo), "%s", source);
            snprintf(e->name, sizeof(e->name), "%s", file.name);
            snprintf(e->category, sizeof(e->category), "%s", file.category);
            snprintf(e->author, sizeof(e->author), "%s", file.author);
            snprintf(e->summary, sizeof(e->summary), "%s", file.summary);
            snprintf(e->license, sizeof(e->license), "%s", file.license);
            e->media_cached_only = 1;
            e->has_release = state_latest(id, &e->release) == 0;
            snprintf(e->release.dir, sizeof(e->release.dir), "%.32s", file.installdir + 9);
            memcpy(e->release.raw, raw, n + 1);
            settle_state(e);
        }
        if (at >= 0 && !catalog->apps[at].fresh) {
            struct manifest latest;
            if (state_latest(id, &latest) == 0 && latest.rev >= catalog->apps[at].release.rev) {
                snprintf(latest.dir, sizeof(latest.dir), "%.32s",
                         n >= 0 ? file.installdir + 9 : rec.dir);
                if (n >= 0)
                    memcpy(latest.raw, raw, n + 1);
                catalog->apps[at].release = latest;
                catalog->apps[at].has_release = 1;
            }
        }
        free(one);
        free(raw);
    }
}
int catalog_too_large(void) { return g_too_large; }
int catalog_fetch(struct catalog *catalog) {
    struct sources sources;
    memset(catalog, 0, sizeof(*catalog));
    attempted_count = 0;
    g_too_large = 0;
    g_progress[0] = 0;
    g_refused_url[0] = 0;
    sources_load(&sources);
    int answered = 0;
    for (int i = 0; i < sources.count; i++)
        if (fetch_source(catalog, i + 1, sources.url[i]) >= 0)
            answered++;
    restore_installed(catalog);
    g_force = 0;
    return answered || catalog->count ? catalog->count : -1;
}
int catalog_prepare(struct app_entry *entry) {
    struct source_repo repo;
    struct pspdx_file file;
    char why[80], url[512];
    if (!sources_parse_repo(entry->repo, &repo))
        return -1;
    if (!entry->release.raw[0]) {
        snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/HEAD/.pspdx",
                 repo.owner, repo.name);
        if (fetch_text(url, NULL) < 0)
            return -1;
        if (response_len > PSPDX_FILE_MAX)
            return -1;
        memcpy(entry->release.raw, response, response_len + 1);
    }
    if (pspdx_parse(entry->release.raw, strlen(entry->release.raw), &file, why, sizeof(why)) < 0 ||
        !sources_same_repo(file.source, entry->repo) ||
        strcmp(file.installdir + 9, entry->release.dir)) {
        logline("install: manifest and selected catalog entry disagree; refresh sources");
        entry->release.raw[0] = 0;
        return -1;
    }
    return 0;
}
int catalog_validate_source(const char *url, int repository) {
    struct catalog *probe = calloc(1, sizeof(*probe));
    if (!probe)
        return -1;
    int rc = -1;
    attempted_count = 0;
    if (repository) {
        struct source_repo repo;
        if (sources_parse_repo(url, &repo))
            rc = take_origin(probe, &repo) > 0 ? 0 : -1;
    } else {
        enum source_kind kind = sources_kind(url);
        char target[SOURCE_URL];
        if (kind == SOURCE_CATALOG_BASE) {
            if (snprintf(target, sizeof(target), "%scatalog.json", url) >= (int)sizeof(target))
                goto done;
            if (fetch_text(target, NULL) == 0 && parse(probe, target) >= 0) {
                rc = 0;
                goto done;
            }
            if (snprintf(target, sizeof(target), "%scatalog.txt", url) >= (int)sizeof(target))
                goto done;
        } else {
            snprintf(target, sizeof(target), "%s", url);
        }
        if (fetch_text(target, NULL) == 0) {
            if (kind == SOURCE_CATALOG)
                rc = parse(probe, target) >= 0 ? 0 : -1;
            else {
                struct source_list list;
                rc = sources_parse_list(response, &list) > 0 ? 0 : -1;
            }
        }
    }
done:
    free(probe);
    return rc;
}

int catalog_check_updates(struct catalog *catalog) {
    int updates = 0;
    for (int i = 0; i < catalog->count; i++) {
        struct app_entry *entry = &catalog->apps[i];
        if (entry->state == APP_NOT_INSTALLED)
            continue;
        if (!entry->has_release) {
            entry->state = APP_UNKNOWN;
            continue;
        }
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
                    if (db_write_record(&self) == 0)
                        entry->local_rev = manifest.rev;
                }
                entry->state = APP_CURRENT;
                logline("self: %s is the published release, rev %u noted", entry->local_version,
                        manifest.rev);
            } else {
                entry->state = APP_UPDATE;
                updates++;
                logline("self: %s installed, %s published", entry->local_version, manifest.version);
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
    if (!response_len)
        return;
    int fd = sceIoOpen(storage_path("PSP/PSPDX/LOGS/http.txt"),
                       PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, response, response_len);
    sceIoClose(fd);
}
