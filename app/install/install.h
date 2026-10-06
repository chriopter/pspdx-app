#ifndef PSPDX_INSTALL_H
#define PSPDX_INSTALL_H

#include <stddef.h>
#include "pspkit-https/https.h"
#include "update/pspdx.h"

/* A release tag is at most 64 characters, and a character at most four
   bytes: what a version is kept in, the tag with its "v" taken off. */
#define VERSION_SIZE (64 * 4 + 1)

/* One release, as the console installs it: the fields that go into a
   download and an unpack, and the repository it came from, which goes
   into the record on the stick so the package can be found at its source
   again. A sha256 of all zeros means nobody has hashed the zip: the origin
   path gives none, and the size is what is checked then. */
struct manifest {
    char id[PSPDX_ID_SIZE];
    unsigned rev;               /* the release's published_at, unix seconds */
    char url[513];              /* the 512 characters of an address the catalog allows */
    unsigned char sha256[32];
    size_t size;
    char version[VERSION_SIZE];
    char repo[PSPDX_URL_SIZE];
    /* The .pspdx as it was read, on the heap and owned by this manifest, or
       NULL until it has been: a file is up to 16 KB and most entries of a
       catalog never have one read. A copy of the struct shares the pointer,
       so whoever copies one hands the text over with manifest_forget on the
       other, or clears it, and never frees it twice. */
    char *raw;
    /* What the .pspdx said about the shape of the zip, straight from the
       file or from a cache entry's "install", and empty when it said
       nothing: root is the directory inside the zip that is the package,
       dir the name it takes under PSP/GAME. Only a zip that is not one
       directory with the EBOOT in it needs either. */
    char added_from[256], checked_from[256];
    unsigned checked_at;
    char root[200];
    char dir[64];
    /* The release came from the app's repository by the tag its .pspdx pins:
       no newer one is looked for there, and only a catalog's entry, by
       another zip, can say there is an update. Kept in the record's latest,
       so a check that does not ask the repository knows it too. */
    int pinned;
};

struct install_report {
    char id[PSPDX_ID_SIZE];
    char dir[64];               /* PSP/GAME/<dir> actually written */
    /* seplugins/<plugin> written instead, for a plugin, and whether it is
       turned off afterwards, as a plugin is that no line of PLUGINS.TXT
       turns on: an install writes no line. */
    char plugin[40];
    int plugin_off;
    char version[VERSION_SIZE];
    unsigned rev;
    int files;
    size_t bytes;
    /* What the install wanted free on the stick, when that is why it
       stopped: INSTALL_NO_SPACE. */
    unsigned long long needed;
    /* Why it stopped, in a few words for the status line, when a download
       or the package is the reason; empty otherwise. */
    char why[64];
};

/* What PSP/PSPDX/db/<id>.json remembers about an installed package. */
struct installed {
    char device[5];
    char id[PSPDX_ID_SIZE];
    char dir[64];
    char version[VERSION_SIZE];
    unsigned rev;
    char repo[PSPDX_URL_SIZE];
    /* The zip that was installed, by its SHA-256, which is what an update is
       told by; all zeros for a record written before hashes were kept. */
    unsigned char sha256[32];
    /* The folder the .pspdx named when the app went in, which may not be
       the one it is in: the app keeps its folder, and only the author's
       naming another one moves it. Empty for a record from before this was
       kept. */
    char file_dir[64];
    /* A plugin is a file, not a folder: seplugins/<plugin> on the device,
       and dir empty. Empty for everything else. */
    char plugin[40];
    /* The .prx as it was installed, by its SHA-256: a file of that name
       with other bytes is not PSPDX's and is never written or removed. */
    unsigned char plugin_sha256[32];
    /* The path of the line PSPDX added to PLUGINS.TXT when the plugin was
       turned on, <device>/seplugins/<plugin>; empty while it added none.
       No other line of the list is ever written. */
    char plugin_line[64];
    /* A write to PLUGINS.TXT that was begun and not yet seen to be there:
       len the list's length before it, now the bytes meant for at, was the
       bytes there before (none for a line put after the end), then the
       plugin_line the record has once it is done. What a power cut in the
       middle of the write is finished from. */
    struct plugin_write {
        int pending;
        size_t at, len;
        char was[96], now[96], then[64];
    } plugin_write;
};

