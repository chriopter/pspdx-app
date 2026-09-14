#include "text.h"
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
#include "update/gunzip.h"
#include "update/pspdx.h"
#include "update/reach.h"
#include "update/sources.h"
#include "util/runtime.h"

#ifndef CATALOG_URL /* a test build may point at a catalog on the host */
#define CATALOG_URL SOURCES_DEFAULT "catalog.json"
#endif

/* A catalog of every app on a long list, each with the history of its
   releases and a description, is some hundreds of kilobytes. Static rather
   than on a stack: it is the one buffer every fetch fills. */
static char response[512 * 1024];
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
static char g_progress[64];
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

void entry_clear(struct app_entry *entry) {
    manifest_forget(&entry->release);
    free(entry->description);
    memset(entry, 0, sizeof(*entry));
}

void catalog_free(struct catalog *catalog) {
    for (int i = 0; i < MAX_APPS; i++)
        entry_clear(&catalog->apps[i]);
    catalog->count = 0;
}

/* A catalog's description, on the heap: plain text of up to 2500 characters
   with newlines, and nothing when it is not that. */
static void copy_description(struct app_entry *entry, cJSON *value) {
    free(entry->description);
    entry->description = NULL;
    if (!cJSON_IsString(value) || !value->valuestring[0])
        return;
    int n = pspdx_characters(value->valuestring, 1);
    size_t len = strlen(value->valuestring);
    if (n < 1 || n > 2500 || !(entry->description = malloc(len + 1)))
        return;
    memcpy(entry->description, value->valuestring, len + 1);
}

/* The tags of an entry, a newline between them, as update/pspdx.h keeps
   them: strings of no control character, at most eight, and only as many as
   fit. A catalog is not held to the file's rules word for word; what does
   not fit them is left out rather than taken as it stands. */
static void copy_tags(char *dst, size_t size, cJSON *tags) {
    size_t used = 0;
    int n = 0;
    cJSON *tag;
    dst[0] = '\0';
    if (!cJSON_IsArray(tags))
        return;
    cJSON_ArrayForEach(tag, tags) {
        if (n >= PSPDX_TAGS)
            break;
        if (!cJSON_IsString(tag) || !tag->valuestring[0])
            continue;
        const unsigned char *p = (const unsigned char *)tag->valuestring;
        while (*p >= 32)
            p++;
        size_t len = strlen(tag->valuestring);
        if (*p || used + (used > 0) + len >= size)
            continue;
        if (used)
            dst[used++] = '\n';
        memcpy(dst + used, tag->valuestring, len + 1);
        used += len;
        n++;
    }
}

