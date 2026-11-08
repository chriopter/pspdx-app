/*
 * Reading a string out of a PARAM.SFO. The header is twenty bytes -- the
 * magic "\0PSF", a version, where the key table and the data table start,
 * and how many entries there are -- then sixteen bytes an entry: the key's
 * offset into the key table, the value's format, its length and room, and
 * its offset into the data table. Everything is little-endian and every
 * offset is checked against the buffer: an SFO is any author's.
 */

#include <string.h>

#include "util/sfo.h"

static unsigned le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

int sfo_string(const void *sfo, size_t len, const char *key, char *out, size_t size) {
    const unsigned char *b = sfo;
    if (size) out[0] = '\0';
    if (!b || len < 20 || memcmp(b, "\0PSF", 4) != 0) return -1;
    unsigned keys = le32(b + 8), data = le32(b + 12), count = le32(b + 16);
    if (keys > len || data > len || count > (len - 20) / 16) return -1;
    size_t want = strlen(key);
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = b + 20 + i * 16;
        unsigned name = keys + le16(e), fmt = le16(e + 2), n = le32(e + 4), at = data + le32(e + 12);
        if (name >= len || len - name < want + 1 || memcmp(b + name, key, want + 1) != 0) continue;
        /* 0x0204 a terminated string, 0x0004 one that need not be; the
           number kind (0x0404) is not what is asked for. */
        if ((fmt != 0x0204 && fmt != 0x0004) || at > len || n > len - at) return -1;
        size_t k = 0;
        while (k < n && b[at + k]) k++;
        if (k >= size) k = size ? size - 1 : 0;
        if (size) {
            memcpy(out, b + at, k);
            out[k] = '\0';
        }
        return 0;
    }
    return -1;
}
