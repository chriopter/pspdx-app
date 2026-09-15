/*
 * PNG in, texture out. libpng does the decoding; the work here is getting the
 * result into the shape the GE insists on: power-of-two dimensions, RGBA in
 * that byte order, 16-byte aligned, and out of the CPU's cache.
 */

#include <pspkernel.h>
#include <malloc.h>
#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "gui/image.h"
#include "util/runtime.h"

/* The largest texture made here. A picture larger than that -- a 640x480
   screenshot -- is read a row at a time and scaled down into one, up to
   MAX_SOURCE_DIM a side: one row of that is 16 KB, and the texture is never
   more than the megabyte a 512x512 one takes. */
#define MAX_DIM 512
#define MAX_SOURCE_DIM 4096

struct source {
    const unsigned char *data;
    size_t len, pos;
};

static void read_from_memory(png_structp png, png_bytep out, png_size_t need) {
    struct source *src = png_get_io_ptr(png);
    if (src->pos + need > src->len) {
        png_error(png, "truncated");
        return;
    }
    memcpy(out, src->data + src->pos, need);
    src->pos += need;
}

static void on_error(png_structp png, png_const_charp msg) {
    logline("png: %s", msg);
    longjmp(png_jmpbuf(png), 1);
}

static void on_warning(png_structp png, png_const_charp msg) {
    (void)png;
    (void)msg;
}