/* 64 hex digits into 32 bytes. 0 for anything else. */
static int parse_sha256(const char *hex, unsigned char *out) {
    if (strlen(hex) != 64 || strspn(hex, "0123456789abcdefABCDEF") != 64)
        return 0;
    for (int i = 0; i < 32; i++) {
        unsigned byte;
        sscanf(hex + 2 * i, "%2x", &byte);
        out[i] = (unsigned char)byte;
    }
    return 1;
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
        memcpy(entry->local_sha256, installed.sha256, sizeof(entry->local_sha256));
        entry->local_has_sha = 0;
        for (int i = 0; i < 32; i++)
            entry->local_has_sha |= installed.sha256[i];
        /* Installed from a list's .pspdx: an update of an entry whose file is
           still to be read reads that one when the repository has none. */
        if (!entry->release.raw && !entry->release.manifest_url[0])
            snprintf(entry->release.manifest_url, sizeof(entry->release.manifest_url), "%s",
                     installed.manifest_url);
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
            "https://chriopter.github.io/pspdx/schema/catalog-v1.json")) {
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
        entry_clear(entry);
        copy_str(entry->id, sizeof(entry->id), cJSON_GetObjectItemCaseSensitive(app, "id"));
        copy_str(entry->name, sizeof(entry->name), cJSON_GetObjectItemCaseSensitive(app, "name"));
        copy_str(entry->author, sizeof(entry->author),
                 cJSON_GetObjectItemCaseSensitive(app, "author"));
        copy_str(entry->summary, sizeof(entry->summary),
                 cJSON_GetObjectItemCaseSensitive(app, "summary"));
        copy_tags(entry->tags, sizeof(entry->tags), cJSON_GetObjectItemCaseSensitive(app, "tags"));
        copy_str(entry->listed_by, sizeof(entry->listed_by),
                 cJSON_GetObjectItemCaseSensitive(app, "listed_by"));
        if (strncmp(entry->listed_by, "https://", 8))
            entry->listed_by[0] = '\0';
        /* What the app is decides what may be done with it: a homebrew is
           installed, a plugin or an ISO only listed, and a type this version
           has never heard of is not an app it can say anything true about. */
        cJSON *kind = cJSON_GetObjectItemCaseSensitive(app, "type");
        if (kind)
            copy_str(entry->type, sizeof(entry->type), kind);
        else
            strcpy(entry->type, "homebrew");
        int homebrew = !strcmp(entry->type, "homebrew");
        int known_type = homebrew || !strcmp(entry->type, "plugin") || !strcmp(entry->type, "iso");
        copy_str(entry->license, sizeof(entry->license),
                 cJSON_GetObjectItemCaseSensitive(app, "license"));
        copy_str(entry->repo, sizeof(entry->repo), cJSON_GetObjectItemCaseSensitive(app, "source"));

        /* The newest release is the first, and the only one a console
           installs or compares; the rest are history. */
        cJSON *release = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(app, "releases"), 0);
        if (cJSON_IsObject(release)) {
            struct manifest *m = &entry->release;
            strncpy(m->id, entry->id, sizeof(m->id) - 1);
            cJSON *published = cJSON_GetObjectItemCaseSensitive(release, "published_at");
            cJSON *tag = cJSON_GetObjectItemCaseSensitive(release, "tag");
            cJSON *size = cJSON_GetObjectItemCaseSensitive(release, "size");
            cJSON *sha = cJSON_GetObjectItemCaseSensitive(release, "sha256");
            /* The rules install.h states: these fields go straight into
               a download and an unpack. */
            int ok = 1;
            m->rev = iso8601(cJSON_IsString(published) ? published->valuestring : NULL);
            if (!m->rev) ok = 0;
            if (cJSON_IsNumber(size) && manifest_size_in_range(size->valuedouble))
                m->size = (size_t)size->valuedouble;
            else
                ok = 0;
            copy_str(m->url, sizeof(m->url), cJSON_GetObjectItemCaseSensitive(release, "url"));
            /* A tag is 1 to 64 characters and no control character; the
               version is kept in bytes enough for 64 of four bytes each. */
            int characters = cJSON_IsString(tag) ? pspdx_characters(tag->valuestring, 0) : -1;
            if (characters >= 1 && characters <= 64) {
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
            /* A homebrew without one goes where the file's rule puts it, and
               a plugin or an ISO names no folder under PSP/GAME at all. */
            cJSON *target = cJSON_GetObjectItemCaseSensitive(app, "installdir");
            char folder[42];
            struct source_repo named;
            if (!homebrew) {
                if (target)
                    entry->has_release = 0;
            } else if (target) {
                if (!cJSON_IsString(target) || !pspdx_install_dir(target->valuestring))
                    entry->has_release = 0;
                else
                    snprintf(m->dir, sizeof(m->dir), "%s", target->valuestring + 9);
            } else {
                int github = sources_parse_repo(entry->repo, &named);
                pspdx_default_dir(github ? named.name : NULL, entry->name, folder, sizeof(folder));
                if (!pspdx_install_dir(folder))
                    entry->has_release = 0;
                else
                    snprintf(m->dir, sizeof(m->dir), "%s", folder + 9);
            }
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
                    manifest_keep_raw(m, raw, strlen(raw));
                free(raw);
            }
#endif
        }

        char shot[256];
        cJSON *media = cJSON_GetObjectItemCaseSensitive(app, "media");
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(media, "icon"));
        asset_url(base, shot, entry->icon, sizeof(entry->icon));
        copy_str(shot, sizeof(shot),
                 cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(media, "screenshots"), 0));
        asset_url(base, shot, entry->screenshot, sizeof(entry->screenshot));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(media, "video"));
        asset_url(base, shot, entry->video, sizeof(entry->video));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(media, "sound"));
        asset_url(base, shot, entry->sound, sizeof(entry->sound));

        /* An id names a directory on the stick and a file in the cache: one
           that cannot be a path component is not an entry. */
        if (!manifest_id_is_safe(entry->id) || !entry->name[0])
            continue;
        if (!entry->has_release || !known_type)
            continue;
        /* The id is what the source and the list make of it, and an entry
           that says another is not believed about the rest either. Away
           from GitHub there is no release this version can check the
           download against, so such an app is listed and not installed. */
        struct source_repo source;
        char expected[96];
        int github = sources_parse_repo(entry->repo, &source);
        if (github)
            sources_repo_id(&source, expected, sizeof(expected));
        else if (strncmp(entry->repo, "https://", 8))
            continue;
        else if (sources_listed_id(entry->listed_by, entry->name, expected, sizeof(expected)) < 0) {
            /* Away from GitHub the id is the list's and the name's, so an entry
               that leaves either with nothing to make one of is no app. Said,
               since the list meant to carry it. */
            logline("catalog: %s is from outside GitHub and %s; dropped", entry->id,
                    !entry->listed_by[0] ? "names no listed_by"
                                         : "its name or its list's host has no letter or digit");
            continue;
        }
        if (strcmp(expected, entry->id) ||
            (github && !sources_release_url(entry->repo, entry->release.url)))
            continue;
        entry->unsupported = !github || !homebrew;
        if (!github)
            logline("catalog: %s comes from outside GitHub; listed, not installable yet", entry->id);
        if (has_id(catalog, entry->id))
            continue;
        struct installed local;
        if (db_read(entry->id, &local) < 0 && catalog->count >= MAX_APPS - state_count())
            continue;
        /* One directory is one app's: a second entry that wants the same
           name would only be refused at install time, so the first keeps it.
           What cannot be installed claims no directory, and is not kept out
           by one either. */
        int claims = entry->release.dir[0] && !entry->unsupported;
        int held;
        for (held = 0; held < catalog->count && claims; held++)
            if (!catalog->apps[held].unsupported &&
                !strcasecmp(catalog->apps[held].release.dir, entry->release.dir))
                break;
        if (claims && held < catalog->count) {
            logline("catalog: %s wants PSP/GAME/%s, which %s has; dropped", entry->id,
                    entry->release.dir, catalog->apps[held].id);
            continue;
        }

        copy_description(entry, cJSON_GetObjectItemCaseSensitive(app, "description"));
        settle_state(entry);
        catalog->count++;
        taken++;
    }
    int empty = cJSON_GetArraySize(apps) == 0;
    g_parsed_empty = empty;
    cJSON_Delete(root);
    return taken || empty || before > 0 ? taken : -1;
}

