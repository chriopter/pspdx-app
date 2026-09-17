#include <math.h>
#include <string.h>

#include "audio/synth.h"

#define TABLE_BITS 11
#define TABLE_SIZE (1 << TABLE_BITS)
#define PARTIALS 4
#define BLOCK 32                /* samples between envelope updates */
#define MIDI_NOTES 128

/* The room the instruments stand in: two feedback delays that are not
   multiples of each other, so their echoes do not line up into a flutter. */
#define ECHO_A 13107            /* 0.297 s at 44.1 kHz */
#define ECHO_B 16361            /* 0.371 s */

static float g_table[TABLE_SIZE];
static int g_rate;

struct timbre {
    float ratio[PARTIALS];          /* partial frequency multiples; a hair off
                                       a whole number is a detuned copy */
    float level[PARTIALS];          /* relative loudness */
    unsigned char power[PARTIALS];  /* envelope raised to this: over sooner */
    float attack_s, decay_s;        /* 0 attack = struck; decay per pitch below */
    unsigned char decay_by_pitch;
    /* Wide: the first two partials lean left, the last two right. With a
       sharp copy on one side and a flat one on the other the two halves of
       the room drift against each other, which is what a chorus does. */
    unsigned char wide;
    float lfo_hz, lfo_depth;        /* a slow swell in the level, 0 = none */
    float dry, send;                /* heard direct, and put into the hall */
};

static const struct timbre TIMBRES[] = {
    /* Six milliseconds of attack: a hammer, not a step. A step from nothing
       to full inside one sample is a click, and was audible as one. */
    [SYNTH_PIANO] = { { 1, 2, 3, 4 }, { 1.00f, 0.42f, 0.18f, 0.08f }, { 1, 2, 2, 3 }, 0.006f, 2.4f, 1, 0, 0, 0, 1.0f, 0 },
    [SYNTH_GLASS] = { { 1, 2, 3, 5 }, { 1.00f, 0.22f, 0.10f, 0.04f }, { 1, 1, 1, 1 }, 0.04f, 2.6f, 0, 0, 0, 0, 1.0f, 0 },
    [SYNTH_PAD]   = { { 1, 2, 3, 4 }, { 1.00f, 0.35f, 0.12f, 0.05f }, { 1, 1, 2, 2 }, 0.9f,  5.5f, 0, 0, 0, 0, 1.0f, 0 },
    /* Exact harmonic ratios survive the PSP's closely spaced speakers
       without the slow acoustic cancellation of detuned stereo copies.
       The envelopes already provide motion; an amplitude LFO on every long
       voice made the whole mix pump or "bubble" every few seconds. */
    [SYNTH_DRIFT]   = { { 1, 2, 3, 4 }, { 1.00f, 0.24f, 0.08f, 0.03f },
                        { 1, 2, 2, 3 }, 3.0f, 12.0f, 0, 1, 0, 0, 1.0f, 0 },
    [SYNTH_SUB]     = { { 1, 2, 3, 4 }, { 1.00f, 0.25f, 0.08f, 0.02f },
                        { 1, 2, 2, 3 }, 3.5f, 15.0f, 0, 0, 0, 0, 1.0f, 0 },
    [SYNTH_SHIMMER] = { { 1, 2, 3, 4 }, { 1.00f, 0.12f, 0.05f, 0.02f },
                        { 1, 2, 2, 3 }, 1.8f, 5.0f, 0, 1, 0, 0, 1.0f, 0 },
    /* Cold and inharmonic; the top of it goes first. A quarter of it is
       heard direct, the rest through the hall, so the bloom is louder than
       the strike and carries the tail. */
    [SYNTH_BELL]    = { { 1.0f, 2.0f, 2.756f, 5.404f }, { 1.00f, 0.50f, 0.35f, 0.10f },
                        { 1, 1, 1, 2 }, 0.03f, 1.6f, 0, 0, 0, 0, 0.25f, 0.90f },
    /* The same glass for the keys, damped: half the ring, a third of the
       hall, so a run of presses is a run and not a wash. */
    [SYNTH_CHIME]   = { { 1.0f, 2.0f, 2.756f, 5.404f }, { 1.00f, 0.45f, 0.28f, 0.06f },
                        { 1, 1, 2, 2 }, 0.03f, 0.8f, 0, 0, 0, 0, 0.30f, 0.30f },
};
#define TIMBRE_COUNT ((int)(sizeof(TIMBRES) / sizeof(*TIMBRES)))

