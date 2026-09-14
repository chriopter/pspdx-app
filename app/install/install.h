#ifndef PSPDX_INSTALL_H
#define PSPDX_INSTALL_H

#include <stddef.h>
#include "pspkit-https/https.h"

/* One release, as the console installs it: the fields that go into a
   download and an unpack, and the repository it came from, which goes
   into the record on the stick so the package can be found at its source
   again. A sha256 of all zeros means nobody has hashed the zip: the origin
   path gives none, and the size is what is checked then. */
struct manifest {
    char id[96];
    unsigned rev;               /* the release's published_at, unix seconds */
    char url[512];
    unsigned char sha256[32];
    size_t size;
    char version[32];
    char repo[256];
    /* What the .pspdx said about the shape of the zip, straight from the
       file or from a cache entry's "install", and empty when it said
       nothing: root is the directory inside the zip that is the package,
       dir the name it takes under PSP/GAME. Only a zip that is not one
       directory with the EBOOT in it needs either. */
    char raw[8193];
    char added_from[256], checked_from[256];
    unsigned checked_at;
    char root[200];
    char dir[64];
};

struct install_report {
    char id[96];
    char dir[64];               /* PSP/GAME/<dir> actually written */
    char version[32];
    unsigned rev;
    int files;
    size_t bytes;
};

/* What PSP/PSPDX/db/<id>.json remembers about an installed package. */
struct installed {
    char id[96];
    char dir[64];
    char version[32];
    unsigned rev;
    char repo[256];
};

int db_read(const char *id, struct installed *out);

/* A record for a package this client did not unpack. There is exactly one --
   PSPDX's own, which is on the stick because somebody copied it there, and
   which still needs a record to be an app like the others. Written the same
   way an install writes one, through a rename, so it is never half a file. */
int db_write_record(const struct installed *record);

/* Removes PSP/GAME/<dir> and forgets the record, in that order: a directory
   left behind with no record would be offered as uninstalled and written over,
   while a record with no directory only costs one line in the database. The
   directory comes from the record and is refused unless it is a plain name --
   nothing here may be talked into deleting a path of someone else's choosing.
   Returns 0 when the package is gone. */
int uninstall(const char *id);

typedef void (*install_phase_cb)(void *ctx, const char *phase);

/* The rules a release's fields are held to by whoever reads them out of
   JSON -- a cache's or GitHub's -- since they go straight into a download
   and an unpack. An id is a path component on the stick: letters, digits,
   dot, dash and underscore, at most eighty of them, no "..". A package is
   at most a gigabyte, and a revision fits an unsigned. */
#define MAX_PACKAGE_BYTES (1024u * 1024u * 1024u)
int manifest_id_is_safe(const char *id);
int manifest_rev_in_range(double rev);
int manifest_size_in_range(double size);
int manifest_has_sha256(const struct manifest *m);

/* A directory under PSP/GAME, wherever the name came from -- a record on
   the stick, a .pspdx, a cache entry: a plain name and nothing else, since
   it is joined to a path that gets written into and, on uninstall,
   recursively deleted. */
int manifest_dir_is_safe(const char *dir);

/* Finishes an install interrupted between its two renames. Call once at
   startup, before anything reads the database. */
void install_recover(void);

/* The user's way out of a transaction recovery could not finish: the
   journal, the archive and the staging directory go, whatever is under
   PSP/GAME stays and is named in line. Returns 0 when the journal is gone. */
int install_discard(char *line, size_t size);

/* What install_release returns when it was called off. */
#define INSTALL_CANCELLED (-9)

/* Calls the install in progress off, from its own progress callback: the
   download or the unpack stops at its next piece and the stick is put back
   the way it was. Past the unpack the rename is moments away and cannot be
   left half done, so it goes through. */
void install_abort(void);

/* Download, verify, unpack, rename into place. Returns 0 on success;
   negative on the phase that failed. Nothing is left half-written. */
int install_release(const struct manifest *release, struct install_report *rep,
                    install_phase_cb phase, https_progress progress, void *pctx);

#endif