/* A gzipped body is inflated as it arrives, and the room is counted in
   text: what has to fit is the catalog, not what it weighed on the wire.
   One byte stays free for the terminator. */
static struct gunzip g_gunzip;
static int response_sink(void *ctx, const void *data, size_t len) {
    (void)ctx;
    int rc = gunzip_feed(&g_gunzip, data, len, response, sizeof(response) - 1, &response_len);
    if (rc == GUNZIP_FULL)
        g_too_large = 1;
    return rc;
}

/* One text file into the response buffer. Returns 0 when it arrived. */
static int fetch_text(const char *url, struct https_result *out) {
    struct https_result r;
    if (g_offline)
        return -1;
    response_len = 0;
    g_too_large = 0;
    /* A catalog is JSON that GitHub Pages sends at a quarter of its size
       when asked, over a radio where every kilobyte is felt. Only a catalog
       asks: the switch goes off again after it, so a release ZIP, which
       nothing makes smaller, is never offered compressed. */
    https_set_accept_gzip(sources_kind(url) == SOURCE_CATALOG);
    gunzip_begin(&g_gunzip);
    int rc = https_get(url, response_sink, NULL, NULL, NULL, &r);
    https_set_accept_gzip(0);
    const char *bad = gunzip_end(&g_gunzip, r.content_encoding);
    if (out)
        *out = r;
    if (rc != 0 || r.status != 200 || bad) {
        if (g_too_large)
            logline("fetch: %s is larger than the %u KB there is room for", url,
                    (unsigned)(sizeof(response) / 1024));
        else if (bad && r.status == 200 && (rc == 0 || g_gunzip.refused))
            logline("fetch: %s, %s", bad, url);
        else
            logline("fetch: rc=%d status=%ld %s", rc, r.status, url);
        return -1;
    }
    if (gunzip_packed(&g_gunzip))
        logline("fetch: %lu bytes gzipped, %lu inflated, %s", (unsigned long)g_gunzip.wire,
                (unsigned long)response_len, url);
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
    int y, mo, d, h = 0, mi = 0, sec = 0;
    /* A release entered by hand may know only its day, "2024-12-20"; it
       counts from that day's midnight, which orders it and dates it, and
       nothing more is asked of it. */
    size_t n = text ? strlen(text) : 0;
    if ((n != 20 && n != 10) || text[4] != '-' || text[7] != '-' ||
        (n == 20 && (text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z')))
        return 0;
    for (size_t i = 0; i < (n == 20 ? 19 : 10); i++)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 &&
            !isdigit((unsigned char)text[i]))
            return 0;
    if (n == 20 ? sscanf(text, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6
                : sscanf(text, "%d-%d-%d", &y, &mo, &d) != 3)
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

/* The response buffer's text, on the heap, for as long as another fetch
   would write over it. */
static char *keep_response(void) {
    char *text = malloc(response_len + 1);
    if (text)
        memcpy(text, response, response_len + 1);
    return text;
}

/* A .pspdx a list serves for a repository that has none, held to v1 and to
   what makes it a stand-in rather than a file anyone could put up: the list
   that vouches for it, in listed_by, and a GitHub repository, whose releases
   are the only ones this version can ask for. 0 with the file parsed, or -1
   with the reason in the log. */
static int standin_ok(const char *url, const char *text, size_t len, struct pspdx_file *file) {
    char reason[64];
    if (pspdx_parse(text, len, file, reason, sizeof(reason)) < 0) {
        logline("origin: the list's .pspdx at %s refused: %s", url, reason);
        return -1;
    }
    if (!file->listed_by[0]) {
        logline("origin: the list's .pspdx at %s names no listed_by; refused", url);
        return -1;
    }
    if (strncmp(file->source, "https://github.com/", 19)) {
        logline("origin: the list's .pspdx at %s names a source outside GitHub, which only a "
                "catalog can list; skipped", url);
        return -1;
    }
    return 0;
}

/* A list's line that is a .pspdx rather than a repository: the file, read
   to learn which repository it stands in for. Its text on the heap and the
   repository, or NULL. */
static char *read_standin(const char *url, struct source_repo *repo) {
    if (fetch_text(url, NULL) < 0) {
        logline("origin: the list's .pspdx at %s did not load", url);
        return NULL;
    }
    struct pspdx_file file;
    char *text = response_len <= PSPDX_FILE_MAX ? keep_response() : NULL;
    if (!text)
        logline("origin: the list's .pspdx at %s is over %u bytes", url, PSPDX_FILE_MAX);
    else if (standin_ok(url, text, response_len, &file) < 0 ||
             !sources_parse_repo(file.source, repo)) {
        free(text);
        text = NULL;
    }
    return text;
}

/* One repository asked at the origin and made into an entry: its .pspdx
   at raw.githubusercontent.com first, since that file is the consent and a
   repository without one is not an app; then api.github.com for the
   release. The pictures are four more URLs under the media directory, not
   fetched here -- the card and the row ask for them when the cursor
   arrives, exactly as they do from a cache.

   standin is a list's .pspdx for the repository, by its URL, or NULL, and
   standin_text what was read there already, or NULL to read it here. The
   repository's own file is asked for first and wins; the list's stands in
   only when the repository answers that it has none. Returns 1 with the
   entry filled, -1 refused, with why kept for the gear tab. */
static int origin_entry(struct app_entry *entry, const struct source_repo *repo,
                        const char *standin, const char *standin_text) {
    char url[SOURCE_URL], api[SOURCE_URL];
    sources_repo_url(repo, url, sizeof(url));
    for (int i = 0; i < attempted_count; i++)
        if (sources_same_url(attempted[i], url))
            return -1;
    if (attempted_count >= MAX_APPS + LIST_REPOS)
        return -1;
    snprintf(attempted[attempted_count++], SOURCE_URL, "%s", url);
    entry_clear(entry);

    struct pspdx_file file;
    char reason[64];
    struct https_result answer;
    memset(&answer, 0, sizeof(answer));
    snprintf(api, sizeof(api), "https://raw.githubusercontent.com/%s/%s/%s/.pspdx", repo->owner,
             repo->name, repo->ref);
    char *text = NULL;
    int served = 0;
    if (fetch_text(api, &answer) == 0) {
        if (standin)
            logline("origin: %s/%s has its own .pspdx; the list's at %s is not used", repo->owner,
                    repo->name, standin);
        text = response_len <= PSPDX_FILE_MAX ? keep_response() : NULL;
    } else if (standin && answer.status == 404) {
        served = 1;
        if (standin_text) {
            text = malloc(strlen(standin_text) + 1);
            if (text)
                strcpy(text, standin_text);
        } else if (fetch_text(standin, NULL) == 0) {
            text = response_len <= PSPDX_FILE_MAX ? keep_response() : NULL;
        } else {
            logline("origin: the list's .pspdx at %s did not load", standin);
            refuse(url, REFUSED_PSPDX);
            return -1;
        }
    } else {
        logline("origin: %s/%s has no .pspdx at %s, not listed", repo->owner, repo->name,
                repo->ref);
        refuse(url, REFUSED_PSPDX);
        return -1;
    }
    size_t len = text ? strlen(text) : 0;
    if (!text) {
        logline("origin: %s/%s .pspdx refused: too large", repo->owner, repo->name);
        goto refused;
    }
    if (served) {
        if (standin_ok(standin, text, len, &file) < 0)
            goto refused;
        logline("origin: %s/%s has no .pspdx of its own; the list's at %s stands in", repo->owner,
                repo->name, standin);
    } else if (pspdx_parse(text, len, &file, reason, sizeof(reason)) < 0) {
        logline("origin: %s/%s .pspdx refused: %s", repo->owner, repo->name, reason);
        goto refused;
    }
    /* A file whose project lives away from GitHub has no releases this
       path could ask for: only a catalog can list it. */
    if (strncmp(file.source, "https://github.com/", 19)) {
        logline("origin: %s/%s .pspdx names a source outside GitHub, which only a catalog "
                "can list; skipped", repo->owner, repo->name);
        goto refused;
    }
    if (!sources_same_url(file.source, url)) {
        logline("origin: %s/%s .pspdx names %s as its source; refused", repo->owner, repo->name,
                file.source);
        goto refused;
    }
    logline("origin: %s/%s .pspdx: %s, %s", repo->owner, repo->name, file.name, file.type);

    sources_repo_id(repo, entry->id, sizeof(entry->id));
    snprintf(entry->repo, sizeof(entry->repo), "%s", url);
    snprintf(entry->name, sizeof(entry->name), "%s", file.name);
    memcpy(entry->tags, file.tags, sizeof(entry->tags));
    memcpy(entry->listed_by, file.listed_by, sizeof(entry->listed_by));
    snprintf(entry->type, sizeof(entry->type), "%s", file.type);
    entry->unsupported = !pspdx_type_installable(file.type);
    snprintf(entry->author, sizeof(entry->author), "%s",
             file.author[0] ? file.author : repo->owner);
    snprintf(entry->summary, sizeof(entry->summary), "%s", file.summary);
    snprintf(entry->license, sizeof(entry->license), "%s", file.license);

    /* What the file says is all there is. A summary or a licence it leaves
       out stays empty rather than costing one of the sixty requests an
       hour GitHub allows an address, and the author it leaves out is the
       account in the URL. */

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
        free(text);
        entry_clear(entry);
        refuse(url, REFUSED_RELEASE);
        return -1;
    }
    struct manifest *m = &entry->release;
    snprintf(m->id, sizeof(m->id), "%s", entry->id);
    snprintf(m->repo, sizeof(m->repo), "%s", url);
    /* A tag is 1 to 64 characters, as a catalog's is; one past that makes
       no version, and the release is refused below rather than cut short. */
    char tag[VERSION_SIZE];
    copy_str(tag, sizeof(tag), cJSON_GetObjectItemCaseSensitive(root, "tag_name"));
    int characters = pspdx_characters(tag, 0);
    if (characters >= 1 && characters <= 64)
        snprintf(m->version, sizeof(m->version), "%s", tag[0] == 'v' ? tag + 1 : tag);
    cJSON *published = cJSON_GetObjectItemCaseSensitive(root, "published_at");
    m->rev = iso8601(cJSON_IsString(published) ? published->valuestring : NULL);
    /* The zip: the only one on the release. */
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
        /* GitHub states an asset's SHA-256 as "sha256:<hex>". Where it
           does, the download is held to it, and an update is told by it. */
        cJSON *digest = cJSON_GetObjectItemCaseSensitive(asset, "digest");
        memset(m->sha256, 0, sizeof(m->sha256));
        if (cJSON_IsString(digest) && !strncmp(digest->valuestring, "sha256:", 7) &&
            !parse_sha256(digest->valuestring + 7, m->sha256))
            memset(m->sha256, 0, sizeof(m->sha256));
    }
    cJSON_Delete(root);
    if (zips != 1 || !m->rev || !m->version[0] || !sources_release_url(m->repo, m->url) ||
        !m->size) {
        logline("origin: %s/%s %s: %d zip%s, rev %u%s", repo->owner, repo->name,
                tag[0] ? tag : "no tag", zips, zips == 1 ? "" : "s", m->rev,
                m->version[0] || !tag[0] ? "" : ", a tag of more than 64 characters");
        free(text);
        entry_clear(entry);
        refuse(url, REFUSED_RELEASE);
        return -1;
    }
    entry->has_release = 1;
    /* The shape of the zip, as the author stated it; install.c holds both
       to their rules, the same ones a cache entry's are held to. */
    snprintf(m->dir, sizeof(m->dir), "%.32s", file.installdir + 9);
    /* The file goes with the entry, and with it where it was read when a
       list served it, which the record keeps for the checks to come. */
    m->raw = text;
    entry->description = pspdx_description(text, len);
    if (served)
        snprintf(m->manifest_url, sizeof(m->manifest_url), "%s", standin);
    snprintf(m->checked_from, sizeof(m->checked_from), "%s", url);
    snprintf(m->added_from, sizeof(m->added_from), "%s", url);
    m->checked_at = (unsigned)time(NULL);
    entry->fresh = 1;
    entry->media_cached_only = 1;
    /* Direct source checks do not fetch preview media. */
    return 1;
refused:
    free(text);
    entry_clear(entry);
    refuse(url, REFUSED_PSPDX);
    return -1;
}

/* The same, into the catalog's next slot: 1 taken, 0 already there, -1
   refused or no room. */
static int take_origin(struct catalog *catalog, const struct source_repo *repo,
                       const char *standin, const char *standin_text) {
    char id[96];
    sources_repo_id(repo, id, sizeof(id));
    if (catalog->count >= MAX_APPS)
        return -1;
    if (has_id(catalog, id))
        return 0;
    struct installed local;
    if (db_read(id, &local) < 0 && catalog->count >= MAX_APPS - state_count())
        return -1;
    struct app_entry *entry = &catalog->apps[catalog->count];
    if (origin_entry(entry, repo, standin, standin_text) < 0)
        return -1;
    settle_state(entry);
    catalog->count++;
    catalog->total++;
    return 1;
}

/* A list's .pspdx line, read and its repository asked at the origin with it:
   1 taken, 0 already there, -1 refused. */
static int take_standin(struct catalog *catalog, const char *url) {
    struct source_repo repo;
    char *text = read_standin(url, &repo);
    if (!text)
        return -1;
    char canonical[SOURCE_URL];
    sources_repo_url(&repo, canonical, sizeof(canonical));
    int rc = catalog_find_repo(catalog, canonical) >= 0 ? 0 : take_origin(catalog, &repo, url, text);
    free(text);
    return rc;
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
        if (list->pspdx[i][0]) {
            /* Named on the status line by its file, the way a repository is
               by its name. */
            const char *file = strrchr(list->pspdx[i], '/');
            file = file ? file + 1 : list->pspdx[i];
            snprintf(g_progress, sizeof(g_progress), T_STATUS_ORIGIN, file, i + 1, list->count);
            (*asked)++;
            int rc = take_standin(catalog, list->pspdx[i]);
            if (rc > 0)
                taken++;
            else if (rc < 0)
                (*refused)++;
            continue;
        }
        sources_repo_url(&list->repo[i], url, sizeof(url));
        if (catalog_find_repo(catalog, url) >= 0)
            continue;
        snprintf(g_progress, sizeof(g_progress), T_STATUS_ORIGIN, list->repo[i].name, i + 1,
                 list->count);
        (*asked)++;
        int rc = take_origin(catalog, &list->repo[i], NULL, NULL);
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
    /* Static: a list of lines of URLs is tens of kilobytes, and only the
       sync thread fetches sources. */
    static struct source_list list;
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

    case SOURCE_PSPDX:
        /* A .pspdx as a source is a list of one such line. */
        taken = take_standin(catalog, url);
        logline("source %d: .pspdx, %d app%s", at, taken > 0, taken < 0 ? ", refused" : "");
        return taken;

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
    if (take_origin(catalog, &repo, NULL, NULL) <= 0)
        return -1;
    return catalog->count - 1;
}

/* How long an answer from an app's own repository stands before it is
   asked again, unless the check is forced. */
#define DIRECT_EVERY_S (6u * 3600u)

static void restore_installed(struct catalog *catalog) {
    unsigned now = (unsigned)time(NULL);
    int stale = catalog->generated && catalog->generated + 24u * 3600u < now;
    for (int i = 0; i < state_count(); i++) {
        char id[96];
        snprintf(id, sizeof(id), "%s", state_id(i));
        struct installed rec;
        if (db_read(id, &rec) < 0)
            continue;
        int at = catalog_find_repo(catalog, rec.repo);
        /* A live catalog that is not a day behind answers for what it
           lists. Past that -- an app no catalog lists, a list served from
           the cache, a stamp a day old -- the app is asked at its own
           repository, but not more often than every six hours, since GitHub
           allows an address sixty requests an hour; what the record last
           heard from that repository stands in between. An answer a catalog
           gave does not count as one, and a forced check asks regardless. */
        int covered = at >= 0 && catalog->apps[at].fresh && !stale;
        if (covered && !g_force) {
            state_note_latest(&catalog->apps[at].release);
            continue;
        }
        struct manifest seen;
        int due = g_force || state_latest(id, &seen) < 0 || !seen.checked_at ||
                  !sources_same_repo(seen.checked_from, seen.repo) ||
                  seen.checked_at + DIRECT_EVERY_S < now;
        int fetched = 0;
        struct pspdx_file file;
        char *raw = NULL;
        int n = state_read_manifest(id, &raw, &file);
        /* Self registration can precede its first manifest fetch. */
        const char *source = n >= 0 ? file.source : rec.repo;
        struct source_repo repo;
        /* One entry to ask into, not a catalog: an app at a time. */
        int ask = due && !g_offline && sources_parse_repo(source, &repo);
        struct app_entry *one = ask ? calloc(1, sizeof(*one)) : NULL;
        /* The status line follows the apps being asked, the way it follows a
           list being walked; a full check is nearly all of this. */
        if (one)
            snprintf(g_progress, sizeof(g_progress), T_STATUS_ORIGIN, repo.name, i + 1,
                     state_count());
        /* An app installed from a list's .pspdx asks its repository first all
           the same, and the list's copy only while the repository has none. */
        if (one && origin_entry(one, &repo, rec.manifest_url[0] ? rec.manifest_url : NULL,
                                NULL) > 0) {
            fetched = 1;
            int known = at >= 0;
            if (at < 0 && catalog->count < MAX_APPS)
                at = catalog->count++;
            if (at >= 0) {
                struct app_entry *dst = &catalog->apps[at];
                if (known) {
                    memcpy(one->icon, dst->icon, sizeof(dst->icon));
                    memcpy(one->screenshot, dst->screenshot, sizeof(dst->screenshot));
                    memcpy(one->video, dst->video, sizeof(dst->video));
                    memcpy(one->sound, dst->sound, sizeof(dst->sound));
                }
                /* The entry moves over whole, its text and description with
                   it, and the one it came in is left owning nothing. */
                entry_clear(dst);
                *dst = *one;
                memset(one, 0, sizeof(*one));
                settle_state(dst);
                state_note_latest(&dst->release);
                if (n < 0) {
                    char path[256];
                    storage_app_path(id, path, sizeof(path));
                    storage_write(path, dst->release.raw, strlen(dst->release.raw));
                }
            }
        } else if (at < 0 && n >= 0 && catalog->count < MAX_APPS) {
            struct app_entry *e = &catalog->apps[catalog->count++];
            entry_clear(e);
            snprintf(e->id, sizeof(e->id), "%s", id);
            snprintf(e->repo, sizeof(e->repo), "%s", source);
            snprintf(e->name, sizeof(e->name), "%s", file.name);
            memcpy(e->tags, file.tags, sizeof(e->tags));
            memcpy(e->listed_by, file.listed_by, sizeof(e->listed_by));
            snprintf(e->type, sizeof(e->type), "%s", file.type);
            e->unsupported = !pspdx_type_installable(file.type);
            snprintf(e->author, sizeof(e->author), "%s", file.author);
            snprintf(e->summary, sizeof(e->summary), "%s", file.summary);
            snprintf(e->license, sizeof(e->license), "%s", file.license);
            e->media_cached_only = 1;
            e->has_release = state_latest(id, &e->release) == 0;
            snprintf(e->release.dir, sizeof(e->release.dir), "%.32s", file.installdir + 9);
            /* The saved file is the entry's now. */
            e->release.raw = raw;
            raw = NULL;
            e->description = pspdx_description(e->release.raw, n);
            settle_state(e);
        }
        if (at >= 0 && !fetched && (!catalog->apps[at].fresh || stale)) {
            struct manifest latest;
            if (state_latest(id, &latest) == 0 && latest.rev >= catalog->apps[at].release.rev) {
                struct app_entry *e = &catalog->apps[at];
                snprintf(latest.dir, sizeof(latest.dir), "%.32s",
                         n >= 0 ? file.installdir + 9 : rec.dir);
                manifest_forget(&e->release);
                e->release = latest;
                if (n >= 0) {
                    e->release.raw = raw;
                    raw = NULL;
                }
                e->has_release = 1;
            }
        }
        if (one)
            entry_clear(one);
        free(one);
        free(raw);
    }
}
int catalog_too_large(void) { return g_too_large; }
int catalog_fetch(struct catalog *catalog) {
    struct sources sources;
    catalog_free(catalog);
    memset(catalog, 0, sizeof(*catalog));
    attempted_count = 0;
    g_too_large = 0;
    g_progress[0] = 0;
    g_refused_url[0] = 0;
    sources_load(&sources);
    reach_reset();
    int answered = 0;
    for (int i = 0; i < sources.count; i++) {
        if (fetch_source(catalog, i + 1, sources.url[i]) >= 0)
            answered++;
        else
            reach_failed(sources.url[i]);
    }
    restore_installed(catalog);
    g_progress[0] = 0;
    g_force = 0;
    return answered || catalog->count ? catalog->count : -1;
}
/* The .pspdx a list serves for an entry whose repository has none, read into
   the entry with where it was read: the one its record remembers, or else
   the one the text list beside its catalog names -- a site's catalog.txt, the
   lines its catalog.json was built from. 0 when one stood in. */
static int find_standin(struct app_entry *entry) {
    struct pspdx_file file;
    struct manifest *m = &entry->release;
    if (m->manifest_url[0]) {
        if (fetch_text(m->manifest_url, NULL) == 0 && response_len <= PSPDX_FILE_MAX &&
            standin_ok(m->manifest_url, response, response_len, &file) == 0 &&
            sources_same_repo(file.source, entry->repo))
            return manifest_keep_raw(m, response, response_len);
        logline("install: %s has no .pspdx, and the list's at %s did not stand in", entry->id,
                m->manifest_url);
        return -1;
    }
    size_t n = strlen(m->added_from);
    char txt[SOURCE_URL];
    if (n < 12 || strcmp(m->added_from + n - 12, "catalog.json") ||
        snprintf(txt, sizeof(txt), "%.*scatalog.txt", (int)(n - 12), m->added_from) >=
            (int)sizeof(txt) ||
        fetch_text(txt, NULL) < 0) {
        logline("install: %s has no .pspdx, and no list's copy is known", entry->id);
        return -1;
    }
    static struct source_list list;
    sources_parse_list(response, &list);
    for (int i = 0; i < list.count; i++) {
        if (!list.pspdx[i][0] || fetch_text(list.pspdx[i], NULL) < 0 ||
            response_len > PSPDX_FILE_MAX ||
            standin_ok(list.pspdx[i], response, response_len, &file) < 0 ||
            !sources_same_repo(file.source, entry->repo))
            continue;
        snprintf(m->manifest_url, sizeof(m->manifest_url), "%s", list.pspdx[i]);
        return manifest_keep_raw(m, response, response_len);
    }
    logline("install: %s has no .pspdx, and %s serves none for it", entry->id, txt);
    return -1;
}

int catalog_prepare(struct app_entry *entry) {
    struct source_repo repo;
    struct pspdx_file file;
    char why[80], url[512];
    /* What is listed and not installable says so here, before anything is
       fetched for it. */
    if (entry->unsupported) {
        logline("install: %s cannot be installed yet (type %s)", entry->id, entry->type);
        return -1;
    }
    if (!sources_parse_repo(entry->repo, &repo))
        return -1;
    if (!entry->release.raw) {
        /* The repository's own file first, which always wins; a list's
           copy only when the repository says it has none. */
        struct https_result answer;
        memset(&answer, 0, sizeof(answer));
        snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/HEAD/.pspdx",
                 repo.owner, repo.name);
        if (fetch_text(url, &answer) == 0) {
            if (response_len > PSPDX_FILE_MAX ||
                manifest_keep_raw(&entry->release, response, response_len) < 0)
                return -1;
            entry->release.manifest_url[0] = '\0';
        } else if (answer.status != 404 || find_standin(entry) < 0) {
            return -1;
        }
    }
    if (pspdx_parse(entry->release.raw, strlen(entry->release.raw), &file, why, sizeof(why)) < 0 ||
        !sources_same_repo(file.source, entry->repo) ||
        strcmp(file.installdir + 9, entry->release.dir) ||
        (entry->release.manifest_url[0] && !file.listed_by[0])) {
        logline("install: manifest and selected catalog entry disagree; refresh sources");
        manifest_forget(&entry->release);
        return -1;
    }
    return 0;
}
int catalog_validate_source(const char *url, int repository) {
    int rc = -1;
    attempted_count = 0;
    enum source_kind kind = sources_kind(url);
    /* A repository or a list's .pspdx is one app: asked into one entry. */
    if (repository || kind == SOURCE_PSPDX) {
        struct app_entry *one = calloc(1, sizeof(*one));
        struct source_repo repo;
        char *text = NULL;
        if (!one)
            return -1;
        if (repository && sources_parse_repo(url, &repo))
            rc = origin_entry(one, &repo, NULL, NULL) > 0 ? 0 : -1;
        else if (!repository && (text = read_standin(url, &repo)))
            rc = origin_entry(one, &repo, url, text) > 0 ? 0 : -1;
        free(text);
        entry_clear(one);
        free(one);
        return rc;
    }
    struct catalog *probe = calloc(1, sizeof(*probe));
    if (!probe)
        return -1;
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
            static struct source_list list;
            rc = sources_parse_list(response, &list) > 0 ? 0 : -1;
        }
    }
