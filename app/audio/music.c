/*
 * The tune: a lit room at night. Under it, six modal chords from one
 * scale, D dorian with a lydian turn on the B flat, none of them a
 * dominant, so nothing ever resolves and nothing has to. Each is a haze of
 * three voices and a root, twenty-odd seconds long, swelling in over
 * seconds and closing as it fades, so a change of chord is a change of
 * light. The cycle is written out twice, the second time with other
 * voicings, other entries and two of the roots left out: four and a half
 * minutes before the bed comes round.
 *
 * Over it, four short phrases on an electric piano, one to three notes
 * each, every one on a loop of its own length -- 23.3, 29.9, 37.1 and
 * 44.3 seconds -- and each sitting out some of its rounds. They never line
 * up the same way twice in an evening, and since every note is from the
 * pentatonic that all six chords share, no meeting of them is wrong. Now
 * and then, three times a pass, a glass far off. Everything else is the
 * room. There is no beat.
 */

#include "audio/music.h"
#include "audio/cues.h"
#include "audio/synth.h"

/* Root, then three voices low to high, one voicing per pass. Roots stay
   between A1 and G2; the voices stay at or under A4, mostly under the
   phrases, so the only thing ever a semitone from a phrase is a seventh
   under it. */
struct chord { float seconds; unsigned char root; unsigned char voice[2][3]; };

static const struct chord CYCLE[] = {
    /* D: A C E, then A D F */            { 26.0f, 38, { { 57, 60, 64 }, { 57, 62, 65 } } },
    /* B flat: A D E, then F A D */       { 21.0f, 34, { { 57, 62, 64 }, { 53, 57, 62 } } },
    /* F: C E G, then A E G */            { 23.5f, 41, { { 60, 64, 67 }, { 57, 64, 67 } } },
    /* C: G D A, then E G D */            { 19.0f, 36, { { 55, 62, 69 }, { 52, 55, 62 } } },
    /* A minor: G C D, then E G C */      { 25.0f, 33, { { 55, 60, 62 }, { 52, 55, 60 } } },
    /* G: A C F, then D A C */            { 22.5f, 43, { { 57, 60, 65 }, { 50, 57, 60 } } },
};
#define CHORDS ((int)(sizeof(CYCLE) / sizeof(*CYCLE)))
#define PASSES 2

/* When, in seconds after the chord starts, each voice comes in; the lowest
   is never last, so a chord has a bottom before it has a top. */
static const float ENTRY[PASSES][CHORDS][3] = {
    { { 0.0f, 2.2f, 4.6f }, { 1.4f, 0.0f, 3.8f }, { 0.0f, 3.1f, 1.6f },
      { 0.6f, 4.2f, 2.0f }, { 0.0f, 1.8f, 5.2f }, { 1.1f, 0.0f, 3.4f } },
    { { 0.8f, 0.0f, 3.6f }, { 0.0f, 4.4f, 2.1f }, { 1.7f, 0.0f, 4.9f },
      { 0.0f, 2.6f, 1.3f }, { 0.9f, 3.9f, 0.0f }, { 0.0f, 1.5f, 4.1f } },
};

/* The voices sit across the room and swap sides from one chord to the
   next, so the weight moves; the root is in the middle. */
static const float PAN[2][3] = { { -0.6f, 0.5f, -0.2f }, { 0.6f, -0.5f, 0.2f } };

/* In the second pass these chords have no root: room under them. */
#define BARE(c) ((c) == 1 || (c) == 4)

/* The far glass, per pass: seconds in, the note, how loud (thousandths),
   which side. Each on a tone of the chord or a ninth over it. */
static const struct { float at; unsigned char note; unsigned short vel; signed char pan; }
    FAR[PASSES][3] = {
    { { 17.5f, 81, 210, -80 }, { 58.0f, 79, 180, 70 }, { 103.5f, 84, 160, -60 } },
    { {  9.0f, 76, 190,  75 }, { 50.0f, 84, 160, -70 }, { 121.0f, 81, 200, 60 } },
};

/* The phrases: how long each loop is, when it first comes in, which of
   its rounds it sits out (bit r of rests: rounds where round % period is
   r), which side it is on, and its notes in seconds from the top of the
   loop. All of them from G A C D E. */
#define PHRASE_NOTES 3
static const struct phrase {
    float seconds, start;
    unsigned char period, rests;
    signed char pan;
    unsigned char count;
    struct { float at; unsigned char note; unsigned short vel; } note[PHRASE_NOTES];
} PHRASES[] = {
    /* A fourth up. */
    { 23.3f,  7.0f, 4, 1 << 3, -55, 2, { { 0.0f, 69, 340 }, { 1.6f, 76, 220 } } },
    /* One note, alone. */
    { 29.9f, 15.2f, 3, 1 << 2,  60, 1, { { 0.0f, 74, 290 } } },
    /* Three, rising and falling back. */
    { 37.1f, 24.0f, 5, 1 << 1 | 1 << 3, -15, 3, { { 0.0f, 67, 290 }, { 2.4f, 72, 220 }, { 5.1f, 69, 180 } } },
    /* A step down, high. */
    { 44.3f, 36.5f, 2, 1 << 1,  75, 2, { { 0.0f, 76, 220 }, { 1.1f, 74, 180 } } },
};
#define PHRASE_COUNT ((int)(sizeof(PHRASES) / sizeof(*PHRASES)))

