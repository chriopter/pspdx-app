#include "video/psmf.h"
#include "video/mp4.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void be16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void be32(unsigned char *p, unsigned v) {
    be16(p, v >> 16);
    be16(p + 2, v);
}

static unsigned char *one_frame(size_t *len) {
    *len = PSMF_HEADER + PSMF_PACK;
    unsigned char *p = calloc(1, *len);
    assert(p);
    memcpy(p, "PSMF0015", 8);
    be32(p + 0x08, PSMF_HEADER);
    be32(p + 0x0c, PSMF_PACK);
    be32(p + 0x56, 90000);
    be32(p + 0x5c, 93000);
    be16(p + 0x80, 1);
    p[0x82] = 0xe0;
    p[0x8e] = 30;
    p[0x8f] = 17;

    unsigned char *pack = p + PSMF_HEADER;
    pack[0] = 0; pack[1] = 0; pack[2] = 1; pack[3] = 0xba;
    pack[4] = 0x44; pack[6] = 0x04; pack[8] = 0x04; pack[9] = 0x01;
    pack[10] = 0; pack[11] = 0x17; pack[12] = 0x73; pack[13] = 0xf8;
    unsigned char sys[] = {
        0, 0, 1, 0xbb, 0, 12, 0x80, 0xc3, 0x51,
        0x80, 0xf0, 0x7f, 0xb9, 0xe0, 0x50, 0xbd, 0xe0, 8
    };
    memcpy(pack + 14, sys, sizeof(sys));
    unsigned char *bf = pack + 32;
    bf[2] = 1; bf[3] = 0xbf; be16(bf + 4, 22);
    bf[6] = 1; bf[7] = 0xe0; be16(bf + 20, 6); be16(bf + 22, 1);
    be16(bf + 26, 1);
    unsigned char *pes = pack + 60;
    pes[2] = 1; pes[3] = 0xe0; be16(pes + 4, 3); pes[7] = 0x80;
    return p;
}

struct mp4_file {
    unsigned char data[2048];
    size_t pos;
    size_t stts, stsc, stsz, stco, stss, ctts;
};

static void mp4_put32(struct mp4_file *f, unsigned v) {
    assert(f->pos + 4 <= sizeof(f->data));
    be32(f->data + f->pos, v);
    f->pos += 4;
}

static size_t box_begin(struct mp4_file *f, const char type[4]) {
    size_t at = f->pos;
    mp4_put32(f, 0);
    memcpy(f->data + f->pos, type, 4);
    f->pos += 4;
    return at;
}

static void box_end(struct mp4_file *f, size_t at) {
    assert(f->pos - at <= 0xffffffffu);
    be32(f->data + at, (unsigned)(f->pos - at));
}

static void full_table(struct mp4_file *f, const char type[4], size_t *body,
                       unsigned a, unsigned b, unsigned c) {
    size_t box = box_begin(f, type);
    *body = f->pos;
    mp4_put32(f, 0);                 /* version and flags */
    mp4_put32(f, 1);                 /* entry count */
    mp4_put32(f, a);
    if (b != 0xffffffffu) mp4_put32(f, b);
    if (c != 0xffffffffu) mp4_put32(f, c);
    box_end(f, box);
}

static void make_mp4(struct mp4_file *f) {
    memset(f, 0, sizeof(*f));
    size_t moov = box_begin(f, "moov");
    size_t trak = box_begin(f, "trak");
    size_t mdia = box_begin(f, "mdia");

    size_t mdhd = box_begin(f, "mdhd");
    mp4_put32(f, 0); mp4_put32(f, 0); mp4_put32(f, 0);
    mp4_put32(f, 90000); mp4_put32(f, 3000);
    box_end(f, mdhd);
    size_t hdlr = box_begin(f, "hdlr");
    mp4_put32(f, 0); mp4_put32(f, 0); mp4_put32(f, 0x76696465); /* vide */
    box_end(f, hdlr);

    size_t minf = box_begin(f, "minf");
    size_t stbl = box_begin(f, "stbl");
    size_t stsd = box_begin(f, "stsd");
    mp4_put32(f, 0); mp4_put32(f, 1);
    size_t avc1 = box_begin(f, "avc1");
    size_t avc1_body = f->pos;
    memset(f->data + f->pos, 0, 78); f->pos += 78;
    be16(f->data + avc1_body + 24, 16);
    be16(f->data + avc1_body + 26, 16);
    size_t avcc = box_begin(f, "avcC");
    unsigned char config[] = { 1, 0x42, 0, 0x1e, 0xff, 0xe1, 0, 1, 0x67, 1, 0, 1, 0x68 };
    memcpy(f->data + f->pos, config, sizeof(config)); f->pos += sizeof(config);
    box_end(f, avcc); box_end(f, avc1); box_end(f, stsd);

    full_table(f, "stts", &f->stts, 1, 3000, 0xffffffffu);
    full_table(f, "stsc", &f->stsc, 1, 1, 1);
    size_t stsz = box_begin(f, "stsz");
    f->stsz = f->pos;
    mp4_put32(f, 0); mp4_put32(f, 0); mp4_put32(f, 1); mp4_put32(f, 4);
    box_end(f, stsz);
    full_table(f, "stco", &f->stco, 0, 0xffffffffu, 0xffffffffu);
    full_table(f, "stss", &f->stss, 1, 0xffffffffu, 0xffffffffu);
    full_table(f, "ctts", &f->ctts, 1, 0, 0xffffffffu);
    box_end(f, stbl); box_end(f, minf); box_end(f, mdia); box_end(f, trak); box_end(f, moov);
}

