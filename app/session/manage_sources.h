#ifndef PSPDX_MANAGE_SOURCES_H
#define PSPDX_MANAGE_SOURCES_H

#include <stddef.h>

#include "update/sources.h"

/* Manage sources, as a model: Add source at the top, the way a tab that is
   a job carries the job above what it would be done to, and under it one
   row per line of sources.txt, saying how the last fetch went for it. The
   right column's words for the row under the cursor are made here too, so
   the host tests read what the console shows. gui/sources_view.c draws it;
   session/options.c walks it. */

#define MANAGE_ROWS (SOURCES_MAX + 2)

enum manage_kind { MANAGE_ADD, MANAGE_DIRECT, MANAGE_SOURCE };

/* How the last fetch went: not fetched since the line was added, loaded,
   served from its saved copy only, or not loaded at all. */
enum manage_state { MANAGE_NOT_LOADED, MANAGE_OK, MANAGE_OFFLINE, MANAGE_UNREACHABLE };

struct manage_row {
    enum manage_kind kind;
    int source;                 /* index into the sources, -1 for the two jobs */
    enum source_kind skind;     /* what kind of source, for MANAGE_SOURCE */
    enum manage_state state;
    int apps;                   /* apps it brought, -1 when unknown */
    unsigned loaded_at;         /* unix seconds, 0 when not loaded live */
    char name[64];              /* the host, or owner/repo */
    char status[64];            /* the kind and the line under the name */
};

struct manage_sources {
    const struct sources *sources;
    struct manage_row row[MANAGE_ROWS];
    int count, cursor;
};

/* The rows out of the sources and what update/reach.h last took; the
   cursor stays where it was, as far as the rows still reach. */
void manage_build(struct manage_sources *m, const struct sources *s);

/* Up and down, a ring like the list. */
void manage_move(struct manage_sources *m, int by);

/* The row's name, over the right column. */
const char *manage_title(const struct manage_sources *m, int row);

/* The row's URL in full, "" for Add source. */
const char *manage_url(const struct manage_sources *m, int row);

/* What the row is, in a sentence or two. */
void manage_note(const struct manage_sources *m, int row, char *out, size_t size);

/* The facts under the URL -- the kind, the apps, when it was loaded -- one
   line each, at most max of them. Returns how many. */
#define MANAGE_FACT 64
int manage_facts(const struct manage_sources *m, int row, char facts[][MANAGE_FACT], int max);

#endif
