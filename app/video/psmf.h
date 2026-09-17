#ifndef PSPDX_PSMF_H
#define PSPDX_PSMF_H

#include <stddef.h>

#include "video/mp4.h"

/* Wraps the H.264 out of an MP4 the way the PSP's own movie player wants
   it: a PSMF header, then an MPEG-2 program stream in 2048-byte packs with
   the video in PES 0xE0. Same pictures, different envelope. Plain C; the
   result is what sceMpeg reads, and what ffmpeg reads too, which is how it
   gets checked on a desk. */

#define PSMF_HEADER 0x800
#define PSMF_PACK 2048

/* How much room to give psmf_build for a given MP4: the stream plus its
   packaging, rounded up. */
size_t psmf_capacity(size_t mp4_len);

/* Returns the number of bytes written, 0 if out is too small or the track
   is not something the PSP can play. */
size_t psmf_build(const unsigned char *mp4, const struct mp4 *track,
                  unsigned char *out, size_t cap);

/* What a PSMF says about itself, whether psmf_build wrote it or it came out
   of an EBOOT as ICON1.PMF: where the packs start and how many there are,
   the picture's size, the presentation's first and last timestamps at
   90 kHz, and how many pictures are in it, which the header does not say
   and is counted by walking the packs for the PES packets that carry a
   timestamp. */
struct psmf_info {
    size_t stream_offset, stream_size;
    int width, height;
    unsigned start_pts, end_pts;
    int frames;
};

/* True when data starts with the PSMF magic, whatever follows it. */
int psmf_is(const unsigned char *data, size_t len);

/* 0 on success; -1 when it is not a PSMF, has no video stream, or the
   stream does not fit in len. */
int psmf_parse(const unsigned char *data, size_t len, struct psmf_info *out);

/* Copy the header only when the first pack has the strict Sony system-header
   and AU-index layout that real sceMpeg accepts. */
int psmf_decoder_header(const unsigned char *data, size_t len,
                        unsigned char out[PSMF_HEADER]);

/* How long one picture stays, in 90 kHz ticks: the presentation's span
   over its count, or thirty a second when the header cannot say. */
unsigned psmf_frame_ticks(const struct psmf_info *info);

#endif
