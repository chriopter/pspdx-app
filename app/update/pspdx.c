#include "update/pspdx.h"
#include "update/sources.h"
#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
static int characters(const char *s) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        int k = 0;
        unsigned min = 0;
        if (c < 0x80) {
            if (c < 32)
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
        int limit, required;
    };
    char schema[128];
    struct field fields[] = {{"schema", schema, sizeof(schema), 127, 1},
                             {"source", out->source, sizeof(out->source), 255, 1},
                             {"installdir", out->installdir, sizeof(out->installdir), 41, 1},
                             {"name", out->name, sizeof(out->name), 39, 1},
                             {"category", out->category, sizeof(out->category), 11, 1},
                             {"author", out->author, sizeof(out->author), 39, 0},
                             {"summary", out->summary, sizeof(out->summary), 60, 0},
                             {"license", out->license, sizeof(out->license), 64, 0}};
    cJSON *v;
    cJSON_ArrayForEach(v, root) {
        int found = 0;
        for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); i++)
            if (!strcmp(v->string, fields[i].key))
                found = 1;
        if (!found) {
            snprintf(reason, cap, "unknown field: %s", v->string);
            goto bad;
        }
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
        int n = cJSON_IsString(v) ? characters(v->valuestring) : -1;
        if (n < 0 || n > f->limit || (f->required && !n) || strlen(v->valuestring) >= f->size) {
            snprintf(reason, cap, "invalid %s", f->key);
            goto bad;
        }
        strcpy(f->dst, v->valuestring);
    }
    struct source_repo repo;
    if (strcmp(schema, PSPDX_SCHEMA)) {
        snprintf(reason, cap, "schema is not v1");
        goto bad;
    }
    if (!sources_parse_repo(out->source, &repo) || strchr(out->source, '@')) {
        snprintf(reason, cap, "invalid source");
        goto bad;
    }
    if (!pspdx_install_dir(out->installdir)) {
        snprintf(reason, cap, "invalid installdir");
        goto bad;
    }
    if (strcmp(out->category, "app") && strcmp(out->category, "game") &&
        strcmp(out->category, "demo") && strcmp(out->category, "plugin") &&
        strcmp(out->category, "emulator")) {
        snprintf(reason, cap, "invalid category");
        goto bad;
    }
    cJSON_Delete(root);
    reason[0] = 0;
    return 0;
bad:
    cJSON_Delete(root);
    return -1;
}
