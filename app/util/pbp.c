#include "util/storage.h"
/*
 * Reading one section out of an EBOOT.PBP. The header is forty bytes: the
 * magic, a version, and eight little-endian offsets -- SFO, ICON0, ICON1,
 * PIC0, PIC1, SND0, DATA.PSP, DATA.PSAR. A section runs from its offset to
 * the next one's; an absent section has the same offset as its successor.
 * Nothing here holds a buffer: what is read is the caller's to free, and
 * only one section is ever in flight, on the media thread.
 */

#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "install/install.h"
#include "util/pbp.h"
#include "util/runtime.h"

#define HEADER 40
#define SECTIONS 8

/* How large a section of each kind may be before it is refused: a PBP is
   any author's, and a PIC1 of a gigabyte is a mistake, not a picture. The
   SFO is a few hundred bytes; ICON0 a small PNG; PIC0 and PIC1 a screen's
   worth; the film and the sound as much as a real one takes. */
static const size_t CAP[] = {
    [PBP_SFO] = 64 * 1024,
    [PBP_ICON0] = 64 * 1024,
    [PBP_ICON1] = 8 * 1024 * 1024,
    [PBP_PIC0] = 768 * 1024,
    [PBP_PIC1] = 768 * 1024,
    [PBP_SND0] = 2 * 1024 * 1024,
};

static const char *NAME[] = { "SFO", "ICON0", "ICON1", "PIC0", "PIC1", "SND0" };

static unsigned le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned le16(const unsigned char *p) { return p[0] | (p[1] << 8); }

int pbp_section(const char *path, int which, void **out, size_t *len) {
    *out = 0;
    *len = 0;
    if (!path || which < 0 || which > PBP_SND0) return -1;
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return -1;

    unsigned char header[HEADER];
    if (sceIoRead(fd, header, HEADER) != HEADER || memcmp(header, "\0PBP", 4) != 0) {
        logline("pbp: %s is not a PBP", path);
        sceIoClose(fd);
        return -1;
    }
    SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    unsigned offset[SECTIONS];
    for (int i = 0; i < SECTIONS; i++) offset[i] = le32(header + 8 + i * 4);
    /* The offsets go up, and the last one ends at the file's end. A header
       that says otherwise describes a file this is not going to guess at. */
    for (int i = 0; i < SECTIONS; i++) {
        unsigned next = i + 1 < SECTIONS ? offset[i + 1] : (unsigned)size;
        if (offset[i] < HEADER || offset[i] > next || (SceOff)next > size) {
            logline("pbp: %s has offsets out of order", path);
            sceIoClose(fd);
            return -1;
        }
    }
    unsigned start = offset[which], end = offset[which + 1];
    size_t n = end - start;
    if (n == 0) { sceIoClose(fd); return -1; }
    if (n > CAP[which]) {
        logline("pbp: %s of %lu KB in %s, at most %lu", NAME[which],
                (unsigned long)(n / 1024), path, (unsigned long)(CAP[which] / 1024));
        sceIoClose(fd);
        return -1;
    }
    unsigned char *buf = malloc(n);
    if (!buf) {
        logline("pbp: no room for %lu KB of %s", (unsigned long)(n / 1024), NAME[which]);
        sceIoClose(fd);
        return -1;
    }
    if (sceIoLseek(fd, start, PSP_SEEK_SET) != (SceOff)start ||
        sceIoRead(fd, buf, (SceSize)n) != (int)n) {
        logline("pbp: short read of %s from %s", NAME[which], path);
        free(buf);
        sceIoClose(fd);
        return -1;
    }
    sceIoClose(fd);
    logline("pbp: %s %lu bytes from %s", NAME[which], (unsigned long)n, path);
    *out = buf;
    *len = n;
    return 0;
}

/* A PARAM.SFO: the magic, a version, then the offsets of a key table and
   a data table and the count of entries, which follow the twenty-byte
   header. Each entry is sixteen bytes and names a key by offset into the
   key table and a value by offset into the data table. */
int pbp_title(const char *path, char *out, size_t size) {
    if (!out || size == 0) return -1;
    out[0] = '\0';
    void *raw;
    size_t len;
    if (pbp_section(path, PBP_SFO, &raw, &len) != 0) return -1;
    const unsigned char *sfo = raw;
    int rc = -1;
    if (len < 20 || memcmp(sfo, "\0PSF", 4) != 0) {
        logline("pbp: the SFO in %s is not one", path);
        free(raw);
        return -1;
    }
    unsigned keys = le32(sfo + 8), data = le32(sfo + 12), count = le32(sfo + 16);
    for (unsigned i = 0; i < count; i++) {
        size_t at = 20 + (size_t)i * 16;
        if (at + 16 > len) break;
        unsigned key = keys + le16(sfo + at);
        unsigned fmt = le16(sfo + at + 2);
        unsigned n = le32(sfo + at + 4);
        unsigned value = data + le32(sfo + at + 12);
        /* Every key is NUL-terminated inside the table; one that runs off
           the end is refused rather than read past. */
        if (key >= len || !memchr(sfo + key, 0, len - key)) break;
        if (strcmp((const char *)sfo + key, "TITLE") != 0) continue;
        /* 0x0204 is a UTF-8 string, its length counting the NUL. */
        if (fmt != 0x0204 || value >= len || n > len - value) break;
        size_t copy = n;
        while (copy > 0 && sfo[value + copy - 1] == '\0') copy--;
        if (copy >= size) copy = size - 1;
        memcpy(out, sfo + value, copy);
        out[copy] = '\0';
        rc = copy > 0 ? 0 : -1;
        break;
    }
    free(raw);
    return rc;
}

int pbp_installed_path(const char *id, char *out, size_t size) {
    struct installed rec;
    if (!id || !id[0] || db_read(id, &rec) != 0 || !rec.dir[0]) return -1;
    snprintf(out, size, storage_path("PSP/GAME/%s/EBOOT.PBP"), rec.dir);
    return 0;
}