/* Everything libm-heavy is built before the audio thread starts. A held key
   can post several strikes in one chunk, and the breathing filter advances
   once every 32 samples; neither path should call powf/expf at playback. */
static float g_note_inc[MIDI_NOTES];
static float g_decay[TIMBRE_COUNT][MIDI_NOTES];

struct voice {
    /* One phase per partial, 32-bit fixed point with the table index on
       top. A partial's own step is the fundamental's times its ratio, which
       is the same number the multiply used to make every sample. */
    unsigned phase[PARTIALS], inc[PARTIALS];
    float env;                  /* fundamental's envelope, 1 at the strike */
    float peak;                 /* where the attack is heading */
    float attack;               /* per-block rise while env < peak, 0 = struck */
    float decay;                /* per-block multiplier once past the peak */
    float gain_l, gain_r, send; /* direct each side, and into the hall */
    float gain[PARTIALS];       /* this block's level per partial */
    unsigned lfo_phase, lfo_inc;/* the swell, stepped per block */
    float lfo_depth;
    int pair;                   /* the upper two are under hearing this block */
    const struct timbre *t;
    int active;
    int music;                  /* the tune, as opposed to the interface */
};

static struct voice g_voices[SYNTH_VOICES];
static float g_echo_a[ECHO_A], g_echo_b[ECHO_B];
static int g_echo_pos_a, g_echo_pos_b;
static float g_echo_lp;

/* The tune's bus: a fixed two-pole low-pass. The interface's sounds do not
   go through it. Keeping this deterministic also avoids turning deliberately
   generated noise into speaker hiss that can be mistaken for crackle. */
static float g_bus_lp[4];       /* l1 l2 r1 r2 */
static float g_bus_filter_a;

