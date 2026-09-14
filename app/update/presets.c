#include "update/presets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update/sources.h"
#include "util/runtime.h"
#include "util/self_presets.h"
#include "util/storage.h"

#define SEEN_PATH storage_path("PSP/PSPDX/presets.seen")
#define LIST_MAX 4096
#define SEEN_MAX 64

struct preset_list {
    char url[SOURCES_MAX][SOURCE_URL];
    int count;
};

static char g_seen[SEEN_MAX][SOURCE_URL];
static int g_seen_count;

/* The line *at starts, cut off at its end and its comment, its ends
   trimmed; *at moves on to the next. NULL once the text is through. */
static char *next_line(char **at) {
    char *line = *at;
    if (!line || !*line)
        return NULL;
    char *end = strpbrk(line, "\r\n");
    if (end)
        *end++ = '\0';
    *at = end;
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';
    while (isspace((unsigned char)*line))
        line++;
    size_t n = strlen(line);
    while (n && isspace((unsigned char)line[n - 1]))
        line[--n] = '\0';
    return line;
}

/* The sources a list names, in its order. A line has to be what sources.txt
   takes -- an https URL of a sane length -- and what Add source would take;
   anything else is said and passed over, never the end of the start. */
static int parse(const char *text, const char *from, struct preset_list *out) {
    static char copy[LIST_MAX + 1];
    snprintf(copy, sizeof(copy), "%s", text);
    char *at = copy;
    if (!strncmp(at, "\xEF\xBB\xBF", 3))
        at += 3;
    out->count = 0;
    for (char *line; (line = next_line(&at));) {
        char url[SOURCE_URL];
        if (!*line)
            continue;
        if (strncmp(line, "https://", 8) || strlen(line) >= SOURCE_URL ||
            sources_normalize(line, url, sizeof(url)) < 0) {
            logline("presets: %s: not a source, skipped: %.40s", from, line);
            continue;
        }
        if (out->count == SOURCES_MAX) {
            logline("presets: %s names more than %d sources, the rest skipped", from, SOURCES_MAX);
            break;
        }
        memcpy(out->url[out->count++], url, strlen(url) + 1);
    }
    return out->count;
}

/* What presets.seen remembers. A file that is there and will not read
   leaves the list empty, which offers every preset again: a source the
   user removed can come back once, which is better than never adding a
   new one. */
static void read_seen(void) {
    g_seen_count = 0;
    char *text = NULL;
    int n = storage_read(SEEN_PATH, &text, SEEN_MAX * (SOURCE_URL + 1) + 256);
    if (n < 0) {
        if (storage_exists(SEEN_PATH))
            logline("presets: presets.seen will not read, every preset counts as new");
        return;
    }
    char *at = text;
    for (char *line; (line = next_line(&at)) && g_seen_count < SEEN_MAX;)
        if (*line && strlen(line) < SOURCE_URL)
            memcpy(g_seen[g_seen_count++], line, strlen(line) + 1);
    free(text);
}

static int seen(const char *url) {
    for (int i = 0; i < g_seen_count; i++)
        if (sources_same_url(g_seen[i], url))
            return 1;
    return 0;
}

/* The whole list, rewritten: the file is PSPDX's own, not the user's. */
static void write_seen(void) {
    static char text[SEEN_MAX * (SOURCE_URL + 1) + 128];
    size_t at = (size_t)snprintf(text, sizeof(text),
                                 "# Presets already offered; none of these is added again\n");
    for (int i = 0; i < g_seen_count; i++)
        at += (size_t)snprintf(text + at, sizeof(text) - at, "%s\n", g_seen[i]);
    if (storage_write(SEEN_PATH, text, at) < 0)
        logline("presets: presets.seen would not write");
}

int presets_merge(const char *boot) {
#ifdef CATALOG_URL
    /* A build pointed at a catalog on the host trusts that host's CA alone:
       the published catalogs would be handshake failures in a log that a
       campaign is judged on. */
    (void)boot;
    logline("presets: a build for %s offers none", CATALOG_URL);
    return 0;
#else
    static struct preset_list list;
    char path[256];
    char *raw = NULL;
    int n = -1;
    const char *slash = boot ? strrchr(boot, '/') : NULL;
    if (slash && (size_t)(slash - boot) + sizeof("/presets.txt") <= sizeof(path)) {
        snprintf(path, sizeof(path), "%.*s/presets.txt", (int)(slash - boot), boot);
        n = storage_read(path, &raw, LIST_MAX);
    }
    if (n < 0 || parse(raw, "presets.txt", &list) == 0) {
        logline("presets: %s, the built-in list stands",
                n < 0 ? "no readable presets.txt beside the EBOOT" : "presets.txt names no source");
        parse(PSPDX_PRESETS, "the built-in list", &list);
    }
    free(raw);

    read_seen();
    int added = 0, marked = 0;
    for (int i = 0; i < list.count; i++) {
        if (seen(list.url[i]))
            continue;
        /* Already in the file counts as offered too: a stick that had the
           first catalog before there were presets gets only the rest. A
           write that fails is not remembered, and is tried next start. */
        char url[SOURCE_URL];
        int rc = sources_add(list.url[i], url, sizeof(url));
        if (rc < 0) {
            logline("presets: %s was not added, offered again next start", list.url[i]);
            continue;
        }
        added += rc;
        if (g_seen_count == SEEN_MAX) {
            logline("presets: presets.seen is full");
            continue;
        }
        memcpy(g_seen[g_seen_count++], url, strlen(url) + 1);
        marked++;
    }
    if (marked)
        write_seen();
    return added;
#endif
}
