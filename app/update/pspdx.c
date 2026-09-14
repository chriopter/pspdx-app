#include "update/pspdx.h"
#include "update/sources.h"
#include <cjson/cJSON.h>
#include <ctype.h>
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
    if (!n || n > 32 || !strcmp(d, ".") || !strcmp(d, "..") || !strcasecmp(d, ".pspdx-stage"))
        return 0;
    for (; *d; d++)
        if (!((*d >= 'A' && *d <= 'Z') || (*d >= 'a' && *d <= 'z') || (*d >= '0' && *d <= '9') ||
              strchr("_.-", *d)))
            return 0;
    return 1;
}
void pspdx_default_dir(const char *repo, const char *name, char *out, size_t size) {
    size_t n = (size_t)snprintf(out, size, "PSP/GAME/");
    int kept = 0;
    for (const char *p = repo ? repo : name; *p && kept < 32 && n + 1 < size; p++)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '_' || *p == '.' || *p == '-') {
            out[n++] = *p;
            kept++;
        }
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
    if (!text || !len || len > PSPDX_FILE_MAX || memchr(text, 0, len))
        return -1;
    /* cJSON strings cannot represent embedded NULs. Reject their JSON escape. */
    for (size_t i = 0; i + 5 < len; i++)
        if (!memcmp(text + i, "\\u0000", 6))
            return -1;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, 0);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    while (end < text + len && isspace((unsigned char)*end))
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
                             {"installdir", out->installdir, sizeof(out->installdir), 41, 0, 0},
                             {"author", out->author, sizeof(out->author), 60, 0, 0},
                             {"summary", out->summary, sizeof(out->summary), 60, 0, 0},
                             {"license", out->license, sizeof(out->license), 60, 0, 0},
                             {"listed_by", out->listed_by, sizeof(out->listed_by), 255, 0, 0},
                             {"description", NULL, PSPDX_FILE_MAX + 1, 2500, 0, 1}};
    cJSON *v;
    cJSON_ArrayForEach(v, root) {
        int found = !strcmp(v->string, "tags");
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
    int listed = cJSON_GetObjectItemCaseSensitive(root, "listed_by") != NULL;
    if (listed && (strncmp(out->listed_by, "https://", 8) || !out->listed_by[8])) {
        snprintf(reason, cap, "invalid listed_by");
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
    /* The id is the repository's on GitHub. Anywhere else it is the list
       that vouches for the app and the app's name, so a file that names no
       list, or whose list and name leave nothing to make one of, is no app
       anyone could find again. */
    if (github)
        sources_repo_id(&repo, out->id, sizeof(out->id));
    else if (!listed || sources_listed_id(out->listed_by, out->name, out->id, sizeof(out->id)) < 0) {
        snprintf(reason, cap, "no id: a source outside GitHub needs listed_by and a name");
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
    reason[0] = 0;
    return 0;
bad:
    cJSON_Delete(root);
    return -1;
}
