#ifndef PSPDX_FLAGS_H
#define PSPDX_FLAGS_H

/* The flag of the region a UMD was sold in, as the PSP's own font has none:
   EU, US, JP and KR drawn in rectangles at FLAG_W x FLAG_H, one to one, and
   Asia as the globe mark. x, y the top left in whole pixels; alpha fades
   the whole flag. Anything else is its code in letters. */
#define FLAG_W 16
#define FLAG_H 11
void flag_draw(const char *region, int x, int y, int alpha);

#endif
