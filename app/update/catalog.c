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
/* What the fetch of all sources keeps of those answers: a source that went
   without because its catalog did not fit, which every later fetch_text
   would start over; and whether the source just fetched came from nothing
   but the snapshot saved on the stick. */
static int g_source_too_large, g_took_saved;
/* A text -- a catalog, a list, a .pspdx, an answer of GitHub's API -- has
   this long in all, however it arrives: the library's stall timeout is per
   read, and a server that sends a byte now and then would otherwise hold
   the sync thread for as long as it liked. A catalog that fits the buffer
   takes seconds on 802.11b. */
#define TEXT_LIMIT_S 120u
static char attempted[MAX_APPS + LIST_REPOS][SOURCE_URL];
static int attempted_count;
void catalog_offline(int value) { g_offline = value; }
void catalog_force_sources(void) { g_force = 1; }

/* The cache last taken: the info band names its host, and the bench fetches
   it. Before any source has answered it is the built-in one. */
static char g_catalog_url[SOURCE_URL] = CATALOG_URL;
static char g_progress[64];
static char g_refused_url[SOURCE_URL];
static char g_refused_folder[64];
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

/* 64 hex digits into 32 bytes. 0 for anything else, and for 64 zeros,
   which is no hash anyone computed. */
static int parse_sha256(const char *hex, unsigned char *out) {
    if (strlen(hex) != 64 || strspn(hex, "0123456789abcdefABCDEF") != 64 ||
        strspn(hex, "0") == 64)
        return 0;
    for (int i = 0; i < 32; i++) {
        unsigned byte;
        sscanf(hex + 2 * i, "%2x", &byte);
        out[i] = (unsigned char)byte;
    }
    return 1;
}

/* Whether a value holds a string, a key among them, with a byte below 32
   other than the newline, the tab and the carriage return JSON text may
   carry: a NUL escape, which pspdx_json_mark_nul made one, above all. */
static int holds_control(const cJSON *v) {
    for (; v; v = v->next) {
        for (const char *t = v->string; t && *t; t++)
            if ((unsigned char)*t < 32)
                return 1;
        if (cJSON_IsString(v))
            for (const char *t = v->valuestring; *t; t++)
                if ((unsigned char)*t < 32 && !strchr("\n\t\r", *t))
                    return 1;
        if (v->child && holds_control(v->child))
            return 1;
    }
    return 0;
}

/* A description or a summary as a catalog writer on Windows leaves it:
   CR LF and a lone CR are a newline, a tab a space, and a summary, which
   has no lines, takes a space for a newline too. In place: the text only
   gets shorter. The .pspdx written out of the entry is held to v1 as
   before, and so is the schema. */
static void mend_text(cJSON *value, int newline) {
    if (!cJSON_IsString(value))
        return;
    char *w = value->valuestring;
    for (const char *r = value->valuestring; *r; r++) {
        char c = *r;
        if (c == '\r') {
            if (r[1] == '\n')
                r++;
            c = '\n';
        }
        if (c == '\t' || (c == '\n' && !newline))
            c = ' ';
        *w++ = c;
    }
    *w = '\0';
}

/* Entries point at their assets relative to the catalog, so that moving the
   whole thing to another host stays a one-line change. */
static void asset_url(const char *base, const char *rel, char *out, size_t size) {
    if (!rel || !rel[0]) {
        out[0] = '\0';
        return;
    }
    int n;
    const char *scheme_end = strstr(base, "://");
    if (strncasecmp(rel, "http://", 7) == 0 || strncasecmp(rel, "https://", 8) == 0) {
        n = snprintf(out, size, "%s", rel);
    } else if (!scheme_end) {
        n = -1;
    } else if (rel[0] == '/' && rel[1] == '/') {
        /* Another host, by the catalog's scheme. */
        n = snprintf(out, size, "%.*s%s", (int)(scheme_end - base) + 1, base, rel);
    } else if (rel[0] == '/') {
        /* The catalog's host, from its root. */
        const char *host = scheme_end + 3;
        n = snprintf(out, size, "%.*s%s", (int)(host - base + strcspn(host, "/?#")), base, rel);
    } else {
        /* Beside the catalog: its path up to the last slash, without a query. */
        const char *end = base + strcspn(base, "?#");
        const char *slash = end;
        while (slash > scheme_end + 3 && slash[-1] != '/')
            slash--;
        if (slash <= scheme_end + 3)
            n = snprintf(out, size, "%.*s/%s", (int)(end - base), base, rel);
        else
            n = snprintf(out, size, "%.*s%s", (int)(slash - base), base, rel);
    }
    /* An address cut to fit is another address: none at all instead. */
    if (n < 0 || (size_t)n >= size)
        out[0] = '\0';
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
    } else {
        entry->state = APP_NOT_INSTALLED;
    }
}

/* An id as PSPDX makes them: letters and digits in lower case, in parts
   joined by one dot, at least two parts, and no longer than the id buffers
   and the file names made of it hold. */
static int id_well_formed(const char *id) {
    size_t n = strlen(id), part = 0;
    int dots = 0;
    if (n >= PSPDX_ID_SIZE)
        return 0;
    for (const char *p = id; *p; p++) {
        if (*p == '.') {
            if (!part)
                return 0;
            dots++;
            part = 0;
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) {
            part++;
        } else {
            return 0;
        }
    }
    return dots && part;
}

/* The first source to name an id wins: an entry already there is left
   alone, whatever a later source says about it. */
static int has_id(const struct catalog *catalog, const char *id) {
    for (int i = 0; i < catalog->count; i++)
        if (strcmp(catalog->apps[i].id, id) == 0)
            return 1;
    return 0;
}

static int entry_pspdx(struct app_entry *entry);

/* Whether an object names one of the fields this version reads twice. cJSON
   finds the first and most other readers the last, so such an entry says
   two things, and is refused as a .pspdx that did so would be. A field this
   version does not know is passed over, as it is in a .pspdx. */
static int names_twice(const cJSON *object, const char *const *keys) {
    for (; cJSON_IsObject(object) && *keys; keys++) {
        int seen = 0;
        for (const cJSON *v = object->child; v; v = v->next)
            if (v->string && !strcmp(v->string, *keys) && seen++)
                return 1;
    }
    return 0;
}
static const char *const APP_KEYS[] = {"id", "source", "name", "type", "category", "tags",
                                       "installdir", "summary", "author", "license",
                                       "description", "website", "media",
                                       "releases", NULL};
static const char *const MEDIA_KEYS[] = {"icon", "screenshots", "video", "sound", NULL};
static const char *const RELEASE_KEYS[] = {"tag", "published_at", "url", "size", "sha256",
                                           "eboot_md5", "changelog", NULL};

/* The fields of an entry, held to the rules of the .pspdx an install writes
   out of them where the repository has none. The entry's own copies cannot
   say: a field too long for its buffer is left empty there, and a tag that
   breaks the rules is left out, where the file would be refused. NULL when
   they hold, or the field that does not. */