static float midi_hz(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

void synth_init(int sample_rate) {
    g_rate = sample_rate;
    for (int i = 0; i < TABLE_SIZE; i++)
        g_table[i] = sinf((float)i / TABLE_SIZE * 6.28318530f);
    float blocks_per_s = (float)g_rate / BLOCK;
    for (int note = 0; note < MIDI_NOTES; note++) {
        g_note_inc[note] = midi_hz(note) / g_rate * 4294967296.0f;
        for (int timbre = 0; timbre < TIMBRE_COUNT; timbre++) {
            const struct timbre *t = &TIMBRES[timbre];
            float seconds = t->decay_s;
            if (t->decay_by_pitch) {
                seconds *= powf(0.5f, (note - 48) / 24.0f);
                if (seconds < 0.35f) seconds = 0.35f;
            }
            g_decay[timbre][note] = expf(-1.0f / (seconds * blocks_per_s));
        }
    }
    g_bus_filter_a = 1.0f - expf(-6.2831853f * 1600.0f / g_rate);
    memset(g_voices, 0, sizeof(g_voices));
    memset(g_echo_a, 0, sizeof(g_echo_a));
    memset(g_echo_b, 0, sizeof(g_echo_b));
    g_echo_pos_a = g_echo_pos_b = 0;
    g_echo_lp = 0.0f;
    memset(g_bus_lp, 0, sizeof(g_bus_lp));
}

/* The tune's level and where it is heading; moved once per block. Two
   seconds up, a second and a half down. */
static float g_music_level = 1.0f;
static volatile float g_music_target = 1.0f;

void synth_set_music_level(float level) {
    g_music_target = level < 0 ? 0 : level > 1 ? 1 : level;
}

static inline float sine(unsigned phase) {
    return g_table[phase >> (32 - TABLE_BITS)];
}

void synth_strike(int note, float velocity, enum synth_timbre timbre, float pan,
                  int music) {
    struct voice *v = 0;
    for (int i = 0; i < SYNTH_VOICES; i++)
        if (!g_voices[i].active) { v = &g_voices[i]; break; }
    if (!v) {
        v = &g_voices[0];
        for (int i = 1; i < SYNTH_VOICES; i++)
            if (g_voices[i].env < v->env) v = &g_voices[i];
    }
    const struct timbre *t = &TIMBRES[timbre];
    float blocks_per_s = (float)g_rate / BLOCK;
    v->t = t;
    float inc = note >= 0 && note < MIDI_NOTES
              ? g_note_inc[note]
              : midi_hz(note) / g_rate * 4294967296.0f;
    for (int p = 0; p < PARTIALS; p++) {
        v->phase[p] = 0;
        v->inc[p] = (unsigned)(inc * t->ratio[p]);
    }
    v->peak = velocity;
    if (t->attack_s > 0.0f) {
        v->env = 0.0f;
        v->attack = velocity / (t->attack_s * blocks_per_s);
    } else {
        v->env = velocity;
        v->attack = 0.0f;
    }
    if (note >= 0 && note < MIDI_NOTES) {
        v->decay = g_decay[timbre][note];
    } else {
        float seconds = t->decay_s;
        if (t->decay_by_pitch) {
            seconds *= powf(0.5f, (note - 48) / 24.0f);
            if (seconds < 0.35f) seconds = 0.35f;
        }
        v->decay = expf(-1.0f / (seconds * blocks_per_s));
    }
    /* Each voice swells at its own pace, set by the note, so a chord's
       voices breathe against each other rather than together. */
    v->lfo_depth = t->lfo_depth;
    v->lfo_phase = (unsigned)note * 0x2D000000u;
    v->lfo_inc = (unsigned)(t->lfo_hz * (1.0f + 0.12f * ((note * 7) % 5)) / blocks_per_s * 4294967296.0f);
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    v->gain_l = (0.5f + 0.5f * (1.0f - pan) * 0.5f + 0.25f * (pan < 0 ? -pan : 0)) * t->dry;
    v->gain_r = (0.5f + 0.5f * (1.0f + pan) * 0.5f + 0.25f * (pan > 0 ? pan : 0)) * t->dry;
    v->send = t->send;
    v->music = music;
    v->active = 1;
}

static inline float soft_clip(float x) {
    /* Gentle above +-0.7, hard wall at +-1. */
    if (x > 0.7f) x = 0.7f + (x - 0.7f) / (1.0f + (x - 0.7f) * 3.0f);
    else if (x < -0.7f) x = -0.7f + (x + 0.7f) / (1.0f - (x + 0.7f) * 3.0f);
    return x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
}

/* Envelopes move once per block; inside a block a voice is two or four
   table lookups and as many multiply-adds per sample, which is what the CPU
   can afford at 44.1 kHz. */
static void voice_block(struct voice *v) {
    if (v->attack > 0.0f) {
        v->env += v->attack;
        if (v->env >= v->peak) { v->env = v->peak; v->attack = 0.0f; }
    } else {
        v->env *= v->decay;
        if (v->env < 0.003f) { v->active = 0; return; }
    }
    float e = v->env;
    if (v->lfo_depth > 0.0f) {
        /* Between 1 - depth and 1: the swell only ever takes away. */
        v->lfo_phase += v->lfo_inc;
        e *= 1.0f - v->lfo_depth * 0.5f * (1.0f - sine(v->lfo_phase));
    }
    float e2 = e * e, e3 = e2 * e;
    for (int p = 0; p < PARTIALS; p++) {
        unsigned char pw = v->t->power[p];
        v->gain[p] = v->t->level[p] * (pw >= 3 ? e3 : pw == 2 ? e2 : e);
    }
    /* The upper partials are quieter and die sooner, so for most of a
       note's life the third is already under the last bit of the output --
       and the fourth with it. Then the block is two partials wide. */
    v->pair = v->gain[2] < 1e-5f;
}

void synth_render(short *out, int frames) {
    while (frames > 0) {
        int n = frames < BLOCK ? frames : BLOCK;
        float mix_l[BLOCK], mix_r[BLOCK];       /* the interface, straight to the room */
        float bus_l[BLOCK], bus_r[BLOCK];       /* the tune, through the filter first */
        memset(mix_l, 0, sizeof(mix_l));
        memset(mix_r, 0, sizeof(mix_r));
        memset(bus_l, 0, sizeof(bus_l));
        memset(bus_r, 0, sizeof(bus_r));

        {
            float step = (float)BLOCK / g_rate;
            float rate = g_music_target < g_music_level ? 1.0f / 1.5f : 1.0f / 2.0f;
            if (g_music_level < g_music_target) {
                g_music_level += step * rate;
                if (g_music_level > g_music_target) g_music_level = g_music_target;
            } else if (g_music_level > g_music_target) {
                g_music_level -= step * rate;
                if (g_music_level < g_music_target) g_music_level = g_music_target;
            }
        }
        int any_music = 0;
        for (int i = 0; i < SYNTH_VOICES; i++) {
            struct voice *v = &g_voices[i];
            if (!v->active) continue;
            voice_block(v);
            if (!v->active) continue;
            float level = v->music ? g_music_level : 1.0f;
            if (level <= 0.0005f) {
                /* Ducking silences the bus, not time. Resume at the phase the
                   sustained note would have reached instead of restarting a
                   frozen waveform at an unrelated crossing. */
                for (int p = 0; p < PARTIALS; p++)
                    v->phase[p] += (unsigned)n * v->inc[p];
                continue;
            }
            float *ml = v->music ? bus_l : mix_l, *mr = v->music ? bus_r : mix_r;
            any_music |= v->music;
            unsigned p0 = v->phase[0], p1 = v->phase[1];
            unsigned i0 = v->inc[0], i1 = v->inc[1];
            float g0 = v->gain[0], g1 = v->gain[1];
            float gl = v->gain_l * level, gr = v->gain_r * level;
            /* Two partials retain the body of every timbre. The upper pair
               cost as much again across every sustained chord and contribute
               little after the 1.6-kHz bus filter and PSP speakers. */
            for (int f = 0; f < n; f++) {
                float s = sine(p0) * g0 + sine(p1) * g1;
                p0 += i0; p1 += i1;
                ml[f] += s * gl;
                mr[f] += s * gr;
            }
            v->phase[2] += (unsigned)n * v->inc[2];
            v->phase[3] += (unsigned)n * v->inc[3];
            v->phase[0] = p0; v->phase[1] = p1;
        }

        /* Fixed gentle low-pass; no random "air" under the signal. */
        if (any_music || g_music_level > 0.0005f) {
            float a = g_bus_filter_a;
            float l1 = g_bus_lp[0], l2 = g_bus_lp[1], r1 = g_bus_lp[2], r2 = g_bus_lp[3];
            for (int f = 0; f < n; f++) {
                l1 += (bus_l[f] - l1) * a; l2 += (l1 - l2) * a;
                r1 += (bus_r[f] - r1) * a; r2 += (r1 - r2) * a;
                mix_l[f] += l2;
                mix_r[f] += r2;
            }
            g_bus_lp[0] = l1; g_bus_lp[1] = l2; g_bus_lp[2] = r1; g_bus_lp[3] = r2;
        }

        /* The stereo room below supplies the tail without a second bank of
           ten floating-point delay lines. */
        float hall_l[BLOCK], hall_r[BLOCK];
        memset(hall_l, 0, sizeof(hall_l));
        memset(hall_r, 0, sizeof(hall_r));

        /* Two echoes fed with a mono sum, darkened a little each pass, and
           laid back in: a room with some depth to it. Neither delay wraps
           often, so the run up to the nearer wrap is taken in one go and
           the two tests come out of the inner loop. */
        for (int f = 0; f < n; ) {
            int m = n - f;
            if (m > ECHO_A - g_echo_pos_a) m = ECHO_A - g_echo_pos_a;
            if (m > ECHO_B - g_echo_pos_b) m = ECHO_B - g_echo_pos_b;
            float *da = &g_echo_a[g_echo_pos_a], *db = &g_echo_b[g_echo_pos_b];
            float lp = g_echo_lp;
            for (int k = 0; k < m; k++, f++) {
                float l = mix_l[f] * 0.60f, r = mix_r[f] * 0.60f;
                float mono = (l + r) * 0.5f;
                float ea = da[k], eb = db[k];
                lp += ((ea + eb) * 0.5f - lp) * 0.30f;
                da[k] = mono + lp * 0.50f;
                db[k] = mono + lp * 0.46f;
                l += ea * 0.30f + eb * 0.20f + hall_l[f];
                r += eb * 0.30f + ea * 0.20f + hall_r[f];
                out[f * 2 + 0] = (short)(soft_clip(l) * 32000.0f);
                out[f * 2 + 1] = (short)(soft_clip(r) * 32000.0f);
            }
            g_echo_lp = lp;
            if ((g_echo_pos_a += m) >= ECHO_A) g_echo_pos_a = 0;
            if ((g_echo_pos_b += m) >= ECHO_B) g_echo_pos_b = 0;
        }
        out += n * 2;
        frames -= n;
    }
}
