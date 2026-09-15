#ifndef PSPDX_CATALOG_H
#define PSPDX_CATALOG_H

#include "pspkit-https/https.h"
#include "install/install.h"
#include "update/pspdx.h"

#define MAX_APPS 64
#define MAX_SUMMARY 241

/* PSPDX is an app in its own catalog, and a few things have to know which row
   is the client itself: the one that cannot be removed while it is running,
   and the one whose record was written by a first start rather than by an
   install. The id is written once, here. */
#define PSPDX_SELF_ID "io.github.chriopter.pspdxapp"
/* What PSPDX up to 0.5 called itself, from where it was published then. */
#define PSPDX_LEGACY_ID "io.github.chriopter.pspdx"
#define PSPDX_LEGACY_SOURCE "https://github.com/chriopter/pspdx"

enum app_state { APP_UNKNOWN, APP_NOT_INSTALLED, APP_CURRENT, APP_UPDATE };

struct app_entry {
    char id[PSPDX_ID_SIZE];
    char name[161];
    char author[241];
    char summary[MAX_SUMMARY];
    char license[241];
    /* The file's tags, a newline between them: the tabs are made of the ones
       the browser knows, and an app may stand in several. */
    char tags[PSPDX_TAGS_TEXT];
    /* The one group the app names, which decides its tab over the tags;
       empty when it names none. */
    char category[PSPDX_CATEGORY_SIZE];
    /* The file's description, on the heap and owned by the entry, or NULL
       when it has none: kilobytes that only the details band reads. */
    char *description;
    /* Listed and not installable by this version: a plugin or an ISO. An
       app from outside GitHub installs from its catalog entry, which is all
       there is of it, and is updated only through a catalog. */
    int unsupported;
    char type[12];              /* homebrew, plugin or iso */
    /* The release the entry installs from, as a cache derived it or as
       GitHub answered at the origin, so what is current is known without
       a fetch per app. repo is the repository it came from: it goes into
       the record on the stick, and it is how a list's line and a cache's
       entry are known to be the same app. */
    struct manifest release;
    int has_release;
    /* The repository answered that it has no .pspdx, and the entry is made
       out of something else: PSPDX_FROM_FILE the .pspdx the user put in
       INBOX, PSPDX_FROM_REPOSITORY the repository's own name, for a
       repository the user typed. 0 for every other entry. */
    int no_pspdx;
    /* A .pspdx from INBOX stands in the release until catalog_prepare has
       asked the repository whether it has one of its own, which wins. */
    int from_inbox;
    /* The release's tag as the list or GitHub wrote it, "v" and all: what a
       pinned tag is compared with, exactly. Empty where neither said. */
    char tag[PSPDX_TAG_SIZE];
    int fresh;
    int media_cached_only;
    char repo[PSPDX_URL_SIZE];
    /* Absolute already: the catalog serves these relative to itself, and
       resolving them once at parse time keeps the base URL in this file. */
    char icon[256];
    char screenshot[256];
    char video[256];
    char sound[256];            /* SND0.AT3 out of the EBOOT, the card's own loop */
    enum app_state state;
    unsigned local_rev, remote_rev;
    /* The installed zip's SHA-256 out of its record, when the record has one. */
    unsigned char local_sha256[32];
    int local_has_sha;
    char local_version[VERSION_SIZE], remote_version[VERSION_SIZE];
};

/* An entry lets go of what it holds on the heap and is all zeros again. Every
   slot of a catalog is either zeros or an entry that owns its text, so this
   is safe on any of them, whether or not it was ever counted. */
void entry_clear(struct app_entry *entry);

struct catalog {
    struct app_entry apps[MAX_APPS];
    int count;
    unsigned generated;             /* the oldest source's own stamp, unix seconds; 0 unknown */
    char generated_from[64];        /* that source's host, for the line that names it */
    int total;
    /* The first entry left out because another has its folder under
       PSP/GAME, said on the status line once the fetch is through; empty
       when there was none. */
    char collision[96];
    size_t response_len;
    struct https_result fetch;
};

/* Every source in PSP/PSPDX/sources.txt, in order, merged into one catalog
   by id, the first to name an id winning. A list's cache is taken when it
   answers; every repository the cache did not cover, and every one when
   there is no cache, is asked at the origin. Returns the number of apps,
   or -1 when no source answered at all. */
int catalog_fetch(struct catalog *catalog);
/* Every slot of the catalog cleared, counted or not, and the count with them. */
void catalog_free(struct catalog *catalog);
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
#define PSPDX_FROM_FILE 1
#define PSPDX_FROM_REPOSITORY 2
int catalog_add_repo(struct catalog *catalog, const char *url, int make);
/* The app a .pspdx from INBOX describes, for a repository that has none of its
   own: its release asked at GitHub by the file, and the file the app's. The
   index of its entry, or -1. */
int catalog_add_file(struct catalog *catalog, const char *raw);
/* For a row that knows no release tag yet -- an installed app no catalog
   lists this session -- the release a .pspdx from INBOX pins, asked at
   GitHub by exactly that tag and put into the entry. 0 when GitHub answered
   with a release that installs, -1 otherwise and outside GitHub. */
int catalog_ask_pinned(struct app_entry *entry, const char *raw);

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
   answer, REFUSED_RELEASE when it has no release with one zip on it, and
   REFUSED_FOLDER as below. Only
   the last one is kept, which is the one the gear tab just asked for. */
#define REFUSED_REPO (-1)
#define REFUSED_RELEASE (-2)
#define REFUSED_PSPDX (-3)
/* REFUSED_FOLDER: the app installs to a folder under PSP/GAME that an entry
   already listed has, which catalog_refused_folder names. */
#define REFUSED_FOLDER (-4)
/* REFUSED_NO_ANSWER: GitHub did not answer -- a rate limit, an error of its
   own -- which says nothing about the repository. */
#define REFUSED_NO_ANSWER (-5)
/* REFUSED_NO_PSPDX: GitHub answered 404 for the repository's .pspdx. */
#define REFUSED_NO_PSPDX (-6)
int catalog_refused(const char *url);
const char *catalog_refused_folder(void);

/* The line that says an app is not listed because its folder, PSP/GAME/<dir>,
   is another app's: the name cut to leave the rest whole in size bytes. */
void catalog_folder_line(char *out, size_t size, const char *name, const char *dir);

int catalog_check_updates(struct catalog *catalog);
/* An update whose version is the one installed: the same tag over another
   ZIP, which the screen calls a new build rather than "0.2.6, 0.2.6". */
int catalog_new_build(const struct app_entry *entry);
void catalog_dump_http(void);

#endif