static const char *entry_fields(const cJSON *app) {
    static const struct {
        const char *key;
        int limit, newline;
    } text[] = {{"name", 40, 0},    {"author", 60, 0},   {"summary", 60, 0},
                {"license", 60, 0}, {"category", 24, 0}, {"description", 2500, 1}};
    for (unsigned i = 0; i < sizeof(text) / sizeof(*text); i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(app, text[i].key);
        int n = cJSON_IsString(v) ? pspdx_characters(v->valuestring, text[i].newline) : -1;
        if (v && (n < 0 || n > text[i].limit || (!n && !strcmp(text[i].key, "category"))))
            return text[i].key;
    }
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(app, "tags"), *tag;
    if (tags && (!cJSON_IsArray(tags) || cJSON_GetArraySize(tags) > PSPDX_TAGS))
        return "tags";
    cJSON_ArrayForEach(tag, tags) {
        int n = cJSON_IsString(tag) ? pspdx_characters(tag->valuestring, 0) : -1;
        if (n < 1 || n > 24)
            return "tags";
        for (const cJSON *w = tags->child; w != tag; w = w->next)
            if (!strcmp(w->valuestring, tag->valuestring))
                return "tags";
    }
    return NULL;
}

/* The entry that already has PSP/GAME/<dir>, but for the one at except, or
   -1. One directory is one app's, and what cannot be installed claims none. */
static int folder_holder(const struct catalog *catalog, const char *dir, int except) {
    for (int i = 0; dir[0] && i < catalog->count; i++)
        if (i != except && !catalog->apps[i].unsupported &&
            !strcasecmp(catalog->apps[i].release.dir, dir))
            return i;
    return -1;
}

/* The id of an app installed on the stick that has PSP/GAME/<dir>, when it
   is not the app an entry with this id and source describes; NULL when none
   is. An installed app keeps its folder whatever a list says, so an entry
   for any other app that names it is the one left out, wherever it stands. */
static const char *folder_installed(const char *dir, const char *id, const char *repo,
                                    struct installed *rec) {
    for (int i = 0; dir[0] && i < state_count(); i++) {
        const char *other = state_id(i);
        if (!other || db_read(other, rec) < 0 || strcasecmp(rec->dir, dir))
            continue;
        if (strcmp(rec->id, id) && !sources_same_repo(rec->repo, repo))
            return rec->id;
    }
    return NULL;
}

/* The id the stick keeps an app from this source under, or NULL: a catalog
   may have given it another id once, or none, and may give yet another
   tomorrow. The record is the app, so the row takes its id, and an install
   updates that record instead of making a second one of the same source. */
static const char *record_for_source(const char *repo, struct installed *rec) {
    for (int i = 0; repo[0] && i < state_count(); i++) {
        const char *id = state_id(i);
        if (id && db_read(id, rec) == 0 && sources_same_repo(rec->repo, repo))
            return rec->id;
    }
    return NULL;
}

/* The folder an app goes to on its next install, out of the one a .pspdx
   names. An app not on the stick goes there. An installed app stays in the
   folder it is in -- one the user renamed, or one a list names otherwise --
   unless the author's file now names another folder than the one it named
   when the app went in: then the folder moves, with everything in it. */
static void install_target(const char *id, const char *file_dir, char *out, size_t size) {
    struct installed rec;
    snprintf(out, size, "%.32s", file_dir);
    if (db_read(id, &rec) < 0 || !manifest_dir_is_safe(rec.dir))
        return;
    char named[64];
    snprintf(named, sizeof(named), "%s", rec.file_dir);
    if (!named[0]) {
        /* A record from before the folder named was kept: the saved file,
           which until now was the one the app was installed from. */
        struct pspdx_file saved;
        char *raw = NULL;
        if (state_read_manifest(id, &raw, &saved) >= 0 && saved.installdir[0])
            snprintf(named, sizeof(named), "%.32s", saved.installdir + 9);
        else
            snprintf(named, sizeof(named), "%s", rec.dir);
        free(raw);
    }
    if (!strcmp(named, file_dir))
        snprintf(out, size, "%s", rec.dir);
}

/* Who has the folder an entry wants: an installed app of another id first,
   then an entry listed before it. NULL when the folder is free. */
static const char *folder_claimed(const struct catalog *catalog, const struct app_entry *entry,
                                  struct installed *rec) {
    if (entry->unsupported)
        return NULL;
    const char *installed = folder_installed(entry->release.dir, entry->id, entry->repo, rec);
    if (installed)
        return installed;
    int held = folder_holder(catalog, entry->release.dir, -1);
    return held >= 0 ? catalog->apps[held].id : NULL;
}

void catalog_folder_line(char *out, size_t size, const char *name, const char *dir) {
    /* The name gives way, never the folder or the end of the sentence. */
    char shown[161];
    int fixed = snprintf(NULL, 0, T_FOLDER_TAKEN, "", dir);
    size_t room = fixed >= 0 && (size_t)fixed + 1 < size ? size - 1 - (size_t)fixed : 0;
    snprintf(shown, room + 1 < sizeof(shown) ? room + 1 : sizeof(shown), "%s", name);
    pspdx_utf8_mend(shown);
    snprintf(out, size, T_FOLDER_TAKEN, shown, dir);
}

/* An entry left out because another has its folder: said in the log, and the
   first such one kept for the status line once the fetch is through, since an
   app that is simply missing from the list says nothing. */
static void folder_taken(struct catalog *catalog, const char *from, const char *id,
                         const char *name, const char *dir, const char *holder) {
    logline("%s: %s wants PSP/GAME/%s, which %s has; not listed", from, id, dir, holder);
    if (!catalog->collision[0])
        catalog_folder_line(catalog->collision, sizeof(catalog->collision), name, dir);
}

/* A catalog.json in the response buffer, merged into the catalog. base is
   the URL it came from, for the assets it names relative to itself.
   Returns the entries taken, or -1 for something that is not a catalog. */
