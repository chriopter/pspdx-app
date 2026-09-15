#include "update/pspdx.h"
#include "update/sources.h"
#include <cjson/cJSON.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
/* No string of the format holds a control character -- except the newline a
   description may put between its paragraphs, when newline says it may. */
int pspdx_characters(const char *s, int newline) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        int k = 0;
        unsigned min = 0;
        if (c < 0x80) {
            if (c < 32 && !(newline && c == '\n'))
                return -1;
        } else if (c >= 0xc2 && c <= 0xdf) {
            k = 1;
            c &= 31;
            min = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            k = 2;
            c &= 15;
            min = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            k = 3;
            c &= 7;
            min = 0x10000;
        } else
            return -1;
        for (int i = 0; i < k; i++) {
            if ((*p & 0xc0) != 0x80)
                return -1;
            c = (c << 6) | (*p++ & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return -1;
        n++;
    }
    return n;
}
/* What cJSON lets through and JSON does not, read before it: a byte that is
   not UTF-8, a control byte inside a string, one between tokens other than
   the four JSON calls whitespace, a number JSON would not write, and the
   escape of a NUL, which a cJSON string cannot hold. Read escape by escape,
   so \\u0000 in the file -- an escaped backslash and the text u0000 -- is
   left alone. A byte order mark at the start is skipped, as cJSON skips it. */
static int json_text(const char *text, size_t len) {
    const unsigned char *p = (const unsigned char *)text, *end = p + len;
    int in = 0;
    if (len >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3))
        p += 3;
    while (p < end) {
        unsigned c = *p;
        if (c < 0x20) {
            if (in || (c != '\t' && c != '\n' && c != '\r'))
                return 0;
            p++;
        } else if (c == '"') {
            in = !in;
            p++;
        } else if (c == '\\' && in) {
            if (end - p < 2)
                return 0;
            if (p[1] == 'u' && end - p >= 6 && !memcmp(p + 2, "0000", 4))
                return 0;
            p += 2;
        } else if (!in && (c == '-' || (c >= '0' && c <= '9'))) {
            /* A number as JSON writes one; cJSON also takes 01, 1. and 1.e5. */
            const unsigned char *q = p + (c == '-');
            if (q == end || *q < '0' || *q > '9')
                return 0;
            if (*q == '0')
                q++;
            else
                while (q < end && *q >= '0' && *q <= '9')
                    q++;
            if (q < end && *q == '.') {
                if (++q == end || *q < '0' || *q > '9')
                    return 0;
                while (q < end && *q >= '0' && *q <= '9')
                    q++;
            }
            if (q < end && (*q == 'e' || *q == 'E')) {
                if (++q < end && (*q == '+' || *q == '-'))
                    q++;
                if (q == end || *q < '0' || *q > '9')
                    return 0;
                while (q < end && *q >= '0' && *q <= '9')
                    q++;
            }
            if (q < end && (*q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-' ||
                            (*q >= '0' && *q <= '9')))
                return 0;
            p = q;
        } else if (c < 0x80) {
            p++;
        } else {
            /* One character of well-formed UTF-8, its bytes all inside len. */
            char one[5] = {0};
            size_t k = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
            if ((size_t)(end - p) < k)
                return 0;
            memcpy(one, p, k);
            if (pspdx_characters(one, 0) != 1)
                return 0;
            p += k;
        }
    }
    return 1;
}
/* Days from the civil date by Howard Hinnant's arithmetic; the month lengths
   are only there to refuse a day no calendar has. */
