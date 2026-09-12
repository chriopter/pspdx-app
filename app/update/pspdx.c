#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

#include "update/pspdx.h"

/* The one line that says which file this is. A version 2 gets a new name,
   so an old console simply does not recognise it and says so, rather than
   reading fields that have moved. */
#define SCHEMA_V1 "https://github.com/chriopter/pspdx-schema/blob/master/schema/v1.pspdx"

/* The five of schema/v1.pspdx, spelled as the schema spells them: one word
   for what this one app is, singular. The tabs above them are plural because
   a tab holds many; the word in the file names the app and is never the
   tab's. */
static const char *CATEGORY[] = { "game", "emulator", "app", "plugin", "demo" };

/* An optional string, cut to what the field holds rather than refused for
   being long: the cache is where a summary over sixty characters is
   reported, and the console has a card of a fixed width anyway. A cut lands
   on a character boundary so a UTF-8 sequence is not halved. */
static void take(char *dst, size_t size, const cJSON *value) {
    dst[0] = '\0';
    if (!cJSON_IsString(value)) return;
    size_t n = strlen(value->valuestring);
    if (n >= size) {
        n = size - 1;
        while (n && ((unsigned char)value->valuestring[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, value->valuestring, n);
    dst[n] = '\0';
}

/* A media directory as the URLs want it: no slash at either end, so that
   "media/", "media" and "/media/" all build the same address. Empty means
   the repository root, which is what a file without the field asks for. */
static void take_dir(char *dst, size_t size, const cJSON *value) {
    take(dst, size, value);
    size_t n = strlen(dst);
    while (n && dst[n - 1] == '/') dst[--n] = '\0';
    size_t lead = 0;
    while (dst[lead] == '/') lead++;
    if (lead) memmove(dst, dst + lead, n - lead + 1);
}

int pspdx_parse(const char *text, size_t len, struct pspdx_file *out,
                char *reason, size_t reason_size) {
    memset(out, 0, sizeof(*out));
    snprintf(reason, reason_size, "unreadable");

    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) { snprintf(reason, reason_size, "not json"); return -1; }
    if (!cJSON_IsObject(root)) {
        snprintf(reason, reason_size, "not an object");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsString(schema) || strcmp(schema->valuestring, SCHEMA_V1) != 0) {
        snprintf(reason, reason_size, "schema is not v1");
        cJSON_Delete(root);
        return -1;
    }

    take(out->name, sizeof(out->name), cJSON_GetObjectItemCaseSensitive(root, "name"));
    if (!out->name[0]) {
        snprintf(reason, reason_size, "no name");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *category = cJSON_GetObjectItemCaseSensitive(root, "category");
    int known = 0;
    if (cJSON_IsString(category))
        for (unsigned i = 0; i < sizeof(CATEGORY) / sizeof(*CATEGORY); i++)
            if (strcmp(category->valuestring, CATEGORY[i]) == 0) known = 1;
    if (!known) {
        /* Not one of the five means no tab shows it, which is the same as
           not being listed; better said here than shown as an empty row. */
        snprintf(reason, reason_size, "category %s",
                 cJSON_IsString(category) ? category->valuestring : "missing");
        cJSON_Delete(root);
        return -1;
    }
    snprintf(out->category, sizeof(out->category), "%s", category->valuestring);

    take(out->author, sizeof(out->author), cJSON_GetObjectItemCaseSensitive(root, "author"));
    take(out->summary, sizeof(out->summary), cJSON_GetObjectItemCaseSensitive(root, "summary"));
    take(out->license, sizeof(out->license), cJSON_GetObjectItemCaseSensitive(root, "license"));
    take(out->asset, sizeof(out->asset), cJSON_GetObjectItemCaseSensitive(root, "asset"));
    take(out->release, sizeof(out->release), cJSON_GetObjectItemCaseSensitive(root, "release"));
    take_dir(out->media, sizeof(out->media), cJSON_GetObjectItemCaseSensitive(root, "media"));

    cJSON *install = cJSON_GetObjectItemCaseSensitive(root, "install");
    if (cJSON_IsObject(install)) {
        take(out->root, sizeof(out->root), cJSON_GetObjectItemCaseSensitive(install, "root"));
        take(out->dir, sizeof(out->dir), cJSON_GetObjectItemCaseSensitive(install, "dir"));
    }

    cJSON_Delete(root);
    reason[0] = '\0';
    return 0;
}