done:
    catalog_free(probe);
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
        /* Read through a pointer: a copy of a manifest would share its text. */
        const struct manifest *manifest = &entry->release;
        entry->remote_rev = manifest->rev;
        /* The same size on both sides, and never the same bytes. */
        memcpy(entry->remote_version, manifest->version, sizeof(entry->remote_version));
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
            size_t n = strlen(manifest->version);
            int same = strncmp(entry->local_version, manifest->version, n) == 0 &&
                       (entry->local_version[n] == '\0' || entry->local_version[n] == '-');
            if (same) {
                struct installed self;
                if (db_read(entry->id, &self) == 0) {
                    self.rev = manifest->rev;
                    if (db_write_record(&self) == 0)
                        entry->local_rev = manifest->rev;
                }
                entry->state = APP_CURRENT;
                logline("self: %s is the published release, rev %u noted", entry->local_version,
                        manifest->rev);
            } else {
                entry->state = APP_UPDATE;
                updates++;
                logline("self: %s installed, %s published", entry->local_version, manifest->version);
            }
            continue;
        }
        /* An update is another zip than the one on the stick. The catalog
           hashes every release, GitHub states a hash for an asset, and an
           install records the hash of what it wrote, so nearly always both
           are known, and then a date says nothing about it: a release
           entered by hand may carry only its day. A record from before hashes
           were kept, or a release nobody hashed, is compared by its time. */
        int newer = entry->local_has_sha && manifest_has_sha256(manifest)
                        ? memcmp(entry->local_sha256, manifest->sha256, 32) != 0
                        : manifest->rev > entry->local_rev;
        if (newer) {
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
