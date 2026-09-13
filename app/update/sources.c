#include "util/storage.h"
#include <ctype.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "update/sources.h"
#include "util/runtime.h"

#define SOURCES_DIR storage_path("PSP/PSPDX")
#define SOURCES_PATH storage_path("PSP/PSPDX/sources.txt")

static char g_text[8 * 1024];

static int read_file(void) {
    char *raw = NULL;
    int n = storage_read(SOURCES_PATH, &raw, sizeof(g_text) - 1);
    if (n < 0)
        return -1;
    memcpy(g_text, raw, n + 1);
    free(raw);
    return n;
}
static int write_default(void) {
    if (storage_exists(SOURCES_PATH))
        return -1;
    static const char text[] =
        "# One HTTPS catalog, list or repository URL per line\n" SOURCES_DEFAULT "\n";
    return storage_write(SOURCES_PATH, text, sizeof(text) - 1);
}

/* One line, its ends trimmed and a comment cut off. Returns the length. */
static int trim(char *line) {
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';
    size_t n = strlen(line);
    while (n && isspace((unsigned char)line[n - 1]))
        line[--n] = '\0';
    size_t lead = 0;
    while (line[lead] && isspace((unsigned char)line[lead]))
        lead++;
    if (lead)
        memmove(line, line + lead, n - lead + 1);
    return (int)(n - lead);
}

/* Two URLs that differ only by case or a trailing slash name the same
   place, and one of them in the file is enough. */
int sources_same_url(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    while (na && a[na - 1] == '/')
        na--;
    while (nb && b[nb - 1] == '/')
        nb--;
    struct source_repo ra, rb;
    if (sources_parse_repo(a, &ra) && sources_parse_repo(b, &rb)) {
        return !strcasecmp(ra.owner, rb.owner) && !strcasecmp(ra.name, rb.name) &&
               !strcmp(ra.ref, rb.ref);
    }
    if (na != nb)
        return 0;
    if (na < 8)
        return !strncmp(a, b, na);
    const char *hostend = strchr(a + 8, '/');
    size_t hostlen = hostend ? (size_t)(hostend - a) : na;
    for (size_t i = 0; i < na; i++) {
        if (i < hostlen) {
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
                return 0;
        } else if (a[i] != b[i])
            return 0;
    }
    return 1;
}

int sources_load(struct sources *s) {
    memset(s, 0, sizeof(*s));
    if (read_file() < 0) {
        if (write_default() < 0 || read_file() < 0) {
            /* No file and no way to make one: the built-in list still
               stands, so the browser is not empty for want of a stick. */
            snprintf(s->url[0], SOURCE_URL, "%s", SOURCES_DEFAULT);
            s->count = 1;
            return 1;
        }
        logline("sources: wrote the built-in list");
    }
    char *line = g_text;
    while (line && *line && s->count < SOURCES_MAX) {
        char *end = strpbrk(line, "\r\n");
        if (end)
            *end++ = '\0';
        if (trim(line) > 0 && strncmp(line, "https://", 8) == 0) {
            int dup = 0;
            for (int i = 0; i < s->count; i++)
                dup = dup || sources_same_url(s->url[i], line);
            /* A line longer than a URL slot is not a URL anyone typed. */
            if (!dup && strlen(line) < SOURCE_URL) {
                memcpy(s->url[s->count], line, strlen(line) + 1);
                s->count++;
            }
        }
        line = end;
        while (line && (*line == '\r' || *line == '\n'))
            line++;
    }
    return s->count;
}

int sources_normalize(const char *text, char *url, size_t size) {
    char line[SOURCE_URL];
    if (!text || strlen(text) >= SOURCE_URL - 24)
        return -1;
    snprintf(line, sizeof(line), "%s", text ? text : "");
    if (trim(line) <= 0 || strchr(line, ' '))
        return -1;
    if (strncmp(line, "https://", 8) == 0)
        snprintf(url, size, "%s", line);
    else if (strncmp(line, "http://", 7) == 0)
        snprintf(url, size, "https://%s", line + 7);
    else if (strncmp(line, "github.com/", 11) == 0)
        snprintf(url, size, "https://%s", line);
    else {
        /* "owner/repo" and nothing else: one slash, both halves there. */
        const char *slash = strchr(line, '/');
        if (!slash || slash == line || !slash[1] || strchr(slash + 1, '/'))
            return -1;
        snprintf(url, size, "https://github.com/%s", line);
    }
    struct source_repo repo;
    if (strpbrk(url, "\r\n\t #") || strlen(url) >= size - 1)
        return -1;
    if (!strncmp(url, "https://github.com/", 19) && !sources_parse_repo(url, &repo))
        return -1;
    return 0;
}
int sources_add(const char *text, char *url, size_t size) {
    if (sources_normalize(text, url, size) < 0)
        return -1;
    struct sources have;
    sources_load(&have);
    for (int i = 0; i < have.count; i++)
        if (sources_same_url(have.url[i], url))
            return 0;
    if (have.count >= SOURCES_MAX) {
        logline("sources: the file is full");
        return -1;
    }

    char all[SOURCES_MAX * (SOURCE_URL + 1) + 1];
    size_t at = 0;
    for (int i = 0; i < have.count; i++)
        at += snprintf(all + at, sizeof(all) - at, "%s\n", have.url[i]);
    at += snprintf(all + at, sizeof(all) - at, "%s\n", url);
    if (storage_write(SOURCES_PATH, all, at) < 0)
        return -1;
    logline("sources: added %s", url);
    return 1;
}