static int parse(struct catalog *catalog, const char *base) {
    /* A NUL inside a string would cut the text short where it goes on; it is
       made a control character first, and the entry that holds one goes. */
    pspdx_json_mark_nul(response, response_len);
    cJSON *root = cJSON_ParseWithLengthOpts(response, response_len + 1, NULL, 1);
    if (!root) {
        logline("catalog: not json");
        return -1;
    }
    /* A catalog is read as v1, the only version there is, unless it names
       another version of PSPDX's own catalog schema. Another tool may name
       nothing, its own copy of the schema by a relative path, or anything
       else: that is its business, and said in the log. */
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    static const char PSPDX_CATALOG[] = "https://chriopter.github.io/pspdx/schema/catalog-v";
    const char *named = cJSON_IsString(schema) ? schema->valuestring : NULL;
    if (named && !strncmp(named, PSPDX_CATALOG, sizeof(PSPDX_CATALOG) - 1)) {
        const char *digits = named + sizeof(PSPDX_CATALOG) - 1;
        size_t n = strspn(digits, "0123456789");
        if (n && !strcmp(digits + n, ".json") && strtoul(digits, NULL, 10) != 1) {
            logline("catalog: %.80s is another version of the catalog schema; not read", named);
            cJSON_Delete(root);
            return -1;
        }
    }
    if (schema && !(named && !strcmp(named, "https://chriopter.github.io/pspdx/schema/catalog-v1.json"))) {
        char *said = named ? NULL : cJSON_PrintUnformatted(schema);
        logline("catalog: schema %.80s is not PSPDX's v1 URL; read as v1", named ? named : said ? said : "?");
        free(said);
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
    unsigned when = pspdx_time(cJSON_IsString(generated) ? generated->valuestring : NULL);
    if (!when) {
        logline("catalog: invalid generated_at");
        cJSON_Delete(root);
        return -1;
    }
    int before = catalog->count;
    int taken = 0;
    cJSON *app;
    cJSON_ArrayForEach(app, apps) {
        if (catalog->count >= MAX_APPS)
            break;
        struct app_entry *entry = &catalog->apps[catalog->count];
        entry_clear(entry);
        cJSON *releases = cJSON_GetObjectItemCaseSensitive(app, "releases");
        if (names_twice(app, APP_KEYS) ||
            names_twice(cJSON_GetObjectItemCaseSensitive(app, "media"), MEDIA_KEYS) ||
            names_twice(cJSON_GetArrayItem(cJSON_IsArray(releases) ? releases : NULL, 0),
                        RELEASE_KEYS)) {
            logline("catalog: an entry names a field twice; dropped");
            continue;
        }
        if (holds_control(app->child)) {
            logline("catalog: an entry holds a NUL or a control character in its text; dropped");
            continue;
        }
        mend_text(cJSON_GetObjectItemCaseSensitive(app, "summary"), 0);
        mend_text(cJSON_GetObjectItemCaseSensitive(app, "description"), 1);
        /* What the catalog calls the entry, read whole: an id cut to fit the
           buffer could look like one it never was. */
        cJSON *named = cJSON_GetObjectItemCaseSensitive(app, "id");
        const char *given = cJSON_IsString(named) ? named->valuestring : NULL;
        copy_str(entry->name, sizeof(entry->name), cJSON_GetObjectItemCaseSensitive(app, "name"));
        copy_str(entry->author, sizeof(entry->author),
                 cJSON_GetObjectItemCaseSensitive(app, "author"));
        copy_str(entry->summary, sizeof(entry->summary),
                 cJSON_GetObjectItemCaseSensitive(app, "summary"));
        copy_tags(entry->tags, sizeof(entry->tags), cJSON_GetObjectItemCaseSensitive(app, "tags"));
        /* The category as a file holds it, 1 to 24 characters and no control
           character; one that is not is left out, as a tag would be. */
        cJSON *group = cJSON_GetObjectItemCaseSensitive(app, "category");
        int letters = cJSON_IsString(group) ? pspdx_characters(group->valuestring, 0) : -1;
        if (letters >= 1 && letters <= 24 && strlen(group->valuestring) < sizeof(entry->category))
            snprintf(entry->category, sizeof(entry->category), "%s", group->valuestring);
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
        /* The id names files on the stick, so PSPDX makes its own from the
           source, as it does for a .pspdx: the repository on GitHub, the
           source's host and the name elsewhere. Empty when neither leaves one
           to make. The catalog's own is taken instead only where it is an
           id already, no other repository's io.github. one, and names no app
           on the stick from another source,
           and where the stick does not keep this app under the one made
           here; anything else is what the list calls the entry, and a list
           may call its entries as it likes. */
        struct source_repo source;
        struct installed record;
        char derived[PSPDX_ID_SIZE] = "";
        int github = sources_parse_repo(entry->repo, &source);
        if (github)
            sources_repo_id(&source, derived, sizeof(derived));
        else if (!strncmp(entry->repo, "https://", 8))
            sources_host_id(entry->repo, entry->name, derived, sizeof(derived));
        int own = given && id_well_formed(given) &&
                  (strncmp(given, "io.github.", 10) || !strcmp(given, derived)) &&
                  !(db_read(given, &record) == 0 && !sources_same_repo(record.repo, entry->repo)) &&
                  !(derived[0] && strcmp(given, derived) && db_read(derived, &record) == 0 &&
                    sources_same_repo(record.repo, entry->repo));
        snprintf(entry->id, sizeof(entry->id), "%s", own ? given : derived);
        const char *kept = record_for_source(entry->repo, &record);
        if (kept && strcmp(kept, entry->id)) {
            logline("catalog: %s is kept on this stick as %s", entry->id, kept);
            snprintf(entry->id, sizeof(entry->id), "%s", kept);
        }

        /* The newest release is the first, and the only one a console
           installs or compares; the rest are history. */
        cJSON *release = cJSON_IsArray(releases) ? cJSON_GetArrayItem(releases, 0) : NULL;
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
            m->rev = pspdx_time(cJSON_IsString(published) ? published->valuestring : NULL);
            if (!m->rev) ok = 0;
            if (cJSON_IsNumber(size) && manifest_size_in_range(size->valuedouble))
                m->size = (size_t)size->valuedouble;
            else
                ok = 0;
            copy_str(m->url, sizeof(m->url), cJSON_GetObjectItemCaseSensitive(release, "url"));
            /* A tag is 1 to 64 characters and no control character; the
               version is kept in bytes enough for 64 of four bytes each. */
            int characters = cJSON_IsString(tag) ? pspdx_characters(tag->valuestring, 0) : -1;
            if (characters >= 1 && characters <= 64 && strlen(tag->valuestring) < sizeof(entry->tag))
                snprintf(entry->tag, sizeof(entry->tag), "%s", tag->valuestring);
            if (characters >= 1 && characters <= 64) {
                /* A "v" is taken off a version, and a tag that is only
                   one is the version. */
                const char *version =
                    tag->valuestring + (tag->valuestring[0] == 'v' && tag->valuestring[1]);
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
            if (sha && !cJSON_IsString(sha)) {
                ok = 0;
            } else if (sha) {
                /* 64 zeros is no hash, and no zip hashes to it. */
                ok = ok && strlen(sha->valuestring) == 64 && strspn(sha->valuestring, "0") != 64;
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
            if (!homebrew) {
                if (target)
                    entry->has_release = 0;
            } else if (target) {
                if (!cJSON_IsString(target) || !pspdx_install_dir(target->valuestring))
                    entry->has_release = 0;
                else
                    snprintf(m->dir, sizeof(m->dir), "%s", target->valuestring + 9);
            } else {
                pspdx_default_dir(github ? source.name : NULL, entry->name, folder, sizeof(folder));
                if (!pspdx_install_dir(folder))
                    entry->has_release = 0;
                else
                    snprintf(m->dir, sizeof(m->dir), "%s", folder + 9);
            }
            /* An installed app keeps its folder: a list that names another --
               a second catalog, listed first -- moves nothing. Only the
               author's own file can, in catalog_prepare. */
            if (homebrew && m->dir[0] && db_read(entry->id, &record) == 0 &&
                sources_same_repo(record.repo, entry->repo) && manifest_dir_is_safe(record.dir) &&
                strcmp(record.dir, m->dir)) {
                logline("catalog: %s is installed in PSP/GAME/%s, not the %s its entry names",
                        entry->id, record.dir, m->dir);
                snprintf(m->dir, sizeof(m->dir), "%.32s", record.dir);
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
        cJSON *shots = cJSON_GetObjectItemCaseSensitive(media, "screenshots");
        copy_str(shot, sizeof(shot), cJSON_IsArray(shots) ? cJSON_GetArrayItem(shots, 0) : NULL);
        asset_url(base, shot, entry->screenshot, sizeof(entry->screenshot));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(media, "video"));
        asset_url(base, shot, entry->video, sizeof(entry->video));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(media, "sound"));
        asset_url(base, shot, entry->sound, sizeof(entry->sound));

        /* A GitHub entry whose release is no good is asked about at its
           repository; away from GitHub the entry was the only word, so its
           going is said. */
        if (!github && entry->name[0] && known_type && !entry->has_release)
            logline("catalog: %s is from outside GitHub and its release cannot be installed "
                    "as it stands; dropped", entry->name);
        if (!entry->name[0] || !entry->has_release || !known_type)
            continue;
        /* Away from GitHub the entry is the app's whole word, and the install
           holds the download to the hash the entry gives. */
        if (!github && strncmp(entry->repo, "https://", 8))
            continue;
        if (!derived[0]) {
            /* Away from GitHub the id is the source's host and the name, so
               an entry that leaves either with nothing to make one of is no
               app. Said, since the list meant to carry it. */
            logline("catalog: %s is from outside GitHub and its source's host and name make "
                    "no id; dropped", entry->name);
            continue;
        }
        /* An id names a directory on the stick and a file in the cache: one
           that cannot be a path component is not an entry. */
        if (!manifest_id_is_safe(entry->id) ||
            (github && !sources_release_url(entry->repo, entry->release.url)))
            continue;
        entry->unsupported = !homebrew;
        if (has_id(catalog, entry->id))
            continue;
        /* An app is one row: a repository an earlier entry listed under
           another id is the same app again. */
        if (catalog_find_repo(catalog, entry->repo) >= 0) {
            logline("catalog: %s names %s, which is listed already; not listed twice", entry->id,
                    entry->repo);
            continue;
        }
        struct installed local;
        if (db_read(entry->id, &local) < 0 && catalog->count >= MAX_APPS - state_count())
            continue;
        copy_description(entry, cJSON_GetObjectItemCaseSensitive(app, "description"));
        /* Any entry is installed from the .pspdx written out of it wherever
           the repository has none, and that file is held to v1: an entry
           that could never make one is not listed as an app that installs. */
        {
            const char *bad = entry_fields(app);
            char why[80] = "";
            if (!bad && !entry->unsupported) {
                struct pspdx_file file;
                char *fixture = entry->release.raw;
                entry->release.raw = NULL;
                if (entry_pspdx(entry) < 0)
                    bad = "too large";
                else if (pspdx_parse(entry->release.raw, strlen(entry->release.raw), &file, why,
                                     sizeof(why)) < 0)
                    bad = why;
                manifest_forget(&entry->release);
                entry->release.raw = fixture;
            }
            if (bad) {
                logline("catalog: %s breaks the .pspdx rules (%s); dropped",
                        entry->id, bad);
                continue;
            }
        }
        /* One directory is one app's: a second entry that wants the same
           name would only be refused at install time. An app installed there
           keeps it; otherwise the first entry does, and the one left out is
           said. What cannot be installed claims no directory, and is not kept
           out by one either. */
        struct installed rec;
        const char *holder = folder_claimed(catalog, entry, &rec);
        if (holder) {
            folder_taken(catalog, "catalog", entry->id, entry->name, entry->release.dir, holder);
            continue;
        }

        if (given && !own) {
            /* Said once the entry is kept, and only as much of the name as
               is safe to put on a line of the log. */
            char shown[41];
            size_t k = 0;
            for (const char *p = given; *p && k + 1 < sizeof(shown); p++)
                shown[k++] = (unsigned char)*p < 32 || *p == 127 ? '?' : *p;
            shown[k] = '\0';
            logline("catalog: \"%s%s\" is not an id this stick can use; listed as %s", shown,
                    given[k] ? "..." : "", entry->id);
        }
        settle_state(entry);
        catalog->count++;
        taken++;
    }
    int empty = cJSON_GetArraySize(apps) == 0;
    g_parsed_empty = empty;
    int rc = taken || empty || before > 0 ? taken : -1;
    /* A catalog that was not taken says nothing about how old the list is. */
    if (rc >= 0) {
        catalog->total += cJSON_GetArraySize(apps);
        if (!catalog->generated || when < catalog->generated) {
            catalog->generated = when;
            /* The host alone: a line on the console has no room for a URL,
               and the host is what a person calls the list. */
            const char *host = strstr(base, "://");
            host = host ? host + 3 : base;
            size_t n = strcspn(host, "/");
            snprintf(catalog->generated_from, sizeof(catalog->generated_from), "%.*s", (int)n,
                     host);
        }
    }
    cJSON_Delete(root);
    return rc;
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
    unsigned start = now_ms();
    https_set_time_limit(TEXT_LIMIT_S);
    int rc = https_get(url, response_sink, NULL, NULL, NULL, &r);
    https_set_time_limit(0);
    https_set_accept_gzip(0);
    const char *bad = gunzip_end(&g_gunzip, r.content_encoding);
    if (out)
        *out = r;
    if (rc != 0 || r.status != 200 || bad) {
        if (rc != 0 && now_ms() - start >= TEXT_LIMIT_S * 1000u)
            logline("fetch: %s took more than %u s; given up", url, TEXT_LIMIT_S);
        else if (g_too_large)
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
    g_took_saved = 0;
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
            g_took_saved = taken >= 0;
            free(raw);
        }
    }
    parsing_cached = 0;
    if (taken >= 0)
        snprintf(g_catalog_url, sizeof(g_catalog_url), "%s", url);
    return taken;
}

/* ------------------------------------------------------------- origin */

static int ends_with_zip(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".zip") == 0;
}


