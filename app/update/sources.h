#ifndef PSPDX_SOURCES_H
#define PSPDX_SOURCES_H

#include <stddef.h>

/* Where the console finds apps: PSP/PSPDX/sources.txt, one URL a line, the
   built-in list first. A line is one of three things -- a list (text with an
   optional "cache <url>" line and one GitHub repository a line), a single
   repository, which is a list of one, or a catalog.json, which is a cache
   with no list behind it. The file is read at every catalog fetch and written
   to by the gear tab; nothing else touches it.

   What a repository is called, what it is, and which zip on the release is
   the package are the repository's own .pspdx, not the list's business:
   see update/pspdx.h. */

#define SOURCES_MAX 16
#define SOURCE_URL 256
#define SOURCES_DEFAULT "https://raw.githubusercontent.com/chriopter/pspdx-catalog/HEAD/repos.txt"

struct sources {
    char url[SOURCES_MAX][SOURCE_URL];
    int count;
};

/* Reads the file, writing it first with the built-in list when it is
   missing. Returns how many sources there are. */
int sources_load(struct sources *s);

/* What the user typed, made into a source: "owner/repo" becomes the
   repository's URL, a bare github.com address gets its scheme, a full URL
   is taken as it is. Appends it to the file unless it is there already.
   Returns 1 added, 0 already present, -1 for something that is not a URL.
   url receives the normalised form either way. */
/* Takes one URL out of the file. Returns 1 removed, 0 not there, -1 for a
   file that could not be written. */
int sources_remove(const char *url);
int sources_release_url(const char *repo,const char *url);
int sources_normalize(const char *text,char *url,size_t size);
int sources_add(const char *text, char *url, size_t size);

enum source_kind { SOURCE_LIST, SOURCE_REPO, SOURCE_CATALOG };
enum source_kind sources_kind(const char *url);

/* One line of a list: https://github.com/<owner>/<repo>[@tag], and nothing
   else. The category, the title, the summary and the rest used to be words
   after the URL; they are the repository's own .pspdx now, so a list says
   only where the apps are and every list shows the same app the same way. */
struct source_repo {
    char owner[40];
    char name[100];
    char ref[40];               /* the tag, or HEAD when none is pinned */
};

#define LIST_REPOS 64
struct source_list {
    char cache[SOURCE_URL];     /* the "cache" line, empty when there is none */
    struct source_repo repo[LIST_REPOS];
    int count;
};

/* The list's text into its parts; lines that name nothing this understands
   are passed over. Returns the number of repositories. */
int sources_parse_list(const char *text, struct source_list *out);

/* A repository URL into its parts. Returns 0 when it is not one. */
int sources_parse_repo(const char *url, struct source_repo *out);

/* Two URLs that differ only by case or a trailing slash name the same
   place. */
int sources_same_url(const char *a, const char *b);

/* The id the repository derives to: io.github.<owner>.<repo>, lower case,
   [a-z0-9] only, so that "Chris-Opter/PSP-Thing" owns io.github.chrisopter.pspthing.
   It names a directory on the stick and never changes. */
void sources_repo_id(const struct source_repo *r, char *id, size_t size);

/* The repository's canonical URL, https://github.com/<owner>/<repo>, with
   no tag on it: what the record on the stick and the cache both call the
   repository, so the two can be compared. */
void sources_repo_url(const struct source_repo *r, char *url, size_t size);

#endif
