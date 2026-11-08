#ifndef PSPDX_ISO_H
#define PSPDX_ISO_H

#include <stddef.h>

/* A UMD image on the stick, read for the two small files the library wants
   out of it: PSP_GAME/PARAM.SFO for its title and PSP_GAME/ICON0.PNG for its
   picture. The image is a plain ISO 9660 one (.iso), or one of the two
   compressed kinds the custom firmwares run -- CSO, its blocks deflated, and
   ZSO, the same layout with LZ4 -- and only the sectors on the way to the
   file are read: the volume descriptor, the directories, the file itself.
   Never the rest of a gigabyte. */

/* The file at inner ("PSP_GAME/PARAM.SFO") inside the image at path, into
   a malloc'd buffer the caller frees. -1 when the image cannot be read,
   the file is not in it, or it is larger than cap. */
int iso_file(const char *path, const char *inner, size_t cap, void **out, size_t *len);

/* Whether a name ends in one of the image kinds iso_file reads. */
int iso_name(const char *name);

#endif
