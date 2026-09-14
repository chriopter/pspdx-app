#ifndef PSPDX_PBP_H
#define PSPDX_PBP_H

#include <stddef.h>

/* An EBOOT.PBP is Sony's bundle: a header of eight offsets, then the
   sections in order. What is on the stick already carries the pictures
   the catalog serves -- the same files, out of the same bundle -- so an
   installed app is pictured from its own EBOOT, and needs no cache and no
   network for it. Only the section asked for is read; the program after
   them is never touched. */

enum pbp_section {
    PBP_SFO,            /* PARAM.SFO: the title among other things */
    PBP_ICON0,          /* PNG, 144x80, the XMB's and the list's icon */
    PBP_ICON1,          /* PMF: a PSMF the player takes as it is */
    PBP_PIC0,           /* PNG, 310x180, seldom present */
    PBP_PIC1,           /* PNG, 480x272, the card's still */
    PBP_SND0            /* AT3: the loop under the card */
};

/* One section into a malloc'd buffer the caller frees. Returns -1 when the
   file is not a PBP, the section is absent, or it is larger than a section
   of its kind has any business being. */
int pbp_section(const char *path, int which, void **out, size_t *len);

/* The EBOOT of an installed package, from the record on the stick that
   says which directory the install went into. -1 when there is no record.
   Here because everything that pictures an installed row asks the same
   question first. */
int pbp_installed_path(const char *id, char *out, size_t size);

#endif
