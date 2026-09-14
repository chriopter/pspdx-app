#include "update/reach.h"

#include <stdio.h>

#include "update/sources.h"

/* By URL rather than by line: the file can change between a fetch and the
   gear being opened, and a line that moved is still the same source. */
struct reach {
    char url[SOURCES_MAX][SOURCE_URL];
    int count;
};

static struct reach g_fetching, g_taken;

void reach_reset(void) { g_fetching.count = 0; }

void reach_failed(const char *url) {
    if (g_fetching.count < SOURCES_MAX)
        snprintf(g_fetching.url[g_fetching.count++], SOURCE_URL, "%s", url);
}

void reach_take(void) { g_taken = g_fetching; }

int reach_unreachable(const char *url) {
    for (int i = 0; i < g_taken.count; i++)
        if (sources_same_url(g_taken.url[i], url))
            return 1;
    return 0;
}
