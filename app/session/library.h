#ifndef PSPDX_LIBRARY_H
#define PSPDX_LIBRARY_H

#include <stddef.h>

/* The library: what is on the Memory Stick to be started -- and on a PSP Go
   its internal storage too -- rather than what the catalogs publish. The
   UMD images under ISO/ (and a folder deep), the EBOOTs under PSP/GAME (and
   PSP/GAME150, PSP/APPS), each named by its own PARAM.SFO and pictured by
   its own ICON0, sorted into what the XMB would call them: PSP games -- the
   discs and the PSN titles --, PS1 games, and homebrew.

   The stick is read on a thread of its own, once at the first look and
   again when an install or a removal has changed it, never a frame at a
   time; what a title is called is kept in a small file beside the
   catalogs, keyed by the path and the size, so a start reads directories
   and nothing inside the games. The main thread takes a finished reading
   over in library_poll(), and everything else here is the main thread's. */

#define LIBRARY_TITLE 64
#define LIBRARY_PATH 256
#define LIBRARY_MAX 1024            /* more than any stick holds games */
#define LIBRARY_COLS 5              /* icons to a line, as the UMD grid has */

enum library_section { LIBRARY_PSP, LIBRARY_PS1, LIBRARY_APPS, LIBRARY_SECTIONS };

struct library_item {
    char title[LIBRARY_TITLE];
    char path[LIBRARY_PATH];        /* the image, or the EBOOT.PBP */
    unsigned key;                   /* the path's hash: the same item across readings */
    unsigned char kind;             /* enum launch_kind */
    unsigned char section;          /* enum library_section */
};

/* Main thread: read the stick (again). A reading under way finishes first
   and the next one follows it. */
void library_rescan(void);
/* Main thread, once a frame: a finished reading taken over. 1 when the
   items changed, and an index held from before means nothing any more --
   library_find() says where the key it stood for is now. */
int library_poll(void);
/* Whether the stick has been read at all yet, and whether it is being. */
int library_ready(void);
int library_scanning(void);

int library_count(void);
const struct library_item *library_item(int n);
int library_find(unsigned key);
/* A section's items run from its first for its count, in title order. */
int library_section_first(int s);
int library_section_count(int s);

/* One step through the sectioned grid: a line holds LIBRARY_COLS items of
   one section, every section starts a line of its own. Along a line dx
   stops at its ends; up or down goes to the column below or above, onto
   the last item of a shorter line, and off the first or last line round to
   the other end only when ring is set. */
int library_step(int n, int dx, int dy, int ring);
/* The line of the grid an item stands on, counting every section's lines;
   and how many lines there are. */
int library_line(int n);
int library_lines(void);

/* Icon thread: an item's ICON0 PNG into a malloc'd buffer, out of its
   image or its EBOOT. -1 when it has none. */
int library_icon_png(const char *path, int kind, void **png, size_t *len);

/* What an EBOOT's PARAM.SFO CATEGORY makes it: a launch kind, or -1 for
   what is not started from the XMB at all -- a game's update, save data. */
int library_kind_of(const char *category);

/* The reading done here and now, on the calling thread: the host tests. */
void library_scan_now(void);

#endif
