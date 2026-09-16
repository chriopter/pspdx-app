#include "update/reach.h"

#include <stdio.h>

#include "update/sources.h"

/* By URL rather than by line: the file can change between a fetch and the
   gear being opened, and a line that moved is still the same source. */
struct reach {
    char url[SOURCES_MAX][SOURCE_URL];
    int apps[SOURCES_MAX];
    unsigned at[SOURCES_MAX];
    int count;
};

static struct reach g_fetching, g_taken, g_fetching_saved, g_taken_saved;
static struct reach g_fetching_loaded, g_taken_loaded;

void reach_reset(void) {
    g_fetching.count = g_fetching_saved.count = g_fetching_loaded.count = 0;
}

static int note(struct reach *r, const char *url) {
    if (r->count >= SOURCES_MAX) return -1;
    snprintf(r->url[r->count], SOURCE_URL, "%s", url);
    return r->count++;
}
void reach_failed(const char *url) { note(&g_fetching, url); }
void reach_saved(const char *url) { note(&g_fetching_saved, url); }
void reach_loaded(const char *url, int apps, unsigned at) {
    int i = note(&g_fetching_loaded, url);
    if (i < 0) return;
    g_fetching_loaded.apps[i] = apps;
    g_fetching_loaded.at[i] = at;
}

void reach_take(void) {
    g_taken = g_fetching;
    g_taken_saved = g_fetching_saved;
    g_taken_loaded = g_fetching_loaded;
}

static int find(const struct reach *r, const char *url) {
    for (int i = 0; i < r->count; i++)
        if (sources_same_url(r->url[i], url))
            return i;
    return -1;
}
int reach_unreachable(const char *url) { return find(&g_taken, url) >= 0; }
int reach_offline_copy(const char *url) { return find(&g_taken_saved, url) >= 0; }
int reach_apps(const char *url) {
    int i = find(&g_taken_loaded, url);
    return i < 0 ? -1 : g_taken_loaded.apps[i];
}
unsigned reach_loaded_at(const char *url) {
    int i = find(&g_taken_loaded, url);
    return i < 0 ? 0 : g_taken_loaded.at[i];
}
