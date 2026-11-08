#ifndef PSPDX_SFO_H
#define PSPDX_SFO_H

#include <stddef.h>

/* A PARAM.SFO is the small table every PSP title carries -- in an EBOOT's
   first section, at PSP_GAME/PARAM.SFO on a disc -- of keys and their
   values: TITLE, what the XMB names it; CATEGORY, what kind of thing it is;
   DISC_ID and the rest. Only strings are read here; that is all the library
   asks of one. */

/* The string under key into out, cut to size and always terminated. 0 when
   it is there, -1 when the table is not an SFO or has no such string. */
int sfo_string(const void *sfo, size_t len, const char *key, char *out, size_t size);

#endif
