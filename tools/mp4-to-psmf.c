/* Wraps an MP4 the way the client does on the PSP, on a desk, so the result
   can be checked with tools the PSP does not have:

     cd app && cc -I. video/mp4.c video/psmf.c ../tools/mp4-to-psmf.c
     ./a.out video.mp4 video.psmf
     ffprobe video.psmf          # the stream the PSP will see
     ffmpeg -ss 0.8 -i video.psmf -frames:v 1 frame.png

   The header is skipped by ffmpeg on its own: it looks for the first pack
   start code. */

#include <stdio.h>
#include <stdlib.h>

#include "video/mp4.h"
#include "video/psmf.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.mp4 out.psmf\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *mp4 = malloc(len);
    if (fread(mp4, 1, len, f) != (size_t)len) { perror("read"); return 1; }
    fclose(f);

    static struct mp4 track;
    if (mp4_parse(mp4, len, &track) != 0) { fprintf(stderr, "%s: no playable video track\n", argv[1]); return 1; }
    printf("%dx%d, %d samples, %u ticks, nal length %d, sps %d pps %d, timescale %u\n",
           track.width, track.height, track.count, track.duration, track.nal_length_size,
           track.sps_len, track.pps_len, track.timescale);

    size_t cap = psmf_capacity(len);
    unsigned char *out = malloc(cap);
    size_t n = psmf_build(mp4, &track, out, cap);
    if (!n) { fprintf(stderr, "psmf_build failed\n"); return 1; }
    f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(out, 1, n, f);
    fclose(f);
    printf("%s: %lu bytes, %lu packs\n", argv[2], (unsigned long)n, (unsigned long)((n - PSMF_HEADER) / PSMF_PACK));
    return 0;
}
