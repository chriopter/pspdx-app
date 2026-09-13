/* Renders the tune and the interface cues to a WAV on the desk, with the
   same code the PSP runs:

     cc -O2 -I. audio/synth.c audio/music.c audio/cues.c tools/render-music.c -lm
     ./a.out music.wav [seconds]

   The tail carries the cues: a scroll down five rows, an open, a done, a
   fail, two seconds apart. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/cues.h"
#include "audio/music.h"
#include "audio/synth.h"

#define RATE 44100

/* cues.c asks the clock whether two moves are one run; on the desk there is
   no clock to ask and no run to tell apart. */
unsigned now_ms(void) { return 0; }

static void put32(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); }

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "music.wav";
    int seconds = argc > 2 ? atoi(argv[2]) : 50;
    long frames = (long)seconds * RATE;

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 4); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, RATE); put32(f, RATE * 4); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, frames * 4);

    synth_init(RATE);
    music_init(RATE);

    /* The cues go in during the last ten seconds. */
    long cue_at[8]; enum cue cue[8]; int idx[8]; int n = 0;
    long start = frames - 10 * RATE;
    for (int i = 0; i < 5; i++) { cue_at[n] = start + i * RATE / 3; cue[n] = CUE_MOVE; idx[n++] = i; }
    cue_at[n] = start + 3 * RATE; cue[n] = CUE_OPEN; idx[n++] = 0;
    cue_at[n] = start + 5 * RATE; cue[n] = CUE_DONE; idx[n++] = 0;
    cue_at[n] = start + 7 * RATE; cue[n] = CUE_FAIL; idx[n++] = 0;

    short buf[1024 * 2];
    int next = 0, peak = 0;
    for (long pos = 0; pos < frames; pos += 1024) {
        while (next < n && cue_at[next] <= pos) { cues_post(cue[next], idx[next]); next++; }
        int chunk = frames - pos < 1024 ? (int)(frames - pos) : 1024;
        music_render(buf, chunk);
        for (int i = 0; i < chunk * 2; i++) {
            int a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        fwrite(buf, 4, chunk, f);
    }
    fclose(f);
    printf("%s: %d s, peak %d of 32767\n", path, seconds, peak);
    return 0;
}
