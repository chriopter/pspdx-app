#include "update/gunzip.h"

#include <string.h>
#include <strings.h>

enum { UNDECIDED, PLAIN, PACKED, ENDED };

void gunzip_begin(struct gunzip *g) {
    if (g->live)
        inflateEnd(&g->z);
    memset(g, 0, sizeof(*g));
}

static int refuse(struct gunzip *g, const char *why) {
    g->refused = 1;
    g->why = why;
    return GUNZIP_CORRUPT;
}

static int copy(const unsigned char *data, size_t len, char *out, size_t room, size_t *out_len) {
    if (len > room - *out_len)
        return GUNZIP_FULL;
    memcpy(out + *out_len, data, len);
    *out_len += len;
    return GUNZIP_OK;
}

/* Inflates what came into what is left of out. A full buffer does not yet
   mean too much text: the stream may be at its end with only the trailer
   still to come. So inflate is offered one spare byte, and only a byte
   written there is text that does not fit. */
static int pour(struct gunzip *g, const unsigned char *data, size_t len, char *out, size_t room,
                size_t *out_len) {
    g->z.next_in = (Bytef *)data;
    g->z.avail_in = (uInt)len;
    while (g->z.avail_in) {
        if (g->mode == ENDED)
            return refuse(g, "data after the end of the gzip");
        unsigned char spare;
        int full = *out_len >= room;
        g->z.next_out = full ? &spare : (Bytef *)out + *out_len;
        g->z.avail_out = full ? 1 : (uInt)(room - *out_len);
        uInt before = g->z.avail_out;
        int rc = inflate(&g->z, Z_NO_FLUSH);
        size_t made = before - g->z.avail_out;
        if (full && made)
            return GUNZIP_FULL;
        if (!full)
            *out_len += made;
        if (rc == Z_STREAM_END)
            g->mode = ENDED;
        else if (rc != Z_OK)
            return refuse(g, "corrupt gzip");
    }
    return GUNZIP_OK;
}

int gunzip_feed(struct gunzip *g, const void *data, size_t len, char *out, size_t room,
                size_t *out_len) {
    const unsigned char *p = data;
    g->wire += len;
    if (g->mode == UNDECIDED) {
        while (len && g->lead_len < 2) {
            g->lead[g->lead_len++] = *p++;
            len--;
        }
        /* Nothing yet, or one byte that may start gzip: the next piece says. */
        if (!g->lead_len || (g->lead[0] == 0x1f && g->lead_len < 2))
            return GUNZIP_OK;
        int rc;
        if (g->lead[0] == 0x1f && g->lead[1] == 0x8b) {
            /* 16 on top of the window size: a gzip wrapper, header and CRC,
               rather than zlib's or none as the ZIP reader has. */
            if (inflateInit2(&g->z, 16 + MAX_WBITS) != Z_OK)
                return refuse(g, "no memory to inflate");
            g->live = 1;
            g->mode = PACKED;
            rc = pour(g, g->lead, g->lead_len, out, room, out_len);
        } else {
            g->mode = PLAIN;
            rc = copy(g->lead, g->lead_len, out, room, out_len);
        }
        if (rc != GUNZIP_OK)
            return rc;
    }
    if (!len)
        return GUNZIP_OK;
    if (g->mode == PLAIN)
        return copy(p, len, out, room, out_len);
    return pour(g, p, len, out, room, out_len);
}

const char *gunzip_end(struct gunzip *g, const char *content_encoding) {
    const char *named = content_encoding ? content_encoding : "";
    int gzip = !strcasecmp(named, "gzip") || !strcasecmp(named, "x-gzip");
    if (g->live) {
        inflateEnd(&g->z);
        g->live = 0;
    }
    if (g->why)
        return g->why;
    /* A body is only taken as the header describes it: gzip nobody announced
       is not a catalog, and neither is an encoding this cannot undo. */
    if (g->mode == UNDECIDED && g->lead_len)
        g->why = "corrupt gzip";
    else if (g->mode == PACKED)
        g->why = "gzip cut short";
    else if (gzip && g->mode != ENDED)
        g->why = "announced gzip and sent something else";
    else if (!gzip && g->mode == ENDED)
        g->why = "gzip that no header announced";
    else if (!gzip && *named && strcasecmp(named, "identity"))
        g->why = "an encoding other than gzip";
    return g->why;
}

int gunzip_packed(const struct gunzip *g) { return g->mode == PACKED || g->mode == ENDED; }