static void mp4_bounds(void) {
    struct mp4_file f;
    struct mp4 track;
    make_mp4(&f);
    assert(mp4_parse(f.data, f.pos, &track) == 0);
    assert(track.count == 1 && track.sample[0].size == 4 && track.sample[0].key);
    for (size_t n = 0; n < f.pos; n++)
        assert(mp4_parse(f.data, n, &track) < 0);

    size_t counts[] = { f.stts + 4, f.stsc + 4, f.stsz + 8,
                        f.stco + 4, f.stss + 4, f.ctts + 4 };
    for (size_t i = 0; i < sizeof(counts) / sizeof(*counts); i++) {
        be32(f.data + counts[i], 2);
        assert(mp4_parse(f.data, f.pos, &track) < 0);
        be32(f.data + counts[i], 1);
    }
    be32(f.data + f.stts + 8, 2);
    assert(mp4_parse(f.data, f.pos, &track) < 0);
    be32(f.data + f.stts + 8, 1);
    be32(f.data + f.stsc + 12, 2);
    assert(mp4_parse(f.data, f.pos, &track) < 0);
    be32(f.data + f.stsc + 12, 1);
    be32(f.data + f.ctts + 8, 2);
    assert(mp4_parse(f.data, f.pos, &track) < 0);
    be32(f.data + f.ctts + 8, 1);
    be32(f.data + f.stss + 8, 2);
    assert(mp4_parse(f.data, f.pos, &track) < 0);
    be32(f.data + f.stss + 8, 1);
    f.data[f.ctts] = 1;
    be32(f.data + f.ctts + 12, 0xffffffffu);
    assert(mp4_parse(f.data, f.pos, &track) < 0);
    f.data[f.ctts] = 0;
    be32(f.data + f.ctts + 12, 0);
    be32(f.data + f.stco + 8, 0xfffffffeu);
    assert(mp4_parse(f.data, f.pos, &track) < 0);
    be32(f.data, 0xffffffffu);
    assert(mp4_parse(f.data, f.pos, &track) < 0);
}

static void parse_file(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long end = ftell(f);
    assert(end > 0 && fseek(f, 0, SEEK_SET) == 0);
    unsigned char *p = malloc((size_t)end);
    assert(p && fread(p, 1, (size_t)end, f) == (size_t)end);
    fclose(f);
    struct psmf_info info;
    assert(psmf_parse(p, (size_t)end, &info) == 0);
    unsigned char header[PSMF_HEADER];
    /* The bundled legacy mux starts directly with E0. It is useful to the
       metadata reader but must never reach real sceMpeg. */
    assert(psmf_decoder_header(p, (size_t)end, header) < 0);
    free(p);
}

int main(int argc, char **argv) {
    mp4_bounds();
    size_t len;
    unsigned char *p = one_frame(&len);
    struct psmf_info info;
    assert(psmf_parse(p, len, &info) == 0);
    assert(info.stream_offset == PSMF_HEADER);
    assert(info.stream_size == PSMF_PACK);
    assert(info.width == 480 && info.height == 272 && info.frames == 1);

    unsigned char header[PSMF_HEADER];
    assert(psmf_decoder_header(p, len, header) == 0);
    assert(psmf_decoder_header(p, len - 1, header) < 0);
    assert(memcmp(header, p, PSMF_HEADER) == 0);

    assert(psmf_parse(p, len - 1, &info) < 0);
    be32(p + 0x08, PSMF_HEADER + 1);
    assert(psmf_parse(p, len, &info) < 0);
    be32(p + 0x08, PSMF_HEADER);
    be32(p + 0x0c, PSMF_PACK - 1);
    assert(psmf_parse(p, len, &info) < 0);
    be32(p + 0x0c, PSMF_PACK);
    be16(p + 0x80, 0xffff);
    assert(psmf_parse(p, len, &info) < 0);
    be16(p + 0x80, 1);
    p[PSMF_HEADER + 17] = 0xe0;
    assert(psmf_parse(p, len, &info) == 0);
    assert(psmf_decoder_header(p, len, header) < 0);
    p[PSMF_HEADER + 17] = 0xbb;
    p[PSMF_HEADER + 4] = 0;
    assert(psmf_parse(p, len, &info) < 0);
    p[PSMF_HEADER + 4] = 0x44;
    memset(p + PSMF_HEADER, 0, PSMF_PACK);
    assert(psmf_parse(p, len, &info) < 0);
    free(p);
    if (argc > 1) parse_file(argv[1]);
    return 0;
}