unsigned pspdx_time(const char *text) {
    int y, mo, d, h = 0, mi = 0, sec = 0;
    /* A release entered by hand may know only its day, "2024-12-20"; it
       counts from that day's midnight, which orders it and dates it, and
       nothing more is asked of it. */
    size_t n = text ? strlen(text) : 0;
    if ((n != 20 && n != 10) || text[4] != '-' || text[7] != '-' ||
        (n == 20 && (text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z')))
        return 0;
    for (size_t i = 0; i < (n == 20 ? 19 : 10); i++)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && (text[i] < '0' || text[i] > '9'))
            return 0;
    if (n == 20 ? sscanf(text, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6
                : sscanf(text, "%d-%d-%d", &y, &mo, &d) != 3)
        return 0;
    static const unsigned char days_in[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (mo < 1 || mo > 12 || d < 1 || d > days_in[mo - 1] + (mo == 2 && leap) || y < 1970 ||
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
/* The release a file pins: an object with a tag of 1 to 64 characters, and
   where it names them an https address of up to 512 characters and the time
   it was published. What else the object holds is passed over, as a field
   of the file this version does not know is. 0, or -1 with why. */
static int pinned_release(const cJSON *v, struct pspdx_file *out, const char **why) {
    static const char *const keys[] = {"tag", "url", "published_at"};
    *why = "invalid release";
    if (!cJSON_IsObject(v))
        return -1;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
        int seen = 0;
        for (const cJSON *w = v->child; w; w = w->next)
            if (!strcmp(w->string, keys[i]) && seen++) {
                *why = "duplicate field in release";
                return -1;
            }
    }
    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(v, "tag");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(v, "url");
    const cJSON *published = cJSON_GetObjectItemCaseSensitive(v, "published_at");
    int n = cJSON_IsString(tag) ? pspdx_characters(tag->valuestring, 0) : -1;
    if (n < 1 || n > 64 || strlen(tag->valuestring) >= sizeof(out->release_tag)) {
        *why = "invalid release tag";
        return -1;
    }
    if (url && (!cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) ||
                !url->valuestring[8] || strlen(url->valuestring) >= sizeof(out->release_url) ||
                strspn(url->valuestring, "!#$%&'()*+,-./0123456789:;=?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                         "[]_abcdefghijklmnopqrstuvwxyz~") !=
                    strlen(url->valuestring))) {
        *why = "invalid release url";
        return -1;
    }
    unsigned when = cJSON_IsString(published) ? pspdx_time(published->valuestring) : 0;
    if (published && !when) {
        *why = "invalid release published_at";
        return -1;
    }
    strcpy(out->release_tag, tag->valuestring);
    if (url)
        strcpy(out->release_url, url->valuestring);
    out->release_published = when;
    return 0;
}
void pspdx_utf8_mend(char *s) {
    size_t n = strlen(s), lead = n;
    /* Back over the continuation bytes to the byte that starts the last
       character, and keep it only if all the bytes it announces are there. */
    while (lead > 0 && (s[lead - 1] & 0xc0) == 0x80 && n - lead < 3)
        lead--;
    if (!lead)
        return;
    unsigned char c = (unsigned char)s[lead - 1];
    size_t want = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    if (c >= 0x80 && n - (lead - 1) < want)
        s[lead - 1] = '\0';
}
char *pspdx_description(const char *text, size_t len) {
    cJSON *root = cJSON_ParseWithLength(text, len);
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "description");
    char *out = NULL;
    if (cJSON_IsString(v) && v->valuestring[0] && pspdx_characters(v->valuestring, 1) >= 0) {
        size_t n = strlen(v->valuestring);
        out = malloc(n + 1);
        if (out)
            memcpy(out, v->valuestring, n + 1);
    }
    cJSON_Delete(root);
    return out;
}
int pspdx_install_dir(const char *path) {
    if (!path || strncmp(path, "PSP/GAME/", 9))
        return 0;
    const char *d = path + 9;
    size_t n = strlen(d);
    /* FAT drops a trailing dot, so PSP/GAME/Demo. is PSP/GAME/Demo on the
       stick and ... the folder above: a name may not end in one. */
    if (!n || n > 32 || d[n - 1] == '.' || !strcasecmp(d, ".pspdx-stage"))
        return 0;
    for (; *d; d++)
        if (!((*d >= 'A' && *d <= 'Z') || (*d >= 'a' && *d <= 'z') || (*d >= '0' && *d <= '9') ||
              strchr("_.-", *d)))
            return 0;
    return 1;
}
void pspdx_default_dir(const char *repo, const char *name, char *out, size_t size) {
    if (size < sizeof("PSP/GAME/")) {
        if (size)
            out[0] = '\0';
        return;
    }
    size_t n = (size_t)snprintf(out, size, "PSP/GAME/");
    int kept = 0;
    for (const char *p = repo ? repo : name; *p && kept < 32 && n + 1 < size; p++)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '_' || *p == '.' || *p == '-') {
            out[n++] = *p;
            kept++;
        }
    /* The dots a name ends in are not a folder's: FAT would drop them. */
    while (n > 9 && out[n - 1] == '.')
        n--;
    out[n] = '\0';
}
int pspdx_type_installable(const char *type) { return type && !strcmp(type, "homebrew"); }
int pspdx_has_tag(const char *tags, const char *word) {
    size_t n = strlen(word);
    for (const char *p = tags; n && *p;) {
        size_t k = strcspn(p, "\n");
        if (k == n && !strncmp(p, word, n))
            return 1;
        p += k;
        if (*p)
            p++;
    }
    return 0;
}
int pspdx_parse(const char *text, size_t len, struct pspdx_file *out, char *reason, size_t cap) {
    memset(out, 0, sizeof(*out));
    snprintf(reason, cap, "invalid manifest");
    if (!text || !len || len > PSPDX_FILE_MAX || !json_text(text, len))
        return -1;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, 0);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    while (end < text + len && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        end++;
    if (end != text + len) {
        cJSON_Delete(root);
        return -1;
    }
    struct field {
        const char *key;
        char *dst;
        size_t size;
        int limit, required, newline;
    };
    char schema[128];
    /* The description has no dst: it is held to its rules and dropped. */
    struct field fields[] = {{"schema", schema, sizeof(schema), 127, 1, 0},
                             {"source", out->source, sizeof(out->source), 255, 1, 0},
                             {"name", out->name, sizeof(out->name), 40, 1, 0},
                             {"type", out->type, sizeof(out->type), 11, 0, 0},
                             {"category", out->category, sizeof(out->category), 24, 0, 0},
                             {"installdir", out->installdir, sizeof(out->installdir), 41, 0, 0},
                             {"author", out->author, sizeof(out->author), 60, 0, 0},
                             {"summary", out->summary, sizeof(out->summary), 60, 0, 0},
                             {"license", out->license, sizeof(out->license), 60, 0, 0},
                             {"description", NULL, PSPDX_FILE_MAX + 1, 2500, 0, 1}};
    cJSON *v;
    cJSON_ArrayForEach(v, root) {
        int found = !strcmp(v->string, "tags") || !strcmp(v->string, "release");
        for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); i++)
            if (!strcmp(v->string, fields[i].key))
                found = 1;
        /* A field this version does not know is passed over, not refused: a
           file written for a later version, or with a note of its own, still
           says everything this one needs, and the schema is what holds an
           author to the fields there are. */
        if (!found)
            continue;
        for (cJSON *w = v->next; w; w = w->next)
            if (!strcmp(v->string, w->string)) {
                snprintf(reason, cap, "duplicate field: %s", v->string);
                goto bad;
            }
    }
    for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        struct field *f = &fields[i];
        v = cJSON_GetObjectItemCaseSensitive(root, f->key);
        if (!v && !f->required)
            continue;
        int n = cJSON_IsString(v) ? pspdx_characters(v->valuestring, f->newline) : -1;
        if (n < 0 || n > f->limit || (f->required && !n) || strlen(v->valuestring) >= f->size) {
            snprintf(reason, cap, "invalid %s", f->key);
            goto bad;
        }
        if (f->dst)
            strcpy(f->dst, v->valuestring);
    }
    /* A category is a word: one of no characters names no group. */
    if (cJSON_GetObjectItemCaseSensitive(root, "category") && !out->category[0]) {
        snprintf(reason, cap, "invalid category");
        goto bad;
    }
    if (strcmp(schema, PSPDX_SCHEMA)) {
        snprintf(reason, cap, "schema is not v1");
        goto bad;
    }
    /* Any https:// address may be where a project lives, since a mirror of
       something abandoned is rarely on GitHub. One on github.com has to be
       a repository: the id, the default folder and the releases are read
       out of that. */
    struct source_repo repo;
    memset(&repo, 0, sizeof(repo));
    int github = !strncmp(out->source, "https://github.com/", 19);
    if (strncmp(out->source, "https://", 8) || !out->source[8] ||
        (github && (!sources_parse_repo(out->source, &repo) || strchr(out->source, '@')))) {
        snprintf(reason, cap, "invalid source");
        goto bad;
    }
    if (!cJSON_GetObjectItemCaseSensitive(root, "type"))
        strcpy(out->type, "homebrew");
    if (strcmp(out->type, "homebrew") && strcmp(out->type, "plugin") && strcmp(out->type, "iso")) {
        snprintf(reason, cap, "invalid type");
        goto bad;
    }
    /* A homebrew goes under PSP/GAME, into the folder the file names or the
       one its repository's name makes, or its own name away from GitHub; a
       plugin or an ISO goes elsewhere and names no folder there. */
    int homebrew = !strcmp(out->type, "homebrew");
    if (cJSON_GetObjectItemCaseSensitive(root, "installdir")) {
        if (!homebrew || !pspdx_install_dir(out->installdir)) {
            snprintf(reason, cap, "invalid installdir");
            goto bad;
        }
    } else if (homebrew) {
        pspdx_default_dir(github ? repo.name : NULL, out->name, out->installdir,
                          sizeof(out->installdir));
        if (!pspdx_install_dir(out->installdir)) {
            out->installdir[0] = 0;
            snprintf(reason, cap, "no installdir");
            goto bad;
        }
    }
    /* The id is the repository's on GitHub. Anywhere else it is the host of
       the source and the app's name, so a file whose host and name leave
       nothing to make one of is no app anyone could find again. */
    if (github ? sources_repo_id(&repo, out->id, sizeof(out->id)) < 0
               : sources_host_id(out->source, out->name, out->id, sizeof(out->id)) < 0) {
        snprintf(reason, cap, "no id: the source's host and the name make none");
        goto bad;
    }
    v = cJSON_GetObjectItemCaseSensitive(root, "release");
    const char *why;
    if (v && pinned_release(v, out, &why) < 0) {
        snprintf(reason, cap, "%s", why);
        goto bad;
    }
    /* GitHub says where a tag's zip is and when it was published; anywhere
       else the file has to. */
    if (v && !github && (!out->release_url[0] || !out->release_published)) {
        snprintf(reason, cap, "invalid release: outside GitHub it needs url and published_at");
        goto bad;
    }
    v = cJSON_GetObjectItemCaseSensitive(root, "tags");
    if (v) {
        size_t used = 0;
        cJSON *tag;
        if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > PSPDX_TAGS) {
            snprintf(reason, cap, "invalid tags");
            goto bad;
        }
        cJSON_ArrayForEach(tag, v) {
            int n = cJSON_IsString(tag) ? pspdx_characters(tag->valuestring, 0) : -1;
            size_t k = n > 0 ? strlen(tag->valuestring) : 0;
            if (n < 1 || n > 24 || used + (used > 0) + k >= sizeof(out->tags)) {
                snprintf(reason, cap, "invalid tag");
                goto bad;
            }
            for (cJSON *w = v->child; w != tag; w = w->next)
                if (!strcmp(w->valuestring, tag->valuestring)) {
                    snprintf(reason, cap, "duplicate tag");
                    goto bad;
                }
            if (used)
                out->tags[used++] = '\n';
            memcpy(out->tags + used, tag->valuestring, k + 1);
            used += k;
        }
    }
    cJSON_Delete(root);
    if (cap)
        reason[0] = 0;
    return 0;
bad:
    cJSON_Delete(root);
    return -1;
}
