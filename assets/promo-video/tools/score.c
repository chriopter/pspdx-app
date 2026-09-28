/* The promo's music bed, played by PSPDX's own synth: the tune the app plays,
   with the app's interface cues and struck notes placed where the cut wants
   them. Built by audio.py against app/audio (read-only):

     cc -O2 -I<stub> -I<app> <app>/audio/synth.c <app>/audio/music.c \
        <app>/audio/cues.c score.c -lm -o score
     ./score events.txt out.wav seconds

   events.txt, one per line, sorted by time:
     <sec> cue <0 move|1 open|2 done|3 fail> <row>
     <sec> strike <midi note> <velocity 0..1> <timbre> <pan -1..1>
     <sec> level <music level 0..1>
   Timbres: 0 piano 1 glass 2 pad 3 drift 4 sub 5 shimmer 6 bell 7 chime.
   Exit status: 0 written, 1 bad arguments or file. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/cues.h"
#include "audio/music.h"
#include "audio/synth.h"

#define RATE 48000
#define BLOCK 64

unsigned now_ms(void) { return 0; }

struct ev { double at; char kind[8]; double a, b, c, d; };

static void put32(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); }

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: score events.txt out.wav seconds\n"); return 1; }
    FILE *in = fopen(argv[1], "r");
    if (!in) { perror(argv[1]); return 1; }
    static struct ev evs[4096];
    int n = 0;
    char line[256];
    while (n < 4096 && fgets(line, sizeof(line), in)) {
        struct ev e = {0};
        if (sscanf(line, "%lf %7s %lf %lf %lf %lf", &e.at, e.kind, &e.a, &e.b, &e.c, &e.d) >= 2) evs[n++] = e;
    }
    fclose(in);

    double seconds = atof(argv[3]);
    long frames = (long)(seconds * RATE);
    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 4); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, RATE); put32(f, RATE * 4); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, frames * 4);

    synth_init(RATE);
    music_init(RATE);
    short buf[BLOCK * 2];
    int next = 0;
    for (long pos = 0; pos < frames; pos += BLOCK) {
        while (next < n && evs[next].at * RATE <= pos) {
            struct ev *e = &evs[next++];
            if (!strcmp(e->kind, "cue")) cues_post((enum cue)(int)e->a, (int)e->b);
            else if (!strcmp(e->kind, "strike"))
                synth_strike((int)e->a, (float)e->b, (enum synth_timbre)(int)e->c, (float)e->d, 0);
            else if (!strcmp(e->kind, "level")) synth_set_music_level((float)e->a);
        }
        int chunk = frames - pos < BLOCK ? (int)(frames - pos) : BLOCK;
        music_render(buf, chunk);
        fwrite(buf, 4, chunk, f);
    }
    fclose(f);
    return 0;
}
