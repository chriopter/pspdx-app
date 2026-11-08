/*
 * The few sectors of a UMD image the library needs. An ISO 9660 volume
 * keeps its primary descriptor in sector 16, and in it the root
 * directory's record; a directory is a run of records, each the extent and
 * length of one file or directory under it, never crossing a sector's end.
 * A path is looked up a name at a time from the root.
 *
 * A CSO cuts the image into blocks (2 KB as a rule) and keeps after its
 * 24-byte header one 32-bit index entry a block and one more: the entry is
 * where the block's bytes start, shifted by the header's alignment, its top
 * bit set where the block is stored as it is. The next entry is where it
 * ends. A ZSO is the same file with LZ4 in place of deflate, and a version
 * 2 CSO says "stored" by a block as long as it is and uses the top bit for
 * LZ4. Only the two entries a block needs are read, never the whole index,
 * which for a full UMD is several megabytes.
 */

#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "util/iso.h"

#define SECTOR 2048
#define DIR_MAX (256 * 1024)        /* a directory longer than this is no game's */

enum { PLAIN, CSO, ZSO };

struct image {
    int fd;
    int kind;
    unsigned block, align, version;
    unsigned long long total;
    unsigned char *buf, *packed;    /* one block out, and its bytes as stored */
    long long cached;               /* the block in buf, -1 for none */
};

static unsigned le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

int iso_name(const char *name) {
    size_t n = strlen(name);
    return n > 4 && name[n - 4] == '.' &&
           (!strcasecmp(name + n - 3, "iso") || !strcasecmp(name + n - 3, "cso") ||
            !strcasecmp(name + n - 3, "zso"));
}

/* An LZ4 block: literals and matches, each sequence a token, until the
   input ends. Returns the bytes written, -1 for anything malformed. */
static int lz4_block(const unsigned char *src, size_t n, unsigned char *dst, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned char c;
            do {
                if (i >= n) return -1;
                c = src[i++];
                lit += c;
            } while (c == 255);
        }
        if (lit > n - i || lit > cap - o) return -1;
        memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;
        if (i >= n) break;              /* the last sequence has no match */
        if (n - i < 2) return -1;
        size_t off = src[i] | (src[i + 1] << 8);
        i += 2;
        if (off == 0 || off > o) return -1;
        size_t m = token & 15;
        if (m == 15) {
            unsigned char c;
            do {
                if (i >= n) return -1;
                c = src[i++];
                m += c;
            } while (c == 255);
        }
        m += 4;
        if (m > cap - o) return -1;
        /* A byte at a time: a match may overlap what it is writing. */
        for (size_t k = 0; k < m; k++, o++) dst[o] = dst[o - off];
    }
    return (int)o;
}

static int inflate_block(const unsigned char *src, size_t n, unsigned char *dst, size_t cap) {
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return -1;   /* raw deflate */
    s.next_in = (unsigned char *)src;
    s.avail_in = (uInt)n;
    s.next_out = dst;
    s.avail_out = (uInt)cap;
    int rc = inflate(&s, Z_FINISH);
    int got = (int)(cap - s.avail_out);
    inflateEnd(&s);
    return rc == Z_STREAM_END || (rc == Z_BUF_ERROR && s.avail_out == 0) || rc == Z_OK ? got : -1;
}

static int image_open(struct image *im, const char *path) {
    memset(im, 0, sizeof(*im));
    im->cached = -1;
    im->fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (im->fd < 0) return -1;
    unsigned char h[24];
    if (sceIoRead(im->fd, h, sizeof(h)) != (int)sizeof(h)) return -1;
    if (!memcmp(h, "CISO", 4) || !memcmp(h, "ZISO", 4)) {
        im->kind = h[0] == 'C' ? CSO : ZSO;
        im->total = le32(h + 8) | (unsigned long long)le32(h + 12) << 32;
        im->block = le32(h + 16);
        im->version = h[20];
        im->align = h[21];
        /* A block is a sector or a few; anything else is not an image. */
        if (im->block < SECTOR || im->block > 64 * 1024 || im->align > 16) return -1;
        im->buf = malloc(im->block);
        im->packed = malloc(2 * (size_t)im->block);
        if (!im->buf || !im->packed) return -1;
    } else {
        im->kind = PLAIN;
        im->total = (unsigned long long)sceIoLseek(im->fd, 0, PSP_SEEK_END);
    }
    return 0;
}

static void image_close(struct image *im) {
    if (im->fd >= 0) sceIoClose(im->fd);
    free(im->buf);
    free(im->packed);
    im->fd = -1;
    im->buf = im->packed = 0;
}

