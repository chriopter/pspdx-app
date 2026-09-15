#include "update/reach.h"

#include <stdio.h>

#include "update/sources.h"

/* By URL rather than by line: the file can change between a fetch and the
   gear being opened, and a line that moved is still the same source. */
struct reach {
    char url[SOURCES_MAX][SOURCE_URL];
    int count;
};

static struct reach g_fetching, g_taken, g_fetching_saved, g_taken_saved;

void reach_reset(void) { g_fetching.count = g_fetching_saved.count = 0; }

static void note(struct reach *r, const char *url) {
    if (r->count < SOURCES_MAX)
        snprintf(r->url[r->count++], SOURCE_URL, "%s", url);
}
void reach_failed(const char *url) { note(&g_fetching, url); }
void reach_saved(const char *url) { note(&g_fetching_saved, url); }

void reach_take(void) {
    g_taken = g_fetching;
    g_taken_saved = g_fetching_saved;
}

static int has(const struct reach *r, const char *url) {
    for (int i = 0; i < r->count; i++)
        if (sources_same_url(r->url[i], url))
            return 1;
    return 0;
}
int reach_unreachable(const char *url) { return has(&g_taken, url); }
int reach_offline_copy(const char *url) { return has(&g_taken_saved, url); }