int db_read(const char *id, struct installed *out);

/* A record for a package this client did not unpack. There is exactly one --
   PSPDX's own, which is on the stick because somebody copied it there, and
   which still needs a record to be an app like the others. Written the same
   way an install writes one, through a rename, so it is never half a file. */
int db_write_record(const struct installed *record);

/* A plugin's file under seplugins/, wherever the name came from -- a zip, a
   record on the stick, a journal: 5 to 32 of [A-Za-z0-9_.-] that end in
   .prx and do not begin with a dot. It is joined to a path and written
   into a line of PLUGINS.TXT, where a comma or a space would part it. */
int manifest_plugin_is_safe(const char *name);

/* Whether an installed plugin is turned on, as ARK-4 reads the
   seplugins/PLUGINS.TXT of its device: by the last line that names it. 1
   on, 0 off -- that line says off, or none names it -- and -1 for an id
   that is no installed plugin. */
int plugin_enabled(const char *id);
/* Turns an installed plugin on or off, by PSPDX's own line and no other:
   the line "always, <path>, on" put after the list's end the first time,
   and from then on the on or off of that line written over itself, the
   list as long as it was. A line somebody else wrote for the plugin is
   never changed, so the plugin may not be what was asked for afterwards:
   returns what plugin_enabled now would for the caller to hold against it,
   or -1 when the list was not written, with the reason in
   plugin_refused(). The list is read when the PSP starts, so that is when
   it takes effect. */
int plugin_switch(const char *id, int on);
/* Why the last plugin_switch or uninstall stopped, for the status line;
   empty when it gave no reason. */
const char *plugin_refused(void);

/* Removes PSP/GAME/<dir> and forgets the record, in that order: a directory
   left behind with no record would be offered as uninstalled and written over,
   while a record with no directory only costs one line in the database. The
   directory comes from the record and is refused unless it is a plain name --
   nothing here may be talked into deleting a path of someone else's choosing.
   A plugin loses the line PSPDX added to PLUGINS.TXT, spaces written over
   it, and its one file. Where that file is no longer the one PSPDX
   installed it is somebody's own build: file and line both stay, so that it
   goes on loading, and only the record goes. Nothing else under seplugins/
   is touched. Returns 0 when the package is gone,
   for a plugin with what was left of it that is not PSPDX's. */
#define UNINSTALL_LINES 1       /* lines of PLUGINS.TXT that name it */
#define UNINSTALL_FILE 2        /* a file of its name that PSPDX did not install */
int uninstall(const char *id);

typedef void (*install_phase_cb)(void *ctx, const char *phase);

/* The rules a release's fields are held to by whoever reads them out of
   JSON -- a cache's or GitHub's -- since they go straight into a download
   and an unpack. An id is a path component on the stick: letters, digits,
   dot, dash and underscore, at most PSPDX_ID_SIZE - 1 of them, no "..". A package is
   at most a gigabyte, and a revision fits an unsigned. */
#define MAX_PACKAGE_BYTES (1024u * 1024u * 1024u)
int manifest_id_is_safe(const char *id);
int manifest_rev_in_range(double rev);
int manifest_size_in_range(double size);
int manifest_has_sha256(const struct manifest *m);

/* The text of the .pspdx given to the manifest, copied onto the heap, and
   whatever it held before let go of. Returns 0, or -1 without memory, when
   the manifest holds nothing. */
int manifest_keep_raw(struct manifest *m, const char *text, size_t len);
/* The text let go of: the manifest holds none afterwards. */
void manifest_forget(struct manifest *m);

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