/* Block b of a compressed image into im->buf. */
static int load_block(struct image *im, long long b) {
    if (im->cached == b) return 0;
    im->cached = -1;
    unsigned char e[8];
    if (sceIoLseek(im->fd, 24 + (SceOff)b * 4, PSP_SEEK_SET) != 24 + (SceOff)b * 4 ||
        sceIoRead(im->fd, e, 8) != 8)
        return -1;
    unsigned e0 = le32(e), e1 = le32(e + 4);
    unsigned long long at = (unsigned long long)(e0 & 0x7FFFFFFFu) << im->align;
    unsigned long long end = (unsigned long long)(e1 & 0x7FFFFFFFu) << im->align;
    if (end < at) return -1;
    size_t n = (size_t)(end - at);
    int stored = im->version >= 2 && im->kind == CSO ? n >= im->block : (e0 & 0x80000000u) != 0;
    int lz4 = im->kind == ZSO || (im->version >= 2 && (e0 & 0x80000000u));
    if (stored && n > im->block) n = im->block;     /* the alignment's padding */
    if (n > 2 * (size_t)im->block) return -1;
    if (sceIoLseek(im->fd, (SceOff)at, PSP_SEEK_SET) != (SceOff)at) return -1;
    if (stored) {
        /* The last block of an image can be short. */
        int got = sceIoRead(im->fd, im->buf, n);
        if (got <= 0) return -1;
        if ((size_t)got < im->block) memset(im->buf + got, 0, im->block - (size_t)got);
    } else {
        if (sceIoRead(im->fd, im->packed, n) != (int)n) return -1;
        int got = lz4 ? lz4_block(im->packed, n, im->buf, im->block)
                      : inflate_block(im->packed, n, im->buf, im->block);
        if (got <= 0) return -1;
        if ((size_t)got < im->block) memset(im->buf + got, 0, im->block - (size_t)got);
    }
    im->cached = b;
    return 0;
}

static int image_read(struct image *im, unsigned long long at, void *dst, size_t n) {
    if (at > im->total || n > im->total - at) return -1;
    if (im->kind == PLAIN)
        return sceIoLseek(im->fd, (SceOff)at, PSP_SEEK_SET) == (SceOff)at &&
               sceIoRead(im->fd, dst, n) == (int)n ? 0 : -1;
    unsigned char *out = dst;
    while (n) {
        long long b = (long long)(at / im->block);
        size_t in = (size_t)(at % im->block), take = im->block - in;
        if (take > n) take = n;
        if (load_block(im, b) != 0) return -1;
        memcpy(out, im->buf + in, take);
        out += take;
        at += take;
        n -= take;
    }
    return 0;
}

/* Whether a directory record's name is the one looked for: case aside,
   and without the ";1" a file's carries or the dot a name with no
   extension can end in. */
static int same_name(const unsigned char *name, size_t n, const char *want, size_t w) {
    size_t k = 0;
    while (k < n && name[k] != ';') k++;
    if (k && name[k - 1] == '.') k--;
    return k == w && !strncasecmp((const char *)name, want, w);
}

/* Where the file at inner starts and how long it is. */
static int lookup(struct image *im, const char *inner, unsigned *lba, unsigned *size) {
    unsigned char pvd[SECTOR];
    if (image_read(im, 16ull * SECTOR, pvd, SECTOR) != 0 || pvd[0] != 1 ||
        memcmp(pvd + 1, "CD001", 5) != 0)
        return -1;
    unsigned at = le32(pvd + 156 + 2), n = le32(pvd + 156 + 10);
    for (const char *p = inner; *p;) {
        size_t w = strcspn(p, "/");
        if (n == 0 || n > DIR_MAX) return -1;
        unsigned char *dir = malloc(n);
        if (!dir) return -1;
        if (image_read(im, (unsigned long long)at * SECTOR, dir, n) != 0) {
            free(dir);
            return -1;
        }
        int found = 0;
        for (unsigned off = 0; off < n;) {
            unsigned len = dir[off];
            /* Nothing more in this sector: the next record is in the next. */
            if (len == 0) { off = (off / SECTOR + 1) * SECTOR; continue; }
            if (len < 34 || off + len > n || 33u + dir[off + 32] > len) break;
            if (same_name(dir + off + 33, dir[off + 32], p, w)) {
                at = le32(dir + off + 2);
                n = le32(dir + off + 10);
                found = 1;
                break;
            }
            off += len;
        }
        free(dir);
        if (!found) return -1;
        p += w;
        while (*p == '/') p++;
    }
    *lba = at;
    *size = n;
    return 0;
}

int iso_file(const char *path, const char *inner, size_t cap, void **out, size_t *len) {
    *out = 0;
    *len = 0;
    struct image im;
    unsigned lba, size;
    int rc = -1;
    if (image_open(&im, path) == 0 && lookup(&im, inner, &lba, &size) == 0 && size && size <= cap) {
        unsigned char *buf = malloc(size);
        if (buf && image_read(&im, (unsigned long long)lba * SECTOR, buf, size) == 0) {
            *out = buf;
            *len = size;
            rc = 0;
        } else {
            free(buf);
        }
    }
    image_close(&im);
    return rc;
}