int sources_remove(const char *url) {
    struct sources have;
    sources_load(&have);
    char all[SOURCES_MAX * (SOURCE_URL + 1) + 1];
    size_t at = 0;
    int found = 0;
    for (int i = 0; i < have.count; i++) {
        if (sources_same_url(have.url[i], url)) { found = 1; continue; }
        at += snprintf(all + at, sizeof(all) - at, "%s\n", have.url[i]);
    }
    if (!found)
        return 0;
    if (storage_write(SOURCES_PATH, all, at) < 0)
        return -1;
    logline("sources: removed %s", url);
    return 1;
}

enum source_kind sources_kind(const char *url) {
    size_t n = strlen(url);
    while (n && url[n - 1] == '/')
        n--;
    if (n > 5 && strncmp(url + n - 5, ".json", 5) == 0)
        return SOURCE_CATALOG;
    struct source_repo r;
    if (sources_parse_repo(url, &r))
        return SOURCE_REPO;
    return SOURCE_LIST;
}

int sources_parse_repo(const char *url, struct source_repo *out) {
    static const char host[] = "https://github.com/";
    memset(out, 0, sizeof(*out));
    if (strncmp(url, host, sizeof(host) - 1) != 0)
        return 0;
    const char *p = url + sizeof(host) - 1;
    const char *slash = strchr(p, '/');
    if (!slash || slash == p)
        return 0;
    size_t n = (size_t)(slash - p);
    if (n >= sizeof(out->owner))
        return 0;
    memcpy(out->owner, p, n);
    p = slash + 1;
    /* The name runs to an "@tag", a slash, or the end; a trailing ".git"
       is how git spells the same repository. */
    size_t len = strcspn(p, "@/");
    if (!len || len >= sizeof(out->name))
        return 0;
    memcpy(out->name, p, len);
    if (len > 4 && strcmp(out->name + len - 4, ".git") == 0)
        out->name[len - 4] = '\0';
    const char *rest = p + len;
    if (*rest == '/')
        rest++;
    if (*rest && *rest != '@')
        return 0;
    if (*rest == '@') {
        rest++;
        if (!*rest || strlen(rest) >= sizeof(out->ref) ||
            strspn(rest, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") !=
                strlen(rest))
            return 0;
        strcpy(out->ref, rest);
    } else
        strcpy(out->ref, "HEAD");
    const char *parts[] = {out->owner, out->name};
    for (int i = 0; i < 2; i++) {
        if (!*parts[i] || !strcmp(parts[i], ".") || !strcmp(parts[i], ".."))
            return 0;
        if (strspn(parts[i], "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") !=
            strlen(parts[i]))
            return 0;
    }
    return 1;
}

int sources_parse_list(const char *text, struct source_list *out) {
    memset(out, 0, sizeof(*out));
    static char copy[64 * 1024];
    snprintf(copy, sizeof(copy), "%s", text);
    char *line = copy;
    while (line && *line && out->count < LIST_REPOS) {
        char *end = strpbrk(line, "\r\n");
        if (end)
            *end++ = '\0';
        if (trim(line) > 0) {
            if (strncmp(line, "cache ", 6) == 0) {
                char *url = line + 6;
                trim(url);
                if (!out->cache[0])
                    snprintf(out->cache, sizeof(out->cache), "%s", url);
            } else {
                /* The URL is the whole line. Anything after it is from a
                   list written for the older shape, where the category and
                   the overrides stood there; it is passed over rather than
                   made to refuse the repository, since the .pspdx says all
                   of it now. */
                line[strcspn(line, " \t")] = '\0';
                if (sources_parse_repo(line, &out->repo[out->count]))
                    out->count++;
            }
        }
        line = end;
        while (line && (*line == '\r' || *line == '\n'))
            line++;
    }
    return out->count;
}

/* The id names a directory on the stick, so only [a-z0-9] of the owner
   and the repository survive in it: "Chris-Opter/PSP-Thing" becomes
   io.github.chrisopter.pspthing. */
static size_t append_plain(char *id, size_t n, size_t size, const char *text) {
    for (const char *p = text; *p && n + 1 < size; p++) {
        int c = tolower((unsigned char)*p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            id[n++] = (char)c;
    }
    id[n] = '\0';
    return n;
}

void sources_repo_id(const struct source_repo *r, char *id, size_t size) {
    size_t n = (size_t)snprintf(id, size, "io.github.");
    n = append_plain(id, n, size, r->owner);
    if (n + 1 < size)
        id[n++] = '.';
    append_plain(id, n, size, r->name);
}

void sources_repo_url(const struct source_repo *r, char *url, size_t size) {
    snprintf(url, size, "https://github.com/%s/%s", r->owner, r->name);
}

int sources_release_url(const char *source, const char *url) {
#ifdef PSPDX_TEST_FIXTURES
    if (!strncmp(url, "https://127.0.0.1:", 18))
        return 1;
#endif
    struct source_repo repo;
    if (!sources_parse_repo(source, &repo))
        return 0;
    char prefix[384];
    snprintf(prefix, sizeof(prefix), "https://github.com/%s/%s/releases/download/", repo.owner,
             repo.name);
    size_t n = strlen(prefix);
    if (strncasecmp(prefix, url, n))
        return 0;
    const char *tail = url + n;
    const char *slash = strchr(tail, '/');
    return slash && slash != tail && slash[1] && !strpbrk(tail, "\r\n\t ");
}