static unsigned next_pow2(unsigned v) {
    unsigned p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* One row of the texture out of the source pixels gathered for it: each
   texel the average of the pixels that fell in it, the colour weighted by
   alpha so a transparent pixel does not grey an edge. sum holds five
   counts a texel -- red, green and blue times alpha, alpha, pixels -- and
   is emptied for the next row. */
static void shrink_row(unsigned *sum, unsigned width, unsigned char *out) {
    for (unsigned x = 0; x < width; x++, sum += 5, out += 4) {
        unsigned alpha = sum[3], pixels = sum[4];
        if (!pixels)
            continue;
        for (int c = 0; c < 3; c++)
            out[c] = alpha ? (unsigned char)((sum[c] + alpha / 2) / alpha) : 0;
        out[3] = (unsigned char)((alpha + pixels / 2) / pixels);
        memset(sum, 0, 5 * sizeof(*sum));
    }
}

int image_decode_png(const void *data, size_t len, struct gfx_texture *out) {
    struct source src = { data, len, 0 };
    /* Set after setjmp and read by its error path: volatile, or a longjmp
       hands back what they held at the setjmp and the memory leaks. */
    png_bytep *volatile rows = 0;
    png_bytep volatile row = 0;
    unsigned *volatile sum = 0;

    memset(out, 0, sizeof(*out));
    if (len < 8 || png_sig_cmp((png_const_bytep)data, 0, 8)) {
        logline("png: not a png");
        return -1;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0,
                                             on_error, on_warning);
    if (!png) return -1;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, 0, 0);
        return -1;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, 0);
        free(rows);
        free(row);
        free(sum);
        gfx_texture_free(out);
        return -1;
    }

    png_set_read_fn(png, &src, read_from_memory);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    if (w > MAX_SOURCE_DIM || h > MAX_SOURCE_DIM || w == 0 || h == 0) {
        logline("png: %ux%u is larger than a picture is read at", (unsigned)w, (unsigned)h);
        png_destroy_read_struct(&png, &info, 0);
        return -1;
    }
    /* What the texture holds: the picture itself, or the picture scaled
       down, its sides kept in proportion, to fit MAX_DIM. */
    unsigned tw = w, th = h;
    int shrink = w > MAX_DIM || h > MAX_DIM;
    if (shrink) {
        /* An interlaced picture arrives in passes over the whole of it, which
           a row at a time cannot scale; none a catalog serves is one. */
        if (png_get_interlace_type(png, info) != PNG_INTERLACE_NONE) {
            logline("png: an interlaced %ux%u is too large to scale down", (unsigned)w,
                    (unsigned)h);
            png_destroy_read_struct(&png, &info, 0);
            return -1;
        }
        if (w >= h) {
            tw = MAX_DIM;
            th = (unsigned)(((unsigned long long)h * MAX_DIM + w / 2) / w);
        } else {
            th = MAX_DIM;
            tw = (unsigned)(((unsigned long long)w * MAX_DIM + h / 2) / h);
        }
        if (!tw) tw = 1;
        if (!th) th = 1;
    }

    /* Whatever the file says, hand back 8-bit RGBA: palettes expanded, low
       bit depths widened, 16-bit narrowed, and an opaque alpha added when
       the image has none. */
    int type = png_get_color_type(png, info);
    int depth = png_get_bit_depth(png, info);
    if (type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    int alpha = (type & PNG_COLOR_MASK_ALPHA) ||
                png_get_valid(png, info, PNG_INFO_tRNS);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (depth == 16) png_set_strip_16(png);
    if (!alpha) png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    if (!shrink) png_set_interlace_handling(png);
    png_read_update_info(png, info);

    if (png_get_channels(png, info) != 4) {
        logline("png: %d channels after expansion, expected 4",
                png_get_channels(png, info));
        png_destroy_read_struct(&png, &info, 0);
        return -1;
    }

    out->w = (int)tw;
    out->h = (int)th;
    out->tw = (int)next_pow2(tw);
    out->th = (int)next_pow2(th);
    /* The GE reads this behind the cache, so it has to be aligned and later
       written back by hand. */
    out->pixels = memalign(16, (size_t)out->tw * out->th * 4);
    if (!out->pixels) {
        logline("png: no room for a %dx%d texture", out->tw, out->th);
        png_destroy_read_struct(&png, &info, 0);
        return -1;
    }
    memset(out->pixels, 0, (size_t)out->tw * out->th * 4);

    if (!shrink) {
        rows = malloc(sizeof(png_bytep) * h);
        if (!rows) {
            png_destroy_read_struct(&png, &info, 0);
            gfx_texture_free(out);
            return -1;
        }
        for (png_uint_32 y = 0; y < h; y++)
            rows[y] = (png_bytep)out->pixels + (size_t)y * out->tw * 4;
        png_read_image(png, rows);
    } else {
        row = malloc((size_t)w * 4);
        sum = calloc((size_t)tw * 5, sizeof(*sum));
        if (!row || !sum) {
            png_destroy_read_struct(&png, &info, 0);
            free(row);
            free(sum);
            gfx_texture_free(out);
            return -1;
        }
        /* Each source row and pixel lands in the texel its position scales
           to; a row of texels is written once the source has moved past it. */
        unsigned at = 0;
        for (png_uint_32 y = 0; y < h; y++) {
            png_read_row(png, row, NULL);
            unsigned ty = (unsigned)((unsigned long long)y * th / h);
            if (ty != at) {
                shrink_row(sum, tw, (unsigned char *)out->pixels + (size_t)at * out->tw * 4);
                at = ty;
            }
            const unsigned char *p = row;
            for (png_uint_32 x = 0; x < w; x++, p += 4) {
                unsigned *s = sum + (unsigned)((unsigned long long)x * tw / w) * 5;
                s[0] += (unsigned)p[0] * p[3];
                s[1] += (unsigned)p[1] * p[3];
                s[2] += (unsigned)p[2] * p[3];
                s[3] += p[3];
                s[4]++;
            }
        }
        shrink_row(sum, tw, (unsigned char *)out->pixels + (size_t)at * out->tw * 4);
        logline("png: %ux%u scaled down to %ux%u", (unsigned)w, (unsigned)h, tw, th);
    }
    png_read_end(png, 0);
    png_destroy_read_struct(&png, &info, 0);
    free(rows);
    free(row);
    free(sum);

    sceKernelDcacheWritebackRange(out->pixels, (unsigned)out->tw * out->th * 4);
    /* Icons come by the dozen and the log ring is forty lines: only a
       picture worth a line gets one. */
    if (out->tw > 256)
        logline("png: %dx%d in a %dx%d texture, %d KB", out->w, out->h, out->tw,
                out->th, out->tw * out->th * 4 / 1024);
    return 0;
}
