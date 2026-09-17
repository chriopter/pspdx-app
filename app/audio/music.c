/*
 * The tune: a dark blue room. Six chords in a slow cycle of about a
 * minute, none of them the same length, on pads that take three seconds
 * to arrive and a dozen to go, so a chord is a wash that the next one
 * comes up through rather than a change. Each chord's four voices arrive
 * one after another, seconds apart, in an order that differs from chord
 * to chord; a sub sits under each root; four or five times a minute a
 * high, quiet light swells and fades far off to one side; eight times a
 * minute a cold bell, at the back of a long hall, on a tone of the chord
 * that is sounding. Nothing else is struck. There is no beat.
 *
 * The cycle is written out twice with different orders and different
 * lights, so the loop is two minutes long and the voices' own slow swells
 * and the long envelopes put a different face on every pass.
 */

#include "audio/music.h"
#include "audio/cues.h"
#include "audio/synth.h"

/* Root for the sub, then four pad voices low to high. Roots stay between
   E1 and C#2: a floor, not a bass line. */
struct chord { float seconds; unsigned char root; unsigned char pad[4]; };

static const struct chord CYCLE[] = {
    /* E add9        */ { 11.0f, 28, { 52, 59, 66, 68 } },
    /* C#m11         */ {  9.5f, 37, { 49, 56, 64, 66 } },
    /* A maj9 #11    */ { 12.0f, 33, { 52, 56, 63, 71 } },
    /* F#m9          */ { 10.0f, 30, { 57, 61, 64, 68 } },
    /* B add9        */ { 10.5f, 35, { 54, 61, 63, 71 } },
    /* G#m11         */ { 11.0f, 32, { 56, 59, 63, 66 } },
};
#define CHORDS ((int)(sizeof(CYCLE) / sizeof(*CYCLE)))
#define PASSES 2

/* When, in seconds after the chord starts, each of the four voices comes
   in; the rows are permutations, one per chord per pass. The lowest voice
   is never last, so a chord has a bottom before it has a top. */
static const float ENTRY[PASSES][CHORDS][4] = {
    { { 0.0f, 1.6f, 3.1f, 4.7f }, { 1.3f, 0.0f, 4.2f, 2.6f }, { 0.4f, 3.5f, 1.9f, 5.0f },
      { 0.0f, 2.8f, 1.2f, 4.4f }, { 1.8f, 0.0f, 3.4f, 4.9f }, { 0.6f, 2.2f, 4.6f, 3.3f } },
    { { 0.9f, 3.0f, 1.5f, 4.4f }, { 0.0f, 2.4f, 1.1f, 3.9f }, { 1.5f, 0.0f, 4.7f, 3.0f },
      { 0.3f, 1.9f, 4.8f, 3.2f }, { 0.0f, 3.7f, 2.1f, 5.1f }, { 1.2f, 0.0f, 2.9f, 4.3f } },
};

/* The lights: which chord, how many seconds in, which pad voice two or
   three octaves up, how loud (of a hundred), and which side. */
static const struct { unsigned char chord; float at; unsigned char voice, octaves, vel; signed char side; }
    LIGHT[PASSES][5] = {
    { { 0, 6.2f, 2, 2, 5, -1 }, { 1, 4.8f, 3, 2, 4, 1 }, { 2, 7.5f, 1, 3, 4, 1 },
      { 3, 3.9f, 2, 2, 5, -1 }, { 5, 6.6f, 3, 2, 4, 1 } },
    { { 0, 8.1f, 3, 2, 4, 1 }, { 2, 3.3f, 2, 2, 5, -1 }, { 3, 6.9f, 1, 3, 4, -1 },
      { 4, 5.4f, 3, 2, 4, 1 }, { 5, 2.7f, 2, 2, 5, -1 } },
};

/* The bells, per pass: seconds into the pass, the note (a tone of the
   chord sounding then, between D5 and A5), how loud (thousandths), and
   which side. The gaps are all different, one pair comes close together,
   and the sides alternate. */