/* A phrase is not played the same each round: its loudness leans on this
   in turn, each phrase from a different place in it. */
static const float SWAY[5] = { 1.0f, 0.78f, 0.92f, 0.70f, 0.86f };

static int g_rate;

struct event { long at; unsigned char note, timbre; unsigned short vel; signed char pan; };  /* vel in thousandths */
#define MAX_EVENTS 128
static struct event g_events[MAX_EVENTS];
static int g_event_count;

/* The bed is one track and each phrase another, each going round on its
   own. pos starts below zero for a phrase that has not come in yet. */
struct track {
    int first, count;           /* its events in g_events, in time order */
    long loop, pos;             /* samples */
    int next;
    unsigned round;
    unsigned char period, rests, sway;
};
#define TRACKS (1 + PHRASE_COUNT)
static struct track g_tracks[TRACKS];

static void add(float at_s, int note, int vel, enum synth_timbre timbre, float pan) {
    if (g_event_count >= MAX_EVENTS) return;
    struct event *e = &g_events[g_event_count++];
    e->at = (long)(at_s * g_rate);
    e->note = (unsigned char)note;
    e->vel = (unsigned short)vel;
    e->timbre = (unsigned char)timbre;
    e->pan = (signed char)(pan * 100);
}

static void sort_events(struct event *ev, int count) {
    for (int i = 1; i < count; i++) {
        struct event e = ev[i];
        int j = i - 1;
        while (j >= 0 && ev[j].at > e.at) { ev[j + 1] = ev[j]; j--; }
        ev[j + 1] = e;
    }
}

void music_init(int sample_rate) {
    g_rate = sample_rate;
    g_event_count = 0;

    float t = 0.0f;
    for (int pass = 0; pass < PASSES; pass++) {
        float pass_start = t;
        for (int c = 0; c < CHORDS; c++) {
            const struct chord *ch = &CYCLE[c];
            /* The root leads by a breath; being slowest to arrive it still
               comes up under the voices rather than before them. */
            if (!(pass == 1 && BARE(c)))
                add(t - 0.5f < 0 ? 0 : t - 0.5f, ch->root, 150, SYNTH_ROOT, 0.0f);
            for (int v = 0; v < 3; v++)
                add(t + ENTRY[pass][c][v], ch->voice[pass][v], v == 2 ? 105 : 125, SYNTH_HAZE,
                    PAN[(pass * CHORDS + c) & 1][v]);
            t += ch->seconds;
        }
        for (int i = 0; i < 3; i++)
            add(pass_start + FAR[pass][i].at, FAR[pass][i].note, FAR[pass][i].vel, SYNTH_FAR,
                FAR[pass][i].pan / 100.0f);
    }
    struct track *bed = &g_tracks[0];
    bed->first = 0;
    bed->count = g_event_count;
    bed->loop = (long)(t * g_rate);
    bed->pos = 0;
    bed->period = 1;
    bed->rests = 0;
    bed->sway = 0;
    sort_events(g_events, g_event_count);

    for (int p = 0; p < PHRASE_COUNT; p++) {
        const struct phrase *ph = &PHRASES[p];
        struct track *tr = &g_tracks[1 + p];
        tr->first = g_event_count;
        for (int i = 0; i < ph->count; i++)
            add(ph->note[i].at, ph->note[i].note, ph->note[i].vel, SYNTH_KEYS, ph->pan / 100.0f);
        tr->count = g_event_count - tr->first;
        tr->loop = (long)(ph->seconds * g_rate);
        tr->pos = -(long)(ph->start * g_rate);
        tr->period = ph->period;
        tr->rests = ph->rests;
        tr->sway = (unsigned char)(p * 2);
    }
    for (int i = 0; i < TRACKS; i++) {
        g_tracks[i].next = 0;
        g_tracks[i].round = 0;
    }
}

/* Plays a track's next event, or takes it round to the top. */
static void step(struct track *tr) {
    if (tr->next < tr->count) {
        const struct event *e = &g_events[tr->first + tr->next++];
        if (tr->rests & (1u << (tr->round % tr->period))) return;
        float vel = e->vel / 1000.0f;
        if (tr != &g_tracks[0]) vel *= SWAY[(tr->round + tr->sway) % 5];
        synth_strike(e->note, vel, (enum synth_timbre)e->timbre, e->pan / 100.0f, 1);
    } else {
        tr->pos = 0;
        tr->next = 0;
        tr->round++;
    }
}

void music_render(short *out, int frames) {
    cues_drain();
    while (frames > 0) {
        long until = frames;
        int due = -1;
        for (int i = 0; i < TRACKS && due < 0; i++) {
            const struct track *tr = &g_tracks[i];
            long d = (tr->next < tr->count ? g_events[tr->first + tr->next].at : tr->loop) - tr->pos;
            if (d <= 0) due = i;
            else if (d < until) until = d;
        }
        if (due >= 0) {
            step(&g_tracks[due]);
            continue;
        }
        int n = (int)until;
        synth_render(out, n);
        out += n * 2;
        frames -= n;
        for (int i = 0; i < TRACKS; i++) g_tracks[i].pos += n;
    }
}
