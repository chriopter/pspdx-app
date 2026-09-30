#ifndef PSPDX_CATALOG_H
#define PSPDX_CATALOG_H

#include "pspkit-https/https.h"
#include "install/install.h"
#include "update/pspdx.h"
#include "update/sources.h"

/* How many apps one fetch may hold, all sources together: far above any
   catalog there is, and the bound for the tables that count by app. The
   entries themselves are on the heap and only as many as there are. */
#define MAX_APPS 4096
#define MAX_SUMMARY 241
/* A source's name, from the catalog's own "name": up to 40 characters. */
#define CATALOG_NAME_SIZE (40 * 4 + 1)

/* PSPDX is an app in its own catalog, and a few things have to know which row
   is the client itself: the one that cannot be removed while it is running,
   and the one whose record was written by a first start rather than by an
   install. The id is written once, here. */
#define PSPDX_SELF_ID "io.github.chriopter.pspdxapp"
/* What PSPDX up to 0.5 called itself, from where it was published then. */
#define PSPDX_LEGACY_ID "io.github.chriopter.pspdx"
#define PSPDX_LEGACY_SOURCE "https://github.com/chriopter/pspdx"

enum app_state { APP_UNKNOWN, APP_NOT_INSTALLED, APP_CURRENT, APP_UPDATE };

/* A text an entry owns on the heap, as long as it is and no longer: a
   catalog of two thousand apps is held in a few megabytes only because the
   longest a field may be is not what every entry carries. NULL is the
   empty text. A struct, not a bare pointer, so that no sizeof or strcpy
   meant for the arrays these were can compile against it. */
struct text { char *s; };
static inline const char *txt(struct text t) { return t.s ? t.s : ""; }
/* The text becomes a copy of value; NULL or "" empties it. -1 without
   memory, the text then empty. */
int text_set(struct text *t, const char *value);
/* The same with the first n bytes of value. */
int text_setn(struct text *t, const char *value, size_t n);
int text_setf(struct text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void text_free(struct text *t);

/* What an entry keeps of the release it installs from: the manifest's
   fields but for the id and the repository, which are the entry's, and
   with its texts on the heap. entry_manifest makes the whole manifest of it
   where install.h and state.h want one. */
struct entry_release {
    unsigned rev;                   /* published_at, unix seconds */
    struct text url, version;
    unsigned char sha256[32];
    size_t size;
    /* The .pspdx as it was read, owned by the entry, or NULL. */
    char *raw;
    struct text added_from, checked_from, root;
    unsigned checked_at;
    char dir[64];
    int pinned;
};

struct app_entry {
    char id[PSPDX_ID_SIZE];
    char name[161];
    struct text author, summary, license;
    /* The file's tags, a newline between them: the tabs are made of the ones
       the browser knows, and an app may stand in several. */
    struct text tags;
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
    struct entry_release release;
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
    struct text tag;
    int fresh;
    int media_cached_only;
    struct text repo;
    /* Absolute already: the catalog serves these relative to itself, and
       resolving them once at parse time keeps the base URL in this file. */
    struct text icon, screenshot, video;
    struct text sound;          /* SND0.AT3 out of the EBOOT, the card's own loop */
    enum app_state state;
    unsigned local_rev, remote_rev;
    /* The installed zip's SHA-256 out of its record, when the record has one. */
    unsigned char local_sha256[32];
    int local_has_sha;
    struct text local_version, remote_version;
    /* The line of sources.txt the entry came from, counted from 1; 0 for
       one that came from none this fetch -- an installed app no source
       lists, a file from INBOX, a repository typed since. */
    unsigned char source;
};

/* An entry lets go of what it holds on the heap and is all zeros again. Every
   slot of a catalog is either zeros or an entry that owns its text, so this
   is safe on any of them, whether or not it was ever counted. */
void entry_clear(struct app_entry *entry);
/* dst, cleared first, becomes a copy of src that owns its own text. -1
   without memory, dst then empty. */
int entry_copy(struct app_entry *dst, const struct app_entry *src);
/* The whole manifest of an entry's release, as install.h and state.h take
   it: id and repository the entry's, the .pspdx copied, so that the caller
   lets go of it with manifest_forget. -1 without memory for that copy. */
int entry_manifest(const struct app_entry *entry, struct manifest *out);
/* dst's release becomes src's, which is left empty: its text is dst's now. */
void entry_move_release(struct app_entry *dst, struct app_entry *src);
/* The .pspdx an entry's release is installed from, copied in; whatever it
   held before let go of. -1 without memory, the old one kept. */
int entry_keep_raw(struct app_entry *entry, const char *text, size_t len);
/* The release of an entry made from a manifest, whose .pspdx the entry
   takes over: m holds none afterwards. -1 without memory. */
int entry_take_manifest(struct app_entry *entry, struct manifest *m);

struct catalog {
    /* On the heap, capacity of them: zeros or entries that own their text.
       Grown only while catalog_fetch runs, on the sync thread, when nothing
       else reads the catalog; an app added later, on the main thread, takes
       one of the places kept free for it and never moves the rest. */
    struct app_entry *apps;
    int capacity;
    /* Set while the catalog may grow: during catalog_fetch, and for a probe
       nobody else holds. */
    int grow;
    int count;
    unsigned generated;             /* the oldest source's own stamp, unix seconds; 0 unknown */
    char generated_from[64];        /* that source's host, for the line that names it */
    int total;
    /* What each line of sources.txt is called where the store is browsed
       by source: the name its catalog gives itself, or one made of its URL
       (sources_name); every repository typed into Direct Install is the
       one word T_SOURCE_DIRECT. sources is how many lines there were. */
    char source_name[SOURCES_MAX][CATALOG_NAME_SIZE];
    int sources;
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
/* Every entry let go of, the places with them, and the count. */
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