/* A zip with more than one EBOOT.PBP -- mostly one built for today's
   firmware at the top and the 1.50 kernel's launchers (X%, __SCE__X, 150/)
   in folders under it -- installs the folder of the one nearest the top,
   whole, when no other is as near. The installer is then going by a rule
   rather than by what the zip plainly is, so it says what it would do and
   asks before any file is written. */
#define LAYOUT_SHOWN 3          /* paths named per list; the rest are counted */
#define LAYOUT_GROUPS 16        /* groups told apart; past that they count as one more each file */
struct install_layout {
    char root[200];             /* the folder in the zip that is installed, "" for its top */
    char dir[64];               /* PSP/GAME/<dir> it goes to */
    int eboots;                 /* EBOOT.PBP in the whole zip */
    unsigned files;             /* files that go in, and their size unpacked */
    unsigned long long bytes;
    /* EBOOT.PBP under root besides the package's own: copied along, never
       started. Paths below root. */
    int nested;
    char nested_path[LAYOUT_SHOWN][64];
    /* What is outside root and so not installed, by the first folder (or
       file) of it that root is not in: EBOOT.PBP or not, it is lost. */
    int left;                   /* EBOOT.PBP among it */
    int left_groups;
    char left_path[LAYOUT_GROUPS][64];
    unsigned left_count[LAYOUT_GROUPS];
    unsigned left_files;
    unsigned long long left_bytes;
    unsigned mac_files;         /* __MACOSX shadows, never read */
    /* Files that go in under a name the zip stored in a code page, turned
       into UTF-8 -- the app may still look for the old bytes. */
    int renamed;
    char renamed_path[2][64];
    int sjis;                   /* of those, read as Shift-JIS rather than CP437 */
    int review;                 /* something here is worth a look before installing */
};
/* 0 goes ahead, anything else calls the install off. With no check set, a
   layout the rule decides goes ahead. */
typedef int (*install_layout_cb)(void *ctx, const struct install_layout *layout);
void install_set_layout_check(install_layout_cb check);

/* What install_release returns when it was called off. */
#define INSTALL_CANCELLED (-9)
/* The Memory Stick has less room than the package needs; the report says
   how much it wanted. */
#define INSTALL_NO_SPACE (-10)
/* What uninstall returns for the folder PSPDX runs from, whatever record
   names it. */
#define INSTALL_SELF (-11)
/* The layout a rule chose was shown and turned down. */
#define INSTALL_DECLINED (-12)


/* PSPDX up to 0.5 kept a record of itself as io.github.chriopter.pspdx, from
   the repository that is the standard alone now. Such a record for the folder
   the client runs from is the client, and goes with its saved file, before
   anything reads the records. Nothing while a transaction is unfinished.
   1 retired, 0 none, -1 failed. */
int install_retire_legacy(void);

/* Calls the install in progress off, from its own progress callback: the
   download or the unpack stops at its next piece and the stick is put back
   the way it was. Past the unpack the rename is moments away and cannot be
   left half done, so it goes through. */
void install_abort(void);

/* Download, verify, unpack, rename into place. Returns 0 on success;
   negative on the phase that failed. Nothing is left half-written. A
   homebrew goes to PSP/GAME/<dir>; a plugin's one .prx to seplugins/,
   turned off until plugin_switch turns it on. */
int install_release_to(const struct manifest *release, const char *device,
                       struct install_report *rep, install_phase_cb phase,
                       https_progress progress, void *pctx);

/* The first install of a plugin whose .prx is not fetched but there
   already, at prx: the copy of pspkit-usbnet the release carries beside the
   EBOOT. Through the same installer as a download, with every check and
   every refusal of it, and the same record: release says which release the
   file is of -- its id, repository, version, the SHA-256 of that release's
   zip, by which an update is told, and the .pspdx -- so that the store
   updates the plugin from then on. Installed turned off, like any. */
int install_bundled(const struct manifest *release, const char *prx, const char *device,
                    struct install_report *rep);

int install_release(const struct manifest *release, struct install_report *rep,
                    install_phase_cb phase, https_progress progress, void *pctx);

#endif
