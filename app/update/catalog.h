#ifndef PSPDX_CATALOG_H
#define PSPDX_CATALOG_H

#include "pspkit-https/https.h"
#include "install/install.h"

#define MAX_APPS 64
#define MAX_SUMMARY 241

/* PSPDX is an app in its own catalog, and a few things have to know which row
   is the client itself: the one that cannot be removed while it is running,
   and the one whose record was written by a first start rather than by an
   install. The id is written once, here. */
#define PSPDX_SELF_ID "io.github.chriopter.pspdx"

enum app_state { APP_UNKNOWN, APP_NOT_INSTALLED, APP_CURRENT, APP_UPDATE };

struct app_entry {
    char id[96];
    char name[157];
    char author[157];
    char summary[MAX_SUMMARY];
    char category[12];
    char license[257];
    /* The release the entry installs from, as a cache derived it or as
       GitHub answered at the origin, so what is current is known without
       a fetch per app. repo is the repository it came from: it goes into
       the record on the stick, and it is how a list's line and a cache's
       entry are known to be the same app. */
    struct manifest release;
    int has_release;
    int fresh;
    int media_cached_only;
    char repo[256];
    /* Absolute already: the catalog serves these relative to itself, and
       resolving them once at parse time keeps the base URL in this file. */
    char icon[256];
    char screenshot[256];
    char video[256];
    char sound[256];            /* SND0.AT3 out of the EBOOT, the card's own loop */
    enum app_state state;
    unsigned local_rev, remote_rev;
    char local_version[32], remote_version[32];
};

struct catalog {
    struct app_entry apps[MAX_APPS];
    int count;
    unsigned generated;             /* the oldest source's own stamp, unix seconds; 0 unknown */
    char generated_from[64];        /* that source's host, for the line that names it */
    int total;
    size_t response_len;
    struct https_result fetch;
};

/* Every source in PSP/PSPDX/sources.txt, in order, merged into one catalog
   by id, the first to name an id winning. A list's cache is taken when it
   answers; every repository the cache did not cover, and every one when
   there is no cache, is asked at the origin. Returns the number of apps,
   or -1 when no source answered at all. */
int catalog_fetch(struct catalog *catalog);
void catalog_offline(int value);
/* Whether the last catalog that did not come was one too big for the
   buffer, which is a different sentence from one that did not answer. */
int catalog_too_large(void);
void catalog_force_sources(void);
int catalog_prepare(struct app_entry *entry);
int catalog_validate_source(const char *url,int repository);

/* One repository asked at the origin and put into the catalog, for the
   unattended install: the index of its entry, which may have been there
   already, or -1 when GitHub had no release with a zip for it. */
int catalog_add_repo(struct catalog *catalog, const char *url);

/* The entry that came from a repository, by its URL, or -1. */
int catalog_find_repo(const struct catalog *catalog, const char *url);

/* The cache last taken, or the built-in one before any was: what the info
   band names as the catalog and what the bench fetches. */
const char *catalog_url(void);

/* Where a fetch has got to, for the status line -- "origin: 3 of 12" while
   a list's repositories are asked at GitHub -- and empty when there is
   nothing more to say than what the network stack says. Written by the
   fetching thread. */
const char *catalog_progress(void);

/* Why a repository, named by its URL, did not make it into the catalog
   when it was asked at the origin: 0 if it did or was never asked for,
   REFUSED_PSPDX when it has no .pspdx in its root or the one it has is not
   a v1 file, REFUSED_REPO when GitHub had no such repository or did not
   answer, REFUSED_RELEASE when it has no release with one zip on it. Only
   the last one is kept, which is the one the gear tab just asked for. */
#define REFUSED_REPO (-1)
#define REFUSED_RELEASE (-2)
#define REFUSED_PSPDX (-3)
int catalog_refused(const char *url);

int catalog_check_updates(struct catalog *catalog);
void catalog_dump_http(void);

#endif
