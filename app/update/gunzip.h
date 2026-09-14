#ifndef PSPDX_UPDATE_GUNZIP_H
#define PSPDX_UPDATE_GUNZIP_H

#include <stddef.h>
#include <zlib.h>

/* A text body into a buffer as it arrives, inflated on the way when it came
   gzipped. The sink cannot see the response's headers -- they reach the
   caller with the result, after the body -- so the body tells: gzip starts
   with 1f 8b, and no JSON or list does. What the header named is held
   against what the body said once the transfer is over. */

enum { GUNZIP_OK = 0, GUNZIP_CORRUPT = -1, GUNZIP_FULL = -2 };

struct gunzip {
    z_stream z;
    int mode;               /* undecided, plain, gzip, gzip ended */
    int live;               /* z holds zlib's state, to be released */
    int refused;            /* a feed returned GUNZIP_CORRUPT */
    unsigned char lead[2];  /* the first bytes, until they have said which */
    size_t lead_len;
    size_t wire;            /* bytes as they arrived */
    const char *why;
};

/* Before the first feed of every transfer. */
void gunzip_begin(struct gunzip *g);

/* One piece of the body, appended at out[*out_len], which never grows past
   room. GUNZIP_FULL when the plain or inflated text would, GUNZIP_CORRUPT
   when the gzip is broken; a sink returning either ends the transfer. */
int gunzip_feed(struct gunzip *g, const void *data, size_t len, char *out, size_t room,
                size_t *out_len);

/* After the transfer, whatever became of it: releases zlib and returns NULL
   when the body was whole and what the Content-Encoding header named ("" or
   NULL for none), or a static line saying what was wrong. */
const char *gunzip_end(struct gunzip *g, const char *content_encoding);

/* Whether the body came gzipped; wire is then its size on the wire. */
int gunzip_packed(const struct gunzip *g);

#endif
