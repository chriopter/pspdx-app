/* The PNG decoder on the host: every file named is decoded as the card and
   the row decode one, and each gets a line -- the result, the picture's size,
   the texture's, and the first and the last pixel of the picture. Built with
   the sanitizers and run with leak checking, so a picture that fails part
   way through cannot leave what it had allocated behind. */
#include "gui/image.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
void logline(const char *fmt, ...) {
    if (!getenv("VERBOSE"))
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
void sceKernelDcacheWritebackRange(void *p, unsigned n) {
    (void)p;
    (void)n;
}
void gfx_texture_free(struct gfx_texture *t) {
    free(t->pixels);
    t->pixels = NULL;
}
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f)
            return 2;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        rewind(f);
        unsigned char *data = malloc(n ? (size_t)n : 1);
        if (!data || fread(data, 1, (size_t)n, f) != (size_t)n)
            return 2;
        fclose(f);
        struct gfx_texture t;
        int rc = image_decode_png(data, (size_t)n, &t);
        free(data);
        if (rc < 0) {
            printf("-1\n");
            if (t.pixels)
                return 3;
            continue;
        }
        const unsigned char *px = t.pixels, *last = px + ((size_t)(t.h - 1) * t.tw + (t.w - 1)) * 4;
        printf("0 %dx%d %dx%d %u,%u,%u,%u %u,%u,%u,%u\n", t.w, t.h, t.tw, t.th, px[0], px[1], px[2],
               px[3], last[0], last[1], last[2], last[3]);
        gfx_texture_free(&t);
    }
    return 0;
}