/* One text file from GitHub's API into the response buffer, parsed.
   Returns the JSON, or NULL when it did not arrive or was not JSON. */
static cJSON *fetch_json(const char *url, struct https_result *out) {
    if (fetch_text(url, out) < 0)
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

/* One repository asked at the origin and made into an entry: its .pspdx
   at raw.githubusercontent.com first, since that file is the consent and a
   repository without one is not an app; then api.github.com for the
   release. The pictures are four more URLs under the media directory, not
   fetched here -- the card and the row ask for them when the cursor
   arrives, exactly as they do from a cache.

   given is the text of a .pspdx to go by instead of asking for the
   repository's: the one the stick keeps for an installed app whose
   repository has none, or one the user put in INBOX. make takes a repository
   that answers it has no .pspdx as an app all the same, made out of its own
   name: only for one the user typed, never for a list's line, since the
   file is the author's consent to being listed.

   Returns 1 with the entry filled, 0 when the repository answers that it
   has no .pspdx, -2 when GitHub's API did not answer -- a rate limit, an
   error -- and -1 refused otherwise; why is kept for the gear tab. */
static int origin_entry(struct app_entry *entry, const struct source_repo *repo, const char *given,
                        int make) {
    char url[SOURCE_URL], api[SOURCE_URL];
    sources_repo_url(repo, url, sizeof(url));
    if (!given) {
        for (int i = 0; i < attempted_count; i++)
            if (sources_same_url(attempted[i], url))
                return -1;
        if (attempted_count >= MAX_APPS + LIST_REPOS)
            return -1;
        snprintf(attempted[attempted_count++], SOURCE_URL, "%s", url);
    }
    entry_clear(entry);

    struct pspdx_file file;
    char reason[64], *text = NULL;
    int made = 0;
    if (given) {
        size_t n = strlen(given);
        if (n > PSPDX_FILE_MAX || !(text = malloc(n + 1)))
            goto refused;
        memcpy(text, given, n + 1);
    } else {
        snprintf(api, sizeof(api), "https://raw.githubusercontent.com/%s/%s/%s/.pspdx",
                 repo->owner, repo->name, repo->ref);
        struct https_result answer;
        memset(&answer, 0, sizeof(answer));
        if (fetch_text(api, &answer) < 0) {
            if (g_offline)
                return -1;
            /* Only GitHub's own 404 says the file is not there. One that
               ended at another host after a redirect, or no answer at all,
               says nothing about the repository. */
            if (answer.status != 404 || strcasecmp(answer.host, "raw.githubusercontent.com")) {
                logline("origin: %s/%s .pspdx did not come (status %ld from %s)", repo->owner,
                        repo->name, answer.status, answer.host[0] ? answer.host : "nowhere");
                refuse(url, REFUSED_NO_ANSWER);
                return -1;
            }
            logline("origin: %s/%s has no .pspdx at %s", repo->owner, repo->name, repo->ref);
            if (!make) {
                refuse(url, REFUSED_NO_PSPDX);
                return 0;
            }
            /* The repository's name and nothing else; the folder and the
               id follow from the source as for any file. */
            cJSON *o = cJSON_CreateObject();
            /* A name has 40 characters, and a repository's are ASCII. */
            char name[41];
            snprintf(name, sizeof(name), "%.40s", repo->name);
            if (o && cJSON_AddStringToObject(o, "schema", PSPDX_SCHEMA) &&
                cJSON_AddStringToObject(o, "source", url) && cJSON_AddStringToObject(o, "name", name))
                text = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            if (!text)
                goto refused;
            made = 1;
            logline("origin: %s/%s made an app of its own name", repo->owner, repo->name);
        } else {
            text = response_len <= PSPDX_FILE_MAX ? keep_response() : NULL;
        }
    }
    size_t len = text ? strlen(text) : 0;
    if (!text) {
        logline("origin: %s/%s .pspdx refused: too large", repo->owner, repo->name);
        goto refused;
    }
    if (pspdx_parse(text, len, &file, reason, sizeof(reason)) < 0) {
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
    memcpy(entry->category, file.category, sizeof(entry->category));
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
       release, and with neither it is whatever GitHub calls latest. A pinned
       one is asked for by its tag, which finds a prerelease too, and nothing
       newer is looked for. */
    const char *pinned = strcmp(repo->ref, "HEAD") != 0 ? repo->ref : NULL;
    int by_file = !pinned && file.release_tag[0];
    if (by_file)
        pinned = file.release_tag;
    /* Room for the longest tag with every byte of it escaped. */
    char release_api[1024];
    if (pinned) {
        char escaped[64 * 4 * 3 + 1];
        size_t k = 0;
        for (const unsigned char *p = (const unsigned char *)pinned; *p; p++) {
            int plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                        (*p >= '0' && *p <= '9') || strchr("-._~", *p);
            k += (size_t)snprintf(escaped + k, sizeof(escaped) - k, plain ? "%c" : "%%%02X", *p);
        }
        snprintf(release_api, sizeof(release_api),
                 "https://api.github.com/repos/%s/%s/releases/tags/%s", repo->owner, repo->name,
                 escaped);
    } else {
        snprintf(release_api, sizeof(release_api),
                 "https://api.github.com/repos/%s/%s/releases/latest", repo->owner, repo->name);
    }
    struct https_result asked;
    memset(&asked, 0, sizeof(asked));
    cJSON *root = fetch_json(release_api, &asked);
    if (!root) {
        /* A 404 is GitHub saying there is no such release. Anything else --
           403 or 429 for the sixty requests an hour, a 5xx, no answer -- is
           GitHub not answering, and says nothing about the app. */
        int none = asked.status == 404;
        if (none)
            logline("origin: %s/%s has no release %s", repo->owner, repo->name,
                    pinned ? pinned : "(latest)");
        else
            logline("origin: %s/%s: GitHub did not answer for release %s (status %ld: rate limit "
                    "or error)", repo->owner, repo->name, pinned ? pinned : "(latest)", asked.status);
        free(text);
        entry_clear(entry);
        refuse(url, none ? REFUSED_RELEASE : REFUSED_NO_ANSWER);
        return none ? -1 : -2;
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
        snprintf(m->version, sizeof(m->version), "%s", tag[0] == 'v' && tag[1] ? tag + 1 : tag);
    cJSON *published = cJSON_GetObjectItemCaseSensitive(root, "published_at");
    m->rev = pspdx_time(cJSON_IsString(published) ? published->valuestring : NULL);
    /* The zip: the asset the file names, when it pins a release and names
       one, which has to be on that release; otherwise the only .zip on it,
       or of several the only one whose name says psp. */
    const char *wanted = by_file && file.release_url[0] ? file.release_url : NULL;
    int zips = 0, psp = 0, named = 0, not_zip = 0;
    cJSON *asset, *zip = NULL, *psp_zip = NULL;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(root, "assets")) {
        cJSON *aname = cJSON_GetObjectItemCaseSensitive(asset, "name");
        cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
        cJSON *link = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
        if (!cJSON_IsString(aname) || !cJSON_IsNumber(size))
            continue;
        if (wanted) {
            /* The asset the file names is the package, and a package is a
               .zip: another file is refused before anything is fetched. */
            if (cJSON_IsString(link) && !strcmp(link->valuestring, wanted)) {
                if (ends_with_zip(aname->valuestring)) {
                    named++;
                    zip = asset;
                } else {
                    not_zip++;
                }
            }
            continue;
        }
        if (!ends_with_zip(aname->valuestring))
            continue;
        zips++;
        zip = asset;
        for (const char *p = aname->valuestring; *p; p++)
            if (!strncasecmp(p, "psp", 3)) {
                psp++;
                psp_zip = asset;
                break;
            }
    }
    cJSON *chosen = wanted ? (named == 1 ? zip : NULL) : zips == 1 ? zip : psp == 1 ? psp_zip : NULL;
    const char *unusable = wanted ? not_zip ? "the release.url of its .pspdx names no .zip"
                                            : "the release.url of its .pspdx is not an asset of the release"
                           : !zips ? "no .zip on the release"
                                   : "several .zip files and not exactly one with psp in its name";
    if (chosen) {
        asset = chosen;
        cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
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
    if (!chosen || !m->rev || !m->version[0] || !sources_release_url(m->repo, m->url) ||
        !m->size) {
        logline("origin: %s/%s %s: %s, rev %u%s", repo->owner, repo->name,
                tag[0] ? tag : "no tag", chosen ? "a zip it cannot install" : unusable, m->rev,
                m->version[0] || !tag[0] ? "" : ", a tag of more than 64 characters");
        free(text);
        entry_clear(entry);
        refuse(url, REFUSED_RELEASE);
        return -1;
    }
    entry->has_release = 1;
    entry->no_pspdx = made ? PSPDX_FROM_REPOSITORY : 0;
    snprintf(entry->tag, sizeof(entry->tag), "%s", tag);
    m->pinned = by_file;
    /* The shape of the zip, as the author stated it; install.c holds both
       to their rules, the same ones a cache entry's are held to. */
    snprintf(m->dir, sizeof(m->dir), "%.32s", file.installdir + 9);
    /* The file goes with the entry. */
    m->raw = text;
    entry->description = pspdx_description(text, len);
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
static int take_origin(struct catalog *catalog, const struct source_repo *repo, int make) {
    char id[PSPDX_ID_SIZE], url[SOURCE_URL];
    struct installed local;
    sources_repo_id(repo, id, sizeof(id));
    sources_repo_url(repo, url, sizeof(url));
    /* Kept on the stick under another id, a catalog's, the app is that one. */
    const char *kept = record_for_source(url, &local);
    if (kept)
        snprintf(id, sizeof(id), "%s", kept);
    if (catalog->count >= MAX_APPS)
        return -1;
    if (has_id(catalog, id))
        return 0;
    if (db_read(id, &local) < 0 && catalog->count >= MAX_APPS - state_count())
        return -1;
    struct app_entry *entry = &catalog->apps[catalog->count];
    /* A repository with no .pspdx is not an app a list can name. */
    if (origin_entry(entry, repo, NULL, make) <= 0)
        return -1;
    snprintf(entry->id, sizeof(entry->id), "%s", id);
    snprintf(entry->release.id, sizeof(entry->release.id), "%s", id);
    char folder[64];
    install_target(id, entry->release.dir, folder, sizeof(folder));
    snprintf(entry->release.dir, sizeof(entry->release.dir), "%s", folder);
    /* The folder rule holds here as it does between a catalog's entries. */
    const char *holder = folder_claimed(catalog, entry, &local);
    if (holder) {
        char url[SOURCE_URL];
        sources_repo_url(repo, url, sizeof(url));
        folder_taken(catalog, "origin", entry->id, entry->name, entry->release.dir, holder);
        refuse(url, REFUSED_FOLDER);
        snprintf(g_refused_folder, sizeof(g_refused_folder), "%s", entry->release.dir);
        entry_clear(entry);
        return -1;
    }
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
                     int *refused, int make) {
    int taken = 0;
    *asked = *refused = 0;
    for (int i = 0; i < list->count; i++) {
        char url[SOURCE_URL];
        sources_repo_url(&list->repo[i], url, sizeof(url));
        if (catalog_find_repo(catalog, url) >= 0)
            continue;
        snprintf(g_progress, sizeof(g_progress), T_STATUS_ORIGIN, list->repo[i].name, i + 1,
                 list->count);
        (*asked)++;
        int rc = take_origin(catalog, &list->repo[i], make);
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
    g_took_saved = 0;

    /* A .pspdx was once a source of its own, a list's file standing in for
       a repository without one. A catalog lists such an app from its entry
       now, so a line of the kind left in sources.txt is passed over, and
       said. */
    if (sources_is_pspdx(url)) {
        logline("source %d: %s is a .pspdx, which is no source any more; skipped", at, url);
        return 0;
    }

    switch (sources_kind(url)) {
    case SOURCE_CATALOG:
        taken = take_cache(catalog, url, CACHE_LIVE_OR_SAVED);
        if (taken < 0 && g_too_large)
            g_source_too_large = 1;
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
        /* Read before catalog.txt is asked for: that fetch starts over. */
        int too_large = g_too_large, listed = 0;
        if (fetch_text(txt, NULL) == 0) {
            sources_parse_list(response, &list);
            if (list.count > 0) {
                listed = 1;
                taken = walk_list(catalog, &list, &asked, &refused, 0);
                /* A list whose every repository a source before it listed
                   already answered as surely as one that added apps. */
                if (taken > 0 || asked == 0) {
                    snprintf(g_catalog_url, sizeof(g_catalog_url), "%s", txt);
                    logline("source %d: catalog.txt, %d repositories asked", at, asked);
                    return taken;
                }
            }
        }
        taken = take_cache(catalog, json, CACHE_SAVED_ONLY);
        if (taken < 0 && too_large)
            g_source_too_large = 1;
        logline("source %d: live catalog %s, saved snapshot %s", at,
                too_large ? "too large" : "unavailable", taken >= 0 ? "used" : "missing");
        /* The list answered, and GitHub had nothing usable for any of its
           repositories: the source is there, its apps are not. */
        if (taken < 0 && listed) {
            logline("source %d: catalog.txt answered, %d of %d repositories refused", at, refused,
                    asked);
            return 0;
        }
        return taken;
    }

    case SOURCE_REPO:
        memset(&list, 0, sizeof(list));
        sources_parse_repo(url, &list.repo[0]);
        list.count = 1;
        /* A repository the user typed is an app even without a .pspdx. */
        taken = walk_list(catalog, &list, &asked, &refused, 1);
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
    /* The list itself answered live, whatever its cache did. */
    g_took_saved = 0;
    taken = walk_list(catalog, &list, &asked, &refused, 0);
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
const char *catalog_refused_folder(void) { return g_refused_folder; }

int catalog_find_repo(const struct catalog *catalog, const char *url) {
    for (int i = 0; i < catalog->count; i++)
        if (sources_same_repo(catalog->apps[i].repo, url))
            return i;
    return -1;
}

int catalog_add_repo(struct catalog *catalog, const char *url, int make) {
    struct source_repo repo;
    attempted_count = 0;
    if (!sources_parse_repo(url, &repo))
        return -1;
    char canonical[SOURCE_URL];
    sources_repo_url(&repo, canonical, sizeof(canonical));
    int at = catalog_find_repo(catalog, canonical);
    if (at >= 0)
        return at;
    if (take_origin(catalog, &repo, make) <= 0)
        return -1;
    return catalog->count - 1;
}

int catalog_add_file(struct catalog *catalog, const char *raw) {
    struct pspdx_file file;
    struct source_repo repo;
    struct installed local;
    char why[80], url[SOURCE_URL], id[PSPDX_ID_SIZE];
    if (pspdx_parse(raw, strlen(raw), &file, why, sizeof(why)) < 0 ||
        !sources_parse_repo(file.source, &repo))
        return -1;
    sources_repo_url(&repo, url, sizeof(url));
    const char *kept = record_for_source(url, &local);
    snprintf(id, sizeof(id), "%s", kept ? kept : file.id);
    if (catalog->count >= MAX_APPS || has_id(catalog, id) || catalog_find_repo(catalog, url) >= 0)
        return -1;
    struct app_entry *entry = &catalog->apps[catalog->count];
    if (origin_entry(entry, &repo, raw, 0) <= 0)
        return -1;
    entry->no_pspdx = PSPDX_FROM_FILE;
    snprintf(entry->id, sizeof(entry->id), "%s", id);
    snprintf(entry->release.id, sizeof(entry->release.id), "%s", id);
    char folder[64];
    install_target(id, entry->release.dir, folder, sizeof(folder));
    snprintf(entry->release.dir, sizeof(entry->release.dir), "%s", folder);
    const char *holder = folder_claimed(catalog, entry, &local);
    if (holder) {
        folder_taken(catalog, "INBOX", entry->id, entry->name, entry->release.dir, holder);
        refuse(url, REFUSED_FOLDER);
        snprintf(g_refused_folder, sizeof(g_refused_folder), "%s", entry->release.dir);
        entry_clear(entry);
        return -1;
    }
    settle_state(entry);
    catalog->count++;
    catalog->total++;
    return catalog->count - 1;
}

int catalog_ask_pinned(struct app_entry *entry, const char *raw) {
    struct source_repo repo;
    struct pspdx_file file;
    char why[80];
    /* Outside GitHub there is no one to ask: the file's url and date are
       all there is, as they are without this. */
    if (entry->tag[0] || !sources_parse_repo(entry->repo, &repo) ||
        pspdx_parse(raw, strlen(raw), &file, why, sizeof(why)) < 0 || !file.release_tag[0])
        return -1;
    struct app_entry *one = calloc(1, sizeof(*one));
    if (!one)
        return -1;
    /* The file is given, so its .pspdx is not asked for: one request, the
       release by its tag. */
    int rc = origin_entry(one, &repo, raw, 0) > 0 ? 0 : -1;
    if (rc == 0) {
        struct manifest *m = &entry->release;
        char dir[sizeof(m->dir)];
        snprintf(dir, sizeof(dir), "%s", m->dir);
        manifest_forget(m);
        *m = one->release;
        memset(&one->release, 0, sizeof(one->release));
        snprintf(m->id, sizeof(m->id), "%s", entry->id);
        snprintf(m->dir, sizeof(m->dir), "%s", dir);
        snprintf(entry->tag, sizeof(entry->tag), "%s", one->tag);
        entry->has_release = 1;
        settle_state(entry);
        logline("INBOX: %s: GitHub has release %s for the file", entry->id, entry->tag);
    }
    entry_clear(one);
    free(one);
    return rc;
}

/* How long an answer from an app's own repository stands before it is
   asked again, unless the check is forced. */
#define DIRECT_EVERY_S (6u * 3600u)

static void restore_installed(struct catalog *catalog) {
    unsigned now = (unsigned)time(NULL);
    int stale = catalog->generated && catalog->generated + 24u * 3600u < now;
    for (int i = 0; i < state_count(); i++) {
        char id[PSPDX_ID_SIZE];
        snprintf(id, sizeof(id), "%s", state_id(i));
        struct installed rec;
        if (db_read(id, &rec) < 0)
            continue;
        int at = catalog_find_repo(catalog, rec.repo);
        /* An app is one row: an id a catalog lists for another source is not
           listed a second time beside it. */
        if (at < 0 && has_id(catalog, id)) {
            logline("restore: %s from %s has its id listed for another source; not listed twice",
                    id, rec.repo);
            continue;
        }
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
        /* An answer stamped after now came from a clock that was wrong then
           or is wrong now; either way it says nothing about six hours. */
        int due = g_force || state_latest(id, &seen) < 0 || !seen.checked_at ||
                  seen.checked_at > now || !sources_same_repo(seen.checked_from, seen.repo) ||
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
        int held;
        int answer = one ? origin_entry(one, &repo, NULL, 0) : -1;
        int listed_fresh = at >= 0 && catalog->apps[at].fresh && !stale, by_saved = 0;
        if (answer == 0 && listed_fresh) {
            /* A repository that has no .pspdx is not wrong: the app came from
               a catalog entry, or from the user's own file, and a catalog
               that lists it and answered is its word. */
            logline("restore: %s has no .pspdx at %s; the catalog entry stands", id, source);
            state_note_latest(&catalog->apps[at].release);
        } else if (answer == 0 && n >= 0) {
            /* Without one, GitHub is asked for the release by the file the
               stick keeps, as for an app with a .pspdx of its own: the same
               ZIP rule, the same digest, and the saved file still the one
               that points at the source. */
            logline("restore: %s has no .pspdx at %s; its release is asked by the saved file", id,
                    source);
            answer = origin_entry(one, &repo, raw, 0);
            by_saved = 1;
        }
        /* An answer that settles nothing still counts as asked -- no .pspdx
           and nothing to ask by, no release the saved file could use, GitHub
           not answering -- so the repository is not asked again before the
           six hours are up, and the record's word stands meanwhile. */
        if (one && (answer == -2 || (answer == 0 && !listed_fresh) || (by_saved && answer < 0))) {
            if (answer == -2)
                logline("restore: %s: GitHub did not answer; the record stands for six hours", id);
            if (state_latest(id, &seen) == 0) {
                sources_repo_url(&repo, seen.checked_from, sizeof(seen.checked_from));
                seen.checked_at = now;
                state_note_latest(&seen);
            }
        }
        if (answer > 0) {
            fetched = 1;
            int known = at >= 0;
            /* The newest file the repository answered with is the one the
               stick keeps, so a check that does not ask again says what a
               forced one said. The folder it named when the app went in is
               written into an older record first, or a moved folder could no
               longer be told from a renamed one. */
            if (one->release.raw && (n < 0 || strcmp(raw, one->release.raw))) {
                char path[256];
                if (n >= 0 && file.installdir[0])
                    state_note_file_dir(id, file.installdir + 9);
                storage_app_path(id, path, sizeof(path));
                storage_write(path, one->release.raw, strlen(one->release.raw));
            }
            char folder[64];
            install_target(id, one->release.dir, folder, sizeof(folder));
            snprintf(one->release.dir, sizeof(one->release.dir), "%s", folder);
            /* Another entry has the folder the repository names now: the
               row a catalog gave the app stays as it was, and none is added. */
            held = one->unsupported ? -1 : folder_holder(catalog, one->release.dir, at);
            if (held >= 0)
                folder_taken(catalog, "restore", id, one->name, one->release.dir,
                            catalog->apps[held].id);
            else if (at < 0 && catalog->count < MAX_APPS)
                at = catalog->count++;
            /* A release the file pins says nothing about updates: where an
               entry lists the app already, it does, and stays, and is what
               the record hears. */
            if (at >= 0 && held < 0 && known && one->release.pinned)
                state_note_latest(&catalog->apps[at].release);
            if (at >= 0 && held < 0 && !(known && one->release.pinned)) {
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
                /* The record's id, which a catalog may have given it rather
                   than the repository. */
                snprintf(dst->id, sizeof(dst->id), "%s", id);
                snprintf(dst->release.id, sizeof(dst->release.id), "%s", id);
                settle_state(dst);
                state_note_latest(&dst->release);
            }
        } else if (at < 0 && n >= 0 && pspdx_type_installable(file.type) &&
                   (held = folder_holder(catalog, file.installdir + 9, -1)) >= 0) {
            folder_taken(catalog, "restore", id, file.name, file.installdir + 9,
                         catalog->apps[held].id);
        } else if (at < 0 && n >= 0 && catalog->count < MAX_APPS) {
            struct app_entry *e = &catalog->apps[catalog->count++];
            entry_clear(e);
            snprintf(e->id, sizeof(e->id), "%s", id);
            snprintf(e->repo, sizeof(e->repo), "%s", source);
            snprintf(e->name, sizeof(e->name), "%s", file.name);
            memcpy(e->tags, file.tags, sizeof(e->tags));
            memcpy(e->category, file.category, sizeof(e->category));
            snprintf(e->type, sizeof(e->type), "%s", file.type);
            e->unsupported = !pspdx_type_installable(file.type);
            snprintf(e->author, sizeof(e->author), "%s", file.author);
            snprintf(e->summary, sizeof(e->summary), "%s", file.summary);
            snprintf(e->license, sizeof(e->license), "%s", file.license);
            e->media_cached_only = 1;
            e->has_release = state_latest(id, &e->release) == 0;
            install_target(id, file.installdir + 9, e->release.dir, sizeof(e->release.dir));
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
                if (n >= 0)
                    install_target(id, file.installdir + 9, latest.dir, sizeof(latest.dir));
                else
                    snprintf(latest.dir, sizeof(latest.dir), "%s", rec.dir);
                /* Taking the record's word moves the row to the folder the
                   saved file names, which another entry may have: then the
                   row stays as the catalog gave it. */
                int moved = e->unsupported ? -1 : folder_holder(catalog, latest.dir, at);
                if (moved >= 0) {
                    folder_taken(catalog, "restore", id, e->name, latest.dir,
                                 catalog->apps[moved].id);
                    goto next;
                }
                manifest_forget(&e->release);
                e->release = latest;
                if (n >= 0) {
                    e->release.raw = raw;
                    raw = NULL;
                }
                e->has_release = 1;
            }
        }
    next:
        if (one)
            entry_clear(one);
        free(one);
        free(raw);
    }
}
int catalog_too_large(void) { return g_source_too_large; }
int catalog_fetch(struct catalog *catalog) {
    struct sources sources;
    catalog_free(catalog);
    memset(catalog, 0, sizeof(*catalog));
    attempted_count = 0;
    g_too_large = 0;
    g_source_too_large = 0;
    g_progress[0] = 0;
    g_refused_url[0] = 0;
    g_refused_folder[0] = 0;
    sources_load(&sources);
    reach_reset();
    int answered = 0;
    for (int i = 0; i < sources.count; i++) {
        int taken = fetch_source(catalog, i + 1, sources.url[i]);
        if (taken >= 0) {
            answered++;
            if (g_took_saved)
                reach_saved(sources.url[i]);
            reach_loaded(sources.url[i], taken, g_took_saved ? 0 : (unsigned)time(NULL));
        } else {
            reach_failed(sources.url[i]);
        }
    }
    restore_installed(catalog);
    g_progress[0] = 0;
    g_force = 0;
    return answered || catalog->count ? catalog->count : -1;
}
/* A catalog entry written as the .pspdx its repository has none of: the
   entry's own words. It is what the install checks and saves beside the
   record, so the app is on the stick the way one with a file of its own is.
   Held to v1 where it is read, like any file. 0 with the text kept in the
   release. */
static int entry_pspdx(struct app_entry *entry) {
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return -1;
    char folder[42];
    snprintf(folder, sizeof(folder), "PSP/GAME/%.32s", entry->release.dir);
    cJSON_AddStringToObject(o, "schema", PSPDX_SCHEMA);
    cJSON_AddStringToObject(o, "source", entry->repo);
    cJSON_AddStringToObject(o, "name", entry->name);
    if (entry->tags[0]) {
        cJSON *tags = cJSON_AddArrayToObject(o, "tags");
        char word[PSPDX_TAGS_TEXT];
        for (const char *p = entry->tags; tags && *p;) {
            size_t n = strcspn(p, "\n");
            snprintf(word, sizeof(word), "%.*s", (int)n, p);
            cJSON_AddItemToArray(tags, cJSON_CreateString(word));
            p += n + (p[n] == '\n');
        }
    }
    if (entry->category[0])
        cJSON_AddStringToObject(o, "category", entry->category);
    cJSON_AddStringToObject(o, "installdir", folder);
    const char *said[][2] = {{"author", entry->author}, {"summary", entry->summary},
                             {"license", entry->license},
                             {"description", entry->description ? entry->description : ""}};
    for (unsigned i = 0; i < sizeof(said) / sizeof(*said); i++)
        if (said[i][1][0])
            cJSON_AddStringToObject(o, said[i][0], said[i][1]);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    int rc = text && strlen(text) <= PSPDX_FILE_MAX &&
                     manifest_keep_raw(&entry->release, text, strlen(text)) == 0
                 ? 0
                 : -1;
    free(text);
    return rc;
}

int catalog_prepare(struct app_entry *entry) {
    struct source_repo repo;
    struct pspdx_file file;
    char why[80] = "", url[512];
    /* What is listed and not installable says so here, before anything is
       fetched for it. */
    if (entry->unsupported) {
        logline("install: %s cannot be installed yet (type %s)", entry->id, entry->type);
        return -1;
    }
    int github = sources_parse_repo(entry->repo, &repo);
    if (entry->from_inbox && github) {
        /* A file from INBOX is installed as the user wrote it only where the
           repository has no .pspdx of its own; one it has wins, as it wins
           over a catalog's entry. */
        struct https_result answer;
        memset(&answer, 0, sizeof(answer));
        snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/HEAD/.pspdx", repo.owner,
                 repo.name);
        if (fetch_text(url, &answer) == 0) {
            if (response_len > PSPDX_FILE_MAX ||
                manifest_keep_raw(&entry->release, response, response_len) < 0)
                return -1;
            logline("INBOX: %s has a .pspdx of its own, which is installed instead of the file",
                    entry->id);
        } else if (answer.status == 404 && !strcasecmp(answer.host, "raw.githubusercontent.com")) {
            entry->no_pspdx = PSPDX_FROM_FILE;
            logline("INBOX: %s: the repository has no .pspdx; installed from your file", entry->id);
        } else {
            logline("install: the .pspdx of %s did not come (status %ld from %s)", entry->id,
                    answer.status, answer.host[0] ? answer.host : "nowhere");
            return -1;
        }
        entry->from_inbox = 0;
    }
    if (!entry->release.raw) {
        /* The repository's own file first, asked for once here, and it
           always wins. Only when the repository answers that it has none
           does the catalog entry's word stand in. Outside GitHub there is no
           file to ask for: the entry is all there is. */
        int missing = !github;
        if (github) {
            struct https_result answer;
            memset(&answer, 0, sizeof(answer));
            snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/HEAD/.pspdx",
                     repo.owner, repo.name);
            if (fetch_text(url, &answer) == 0) {
                if (response_len > PSPDX_FILE_MAX ||
                    manifest_keep_raw(&entry->release, response, response_len) < 0)
                    return -1;
            } else if (answer.status == 404 &&
                       !strcasecmp(answer.host, "raw.githubusercontent.com")) {
                missing = 1;
            } else {
                logline("install: the .pspdx of %s did not come (status %ld from %s)", entry->id,
                        answer.status, answer.host[0] ? answer.host : "nowhere");
                return -1;
            }
        }
        if (missing) {
            if (entry_pspdx(entry) < 0)
                return -1;
            if (github)
                logline("install: %s has no .pspdx; installed from the catalog entry", entry->id);
        }
    }
    if (pspdx_parse(entry->release.raw, strlen(entry->release.raw), &file, why, sizeof(why)) < 0 ||
        !sources_same_repo(file.source, entry->repo) || !file.installdir[0]) {
        logline("install: manifest and selected catalog entry disagree%s%s; refresh sources",
                why[0] ? ": " : "", why);
        manifest_forget(&entry->release);
        return -1;
    }
    /* The file wins over the entry, its folder included, and an installed
       app keeps the folder it is in unless the author moved it. */
    char folder[64];
    install_target(entry->id, file.installdir + 9, folder, sizeof(folder));
    if (strcmp(folder, entry->release.dir))
        logline("install: %s goes to PSP/GAME/%s, the entry named %s", entry->id, folder,
                entry->release.dir);
    snprintf(entry->release.dir, sizeof(entry->release.dir), "%s", folder);
    return 0;
}
int catalog_validate_source(const char *url, int repository) {
    int rc = -1;
    attempted_count = 0;
    enum source_kind kind = sources_kind(url);
    /* No source any more; see fetch_source. */
    if (sources_is_pspdx(url))
        return -1;
    /* A repository is one app: asked into one entry. */
    if (repository) {
        struct app_entry *one = calloc(1, sizeof(*one));
        struct source_repo repo;
        if (!one)
            return -1;
        if (sources_parse_repo(url, &repo))
            rc = origin_entry(one, &repo, NULL, 1) > 0 ? 0 : -1;
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
        /* A release the app's file pins is the one there is, as far as its
           repository goes; only a catalog's entry says otherwise, and such
           an entry is never marked pinned. */
        if (manifest->pinned) {
            entry->state = APP_CURRENT;
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

int catalog_new_build(const struct app_entry *entry) {
    return entry->state == APP_UPDATE && entry->local_version[0] &&
           !strcmp(entry->local_version, entry->remote_version);
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
