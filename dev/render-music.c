/* Renders the tune and the interface cues to a WAV on the desk, with the
   same code the PSP runs, at the rate the PSP runs it: the synth at a
   quarter of 44.1 kHz, then the same linear steps up that audio.c takes.

     cd app && cc -O2 -I. audio/synth.c audio/music.c audio/cues.c ../dev/render-music.c -lm
     ./a.out music.wav [seconds] [nocues]

   The tail carries the cues: a scroll down five rows, an open, a done, a
   fail, two seconds apart. Prints the peak, the loudness, how often the
   wall was hit, how much faster than real time the synth ran, and (on
   x86) whether any float went subnormal on the way. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define CSR_CLEAR() _mm_setcsr(_mm_getcsr() & ~0x3fu)
#define CSR_TINY() (_mm_getcsr() & 0x12u)     /* denormal operand, underflow */
#else
#define CSR_CLEAR() ((void)0)
#define CSR_TINY() 0
#endif

#include "audio/cues.h"
#include "audio/music.h"
#include "audio/synth.h"

#define RATE 44100
#define SYNTH_RATE (RATE / 4)
#define CHUNK 1024

/* cues.c asks the clock whether two moves are one run; on the desk there is
   no clock to ask and no run to tell apart. */
unsigned now_ms(void) { return 0; }

static void put32(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); }

static double seconds_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "music.wav";
    int seconds = argc > 2 ? atoi(argv[2]) : 50;
    int cues = !(argc > 3 && !strcmp(argv[3], "nocues"));
    long frames = (long)seconds * RATE / CHUNK * CHUNK;

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 4); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, RATE); put32(f, RATE * 4); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, frames * 4);

    synth_init(SYNTH_RATE);
    music_init(SYNTH_RATE);
    CSR_CLEAR();

    /* The cues go in during the last ten seconds. */
    long cue_at[8]; enum cue cue[8]; int idx[8]; int n = 0;
    long start = frames - 10 * RATE;
    if (cues) {
        for (int i = 0; i < 5; i++) { cue_at[n] = start + i * RATE / 3; cue[n] = CUE_MOVE; idx[n++] = i; }
        cue_at[n] = start + 3 * RATE; cue[n] = CUE_OPEN; idx[n++] = 0;
        cue_at[n] = start + 5 * RATE; cue[n] = CUE_DONE; idx[n++] = 0;
        cue_at[n] = start + 7 * RATE; cue[n] = CUE_FAIL; idx[n++] = 0;
    }

    /* audio.c's music_render_output, step for step. */
    short low[(CHUNK / 4) * 2], out[CHUNK * 2], carry[2];
    music_render(carry, 1);
    int next = 0, peak = 0, tiny_chunks = 0;
    long wall = 0;
    double sum = 0, synth_s = 0;
    for (long pos = 0; pos < frames; pos += CHUNK) {
        while (next < n && cue_at[next] <= pos) { cues_post(cue[next], idx[next]); next++; }
        double t0 = seconds_now();
        music_render(low, CHUNK / 4);
        synth_s += seconds_now() - t0;
        if (CSR_TINY()) { tiny_chunks++; CSR_CLEAR(); }
        int l = carry[0], r = carry[1];
        for (int i = 0; i < CHUNK / 4; i++) {
            int nl = low[i * 2], nr = low[i * 2 + 1];
            short *d = out + i * 8;
            d[0] = (short)l;                    d[1] = (short)r;
            d[2] = (short)((l * 3 + nl) / 4);   d[3] = (short)((r * 3 + nr) / 4);
            d[4] = (short)((l + nl) / 2);       d[5] = (short)((r + nr) / 2);
            d[6] = (short)((l + nl * 3) / 4);   d[7] = (short)((r + nr * 3) / 4);
            l = nl;
            r = nr;
        }
        carry[0] = (short)l;
        carry[1] = (short)r;
        for (int i = 0; i < CHUNK * 2; i++) {
            int a = out[i] < 0 ? -out[i] : out[i];
            if (a > peak) peak = a;
            if (a >= 32000) wall++;
            sum += (double)out[i] * out[i];
        }
        fwrite(out, 4, CHUNK, f);
    }
    fclose(f);
    double rms = sqrt(sum / (frames * 2.0));
    printf("%s: %d s, peak %d of 32767 (%.1f dBFS), rms %.1f dBFS, %ld samples at the wall,\n"
           "  synth %.3f s for %d s (%.0fx real time), %d chunks with subnormal floats\n",
           path, seconds, peak, 20 * log10(peak / 32767.0), 20 * log10(rms / 32767.0), wall,
           synth_s, seconds, seconds / synth_s, tiny_chunks);
    return 0;
}