static const struct { float at; unsigned char note; unsigned short vel; signed char pan; }
    BELL[PASSES][8] = {
    { {  4.7f, 78, 100, -70 }, { 13.9f, 76,  80,  80 }, { 23.1f, 80, 105, -50 }, { 24.0f, 75,  70,  60 },
      { 35.6f, 81,  95,  70 }, { 44.9f, 75,  85, -80 }, { 50.2f, 78,  75,  50 }, { 58.3f, 80,  90, -60 } },
    { {  7.3f, 80,  90,  60 }, { 12.6f, 78,  80, -80 }, { 27.8f, 76, 100,  50 }, { 36.9f, 78,  85, -60 },
      { 41.1f, 76,  75,  80 }, { 47.4f, 78,  95, -50 }, { 48.6f, 75,  70,  70 }, { 60.8f, 75,  90, -70 } },
};

static int g_rate;
static long g_pos;              /* sample position inside the loop */
static long g_loop;             /* loop length in samples */

struct event { long at; unsigned char note, timbre; unsigned short vel; signed char pan; };  /* vel in thousandths */
#define MAX_EVENTS 256
static struct event g_events[MAX_EVENTS];
static int g_event_count;
static int g_next;

static void add(float at_s, int note, int vel, enum synth_timbre timbre, float pan) {
    if (g_event_count >= MAX_EVENTS) return;
    struct event *e = &g_events[g_event_count++];
    e->at = (long)(at_s * g_rate);
    e->note = (unsigned char)note;
    e->vel = (unsigned short)vel;
    e->timbre = (unsigned char)timbre;
    e->pan = (signed char)(pan * 100);
}

static void sort_events(void) {
    for (int i = 1; i < g_event_count; i++) {
        struct event e = g_events[i];
        int j = i - 1;
        while (j >= 0 && g_events[j].at > e.at) { g_events[j + 1] = g_events[j]; j--; }
        g_events[j + 1] = e;
    }
}

/* The four voices sit across the room, the outer two wide, and swap sides
   from one chord to the next so the weight moves. */
static const float PAN[2][4] = { { -0.8f, 0.45f, -0.35f, 0.8f }, { 0.8f, -0.45f, 0.35f, -0.8f } };

void music_init(int sample_rate) {
    g_rate = sample_rate;
    g_pos = 0;
    g_next = 0;
    g_event_count = 0;

    float t = 0.0f;
    for (int pass = 0; pass < PASSES; pass++) {
        float pass_start = t;
        for (int c = 0; c < CHORDS; c++) {
            const struct chord *ch = &CYCLE[c];
            /* The sub leads by a breath; being slowest to arrive it still
               comes up under the pads rather than before them. */
            add(t - 0.5f < 0 ? 0 : t - 0.5f, ch->root, 260, SYNTH_SUB, 0.0f);
            for (int v = 0; v < 4; v++)
                add(t + ENTRY[pass][c][v], ch->pad[v], v == 3 ? 230 : 260, SYNTH_DRIFT,
                    PAN[(pass * CHORDS + c) & 1][v]);
            t += ch->seconds;
        }
        for (int i = 0; i < 5; i++) {
            float at = pass_start;
            for (int c = 0; c < LIGHT[pass][i].chord; c++) at += CYCLE[c].seconds;
            const struct chord *ch = &CYCLE[LIGHT[pass][i].chord];
            add(at + LIGHT[pass][i].at, ch->pad[LIGHT[pass][i].voice] + 12 * LIGHT[pass][i].octaves,
                LIGHT[pass][i].vel * 10, SYNTH_SHIMMER, 0.9f * LIGHT[pass][i].side);
        }
        for (int i = 0; i < 8; i++)
            add(pass_start + BELL[pass][i].at, BELL[pass][i].note, BELL[pass][i].vel, SYNTH_BELL,
                BELL[pass][i].pan / 100.0f);
    }
    g_loop = (long)(t * g_rate);
    sort_events();
}

void music_render(short *out, int frames) {
    cues_drain();
    while (frames > 0) {
        long until = g_next < g_event_count ? g_events[g_next].at - g_pos
                                             : g_loop - g_pos;
        if (until <= 0) {
            if (g_next < g_event_count) {
                const struct event *e = &g_events[g_next++];
                synth_strike(e->note, e->vel / 1000.0f, (enum synth_timbre)e->timbre,
                             e->pan / 100.0f, 1);
            } else {
                g_pos = 0;
                g_next = 0;
            }
            continue;
        }
        int n = until < frames ? (int)until : frames;
        synth_render(out, n);
        out += n * 2;
        frames -= n;
        g_pos += n;
    }
}
