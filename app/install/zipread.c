/*
 * The smallest ZIP reader that installs a homebrew bundle: central directory,
 * stored and deflated entries, nothing else. Written against zlib alone
 * because pspdev's libzip drags in mbedTLS and its minizip is missing half
 * its symbols. A package format that needs more than this is not one the PSP
 * should be handed.
 */

#include <pspiofilemgr.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

#include "util/runtime.h"
#include "install/zipread.h"

#define SIG_EOCD    0x06054b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_LOCAL   0x04034b50u

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_at(int fd, uint32_t off, void *buf, size_t len) {
    if (sceIoLseek32(fd, (int)off, PSP_SEEK_SET) != (int)off) return -1;
    size_t got = 0;
    while (got < len) {
        int n = sceIoRead(fd, (char *)buf + got, len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

int zip_open(struct zipread *z, const char *path) {
    memset(z, 0, sizeof(*z));
    z->fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (z->fd < 0) return -1;
    int size = sceIoLseek32(z->fd, 0, PSP_SEEK_END);
    if (size < 22) { zip_close(z); return -1; }

    /* The end-of-central-directory record is within the last 64 KB + 22. */
    static unsigned char tail[64 * 1024 + 22];
    uint32_t tlen = (uint32_t)size < sizeof(tail) ? (uint32_t)size : (uint32_t)sizeof(tail);
    uint32_t toff = (uint32_t)size - tlen;
    if (read_at(z->fd, toff, tail, tlen) < 0) { zip_close(z); return -1; }

    int found = -1;
    for (int i = (int)tlen - 22; i >= 0; i--) {
        if (rd32(tail + i) == SIG_EOCD) { found = i; break; }
    }
    if (found < 0) { logline("zip: no end record"); zip_close(z); return -1; }

    const unsigned char *e = tail + found;
    z->entries = rd16(e + 10);
    z->cd_size = rd32(e + 12);
    z->cd_off = rd32(e + 16);
    if (z->entries == 0xffff || z->cd_off == 0xffffffffu) {
        logline("zip: zip64 not supported");
        zip_close(z);
        return -1;
    }
    return 0;
}

void zip_close(struct zipread *z) {
    if (z->fd >= 0) sceIoClose(z->fd);
    z->fd = -1;
}

int zip_first(struct zipread *z, struct zipentry *e) {
    z->cd_pos = z->cd_off;
    z->index = 0;
    return zip_next(z, e);
}

/* Reads one central directory record into e and advances. 1 = got one,
   0 = end, <0 = corrupt. */
int zip_next(struct zipread *z, struct zipentry *e) {
    if (z->index >= z->entries) return 0;
    unsigned char h[46];
    if (read_at(z->fd, z->cd_pos, h, sizeof(h)) < 0) return -1;
    if (rd32(h) != SIG_CENTRAL) { logline("zip: bad central record"); return -1; }

    uint16_t flags = rd16(h + 8);
    e->method = rd16(h + 10);
    e->crc = rd32(h + 16);
    e->csize = rd32(h + 20);
    e->usize = rd32(h + 24);
    uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
    e->local_off = rd32(h + 42);
    e->encrypted = (flags & 1) != 0;

    uint16_t take = nlen < sizeof(e->name) - 1 ? nlen : (uint16_t)(sizeof(e->name) - 1);
    if (read_at(z->fd, z->cd_pos + 46, e->name, take) < 0) return -1;
    if(memchr(e->name,0,take))return -1;
    e->name[take] = '\0';
    e->name_truncated = take != nlen;

    unsigned long long next=(unsigned long long)z->cd_pos+46u+nlen+xlen+clen;
    if(next>(unsigned long long)z->cd_off+z->cd_size)return -1;
    z->cd_pos=(uint32_t)next;
    z->index++;
    return 1;
}

/* The archive was hashed as it came off the network, which says nothing about
   what reached the stick. The per-entry CRC-32 is the only check that covers
   the bytes actually written. */
static int finish(const struct zipentry *e, uint32_t written, unsigned long sum) {
    if (written != e->usize) {
        logline("zip: %s is %lu bytes, not %lu",
                e->name, (unsigned long)written, (unsigned long)e->usize);
        return -1;
    }
    if ((uint32_t)sum != e->crc) { logline("zip: %s fails its crc", e->name); return -1; }
    return 0;
}

/* Streams one entry's bytes to sink. The local header's own name and extra
   lengths are what position the data; the central record's may differ. */
int zip_extract(struct zipread *z, const struct zipentry *e,
                int (*sink)(void *ctx, const void *data, size_t len), void *ctx) {
    if (e->encrypted) { logline("zip: %s is encrypted", e->name); return -1; }
    if (e->method != 0 && e->method != 8) {
        logline("zip: %s uses method %u", e->name, e->method);
        return -1;
    }

    unsigned char lh[30];
    if (read_at(z->fd, e->local_off, lh, sizeof(lh)) < 0) return -1;
    if (rd32(lh) != SIG_LOCAL) { logline("zip: bad local header"); return -1; }
    unsigned long long offset=(unsigned long long)e->local_off+30u+rd16(lh+26)+rd16(lh+28);
    if(offset+e->csize>z->cd_off)return -1;
    uint32_t data=(uint32_t)offset;
    if (sceIoLseek32(z->fd, (int)data, PSP_SEEK_SET) != (int)data) return -1;

    static unsigned char in[32 * 1024];
    static unsigned char out[64 * 1024];
    uint32_t left = e->csize;
    uint32_t written = 0;
    unsigned long sum = crc32(0L, Z_NULL, 0);

    if (e->method == 0) {
        while (left) {
            int n = sceIoRead(z->fd, in, left < sizeof(in) ? left : sizeof(in));
            if (n <= 0) return -1;
            if((uint32_t)n > e->usize - written)return -1;
            if (sink(ctx, in, (size_t)n) != 0) return -1;
            sum = crc32(sum, in, (uInt)n);
            if((uint32_t)n > e->usize - written)return -1;
            written += (uint32_t)n;
            left -= (uint32_t)n;
        }
        return finish(e, written, sum);
    }

    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return -1;   /* raw deflate */
    int rc = -1, zr = Z_OK;
    while (zr != Z_STREAM_END) {
        /* All of the entry's bytes read is not the end of its output: zlib
           can still hold a long match when the out buffer fills, and gives
           it with no more input. The stream ended early only when inflate
           cannot go on and there is nothing left to give it. */
        if (s.avail_in == 0 && left) {
            int n = sceIoRead(z->fd, in, left < sizeof(in) ? left : sizeof(in));
            if (n <= 0) break;
            left -= (uint32_t)n;
            s.next_in = in;
            s.avail_in = (uInt)n;
        }
        s.next_out = out;
        s.avail_out = sizeof(out);
        zr = inflate(&s, Z_NO_FLUSH);
        if (zr == Z_BUF_ERROR && s.avail_in == 0 && left == 0) { logline("zip: %s ended early", e->name); break; }
        if (zr != Z_OK && zr != Z_STREAM_END) { logline("zip: inflate %d in %s", zr, e->name); break; }
        size_t got = sizeof(out) - s.avail_out;
        /* A deflate stream can claim any expansion ratio it likes. Writing
           past the size the central directory declared is how a 42 MB archive
           fills a Memory Stick, so it ends the entry rather than the stick. */
        if (got > e->usize - written) {
            logline("zip: %s expands past its declared size", e->name);
            break;
        }
        if (got && sink(ctx, out, got) != 0) break;
        sum = crc32(sum, out, (uInt)got);
        written += (uint32_t)got;
    }
    if (zr == Z_STREAM_END) rc = finish(e, written, sum);
    inflateEnd(&s);
    return rc;
}
