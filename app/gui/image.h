#ifndef PSPDX_IMAGE_H
#define PSPDX_IMAGE_H

#include <stddef.h>

#include "gui/gfx.h"

/* Decodes a PNG held in memory into a texture the GE can sample. Anything the
   catalog serves fits: at most 480x272, which lands in a 512x512 texture.

   Returns 0 on success and fills out; on failure out is left zeroed and the
   reason is in the log. The caller owns the pixels and frees them with
   gfx_texture_free(). */
int image_decode_png(const void *data, size_t len, struct gfx_texture *out);
/* The same at up to 512 a side, as swizzled 5650: for a page's backdrop. */
int image_decode_png_screen(const void *data, size_t len, struct gfx_texture *out);

#endif
