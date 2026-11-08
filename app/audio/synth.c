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
#define ECHO_A 13107            /* 1.19 s at the quarter rate audio.c runs us at */
#define ECHO_B 16361            /* 1.48 s */

/* The tune's way into the room: three allpasses in a row, prime lengths
   from 12 to 60 ms, so a note reaches the echoes as a cloud rather than a
   copy. The interface goes in as it is. */
#define DIFFUSE_A 131
#define DIFFUSE_B 379
#define DIFFUSE_C 661

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
    /* The tune's instruments are two-operator FM instead: the second
       partial is not heard but bends the phase of the first, by an index
       that is a filter in all but name -- near 0 a sine, near 1 a soft saw,
       past 2 a bell. The index rests at index_rest, opens with the level,
       breathes by bright_depth at bright_hz, and a strike adds index_strike
       that dies in index_s: the bright edge of a struck tine. A third
       partial with a level is a plain sine a few cents off the carrier:
       two oscillators beating slowly in one channel, the warmth of a
       detuned pair without the stereo copies that cancelled in the air
       between the PSP's speakers. */
    unsigned char fm;
    float index_rest, index_strike, index_s, bright_hz, bright_depth;
    float hold_s;                   /* at the peak this long before the decay */
};

static const struct timbre TIMBRES[] = {
    /* Six milliseconds of attack: a hammer, not a step. A step from nothing
       to full inside one sample is a click, and was audible as one. */
    [SYNTH_PIANO] = { { 1, 2, 3, 4 }, { 1.00f, 0.42f, 0.18f, 0.08f }, { 1, 2, 2, 3 }, 0.006f, 2.4f, 1, 0, 0, 0, 1.0f, 0 },
    [SYNTH_GLASS] = { { 1, 2, 3, 5 }, { 1.00f, 0.22f, 0.10f, 0.04f }, { 1, 1, 1, 1 }, 0.04f, 2.6f, 0, 0, 0, 0, 1.0f, 0 },
    [SYNTH_PAD]   = { { 1, 2, 3, 4 }, { 1.00f, 0.35f, 0.12f, 0.05f }, { 1, 1, 2, 2 }, 0.9f,  5.5f, 0, 0, 0, 0, 1.0f, 0 },
    /* The bed: a carrier with an unheard modulator at its own pitch and a
       sine twin seven cents sharp. Dark at rest, opening as it swells,
       holding, closing as it fades, and breathing on a cycle of about
       twenty seconds -- a filter moving, not a bow. The hold keeps a chord
       full until the next is up, so a change is not a hole. */
    [SYNTH_HAZE] = { .ratio = { 1, 1, 1.00405f }, .level = { 1.00f, 0, 0.42f }, .power = { 1, 1, 1 },
                     .attack_s = 4.5f, .hold_s = 11.0f, .decay_s = 10.0f, .dry = 1.0f, .send = 0.55f,
                     .fm = 1, .index_rest = 1.0f, .bright_hz = 0.05f, .bright_depth = 0.45f },
    /* Under it, one low tone, nearly a sine, slower in and out than the
       bed; it beats against a twin three cents off, which at that pitch is
       a swell every few seconds. */
    [SYNTH_ROOT] = { .ratio = { 1, 1, 1.00173f }, .level = { 1.00f, 0, 0.55f }, .power = { 1, 1, 1 },
                     .attack_s = 6.0f, .hold_s = 12.0f, .decay_s = 12.0f, .dry = 1.0f, .send = 0.20f,
                     .fm = 1, .index_rest = 0.40f, .bright_hz = 0.03f, .bright_depth = 0.30f },
    /* The phrases: an electric piano of the DX kind. The strike opens the
       index for a third of a second and it settles to almost a sine, so
       the note is a soft tine and then a hum; half of it is heard through
       the room, which is where it repeats. */
    [SYNTH_KEYS] = { .ratio = { 1, 1 }, .level = { 1.00f }, .power = { 1 },
                     .attack_s = 0.010f, .decay_s = 3.4f, .decay_by_pitch = 1, .dry = 0.75f, .send = 0.85f,
                     .fm = 1, .index_rest = 0.22f, .index_strike = 1.8f, .index_s = 0.30f },
    /* Far off, now and then: glass at 1 : 1.4, which puts no partial on a
       harmonic, struck hard and closing over a second. A third heard
       direct, the rest through the room. */
    [SYNTH_FAR]  = { .ratio = { 1, 1.4f }, .level = { 1.00f }, .power = { 1 },
                     .attack_s = 0.01f, .decay_s = 4.5f, .dry = 0.30f, .send = 1.10f,
                     .fm = 1, .index_rest = 0.30f, .index_strike = 2.2f, .index_s = 1.0f },
    /* The interface's glass: short, so a run of presses is a run and not a
       wash. It keeps its own additive voice; the tune's instruments above
       were made to sit around it. */
    [SYNTH_CHIME]   = { { 1.0f, 2.0f, 2.756f, 5.404f }, { 1.00f, 0.45f, 0.28f, 0.06f },
                        { 1, 1, 2, 2 }, 0.03f, 0.8f, 0, 0, 0, 0, 0.30f, 0.30f },
};
#define TIMBRE_COUNT ((int)(sizeof(TIMBRES) / sizeof(*TIMBRES)))

/* Everything libm-heavy is built before the audio thread starts. A held key
   can post several strikes in one chunk, and the breathing filter advances
   once every 32 samples; neither path should call powf/expf at playback. */
static float g_note_inc[MIDI_NOTES];
static float g_decay[TIMBRE_COUNT][MIDI_NOTES];
static float g_index_decay[TIMBRE_COUNT];
/* Notes above C5 get less index as they go up: their sidebands would
   otherwise fold back off the 5.5-kHz ceiling as tones from no chord. */
static float g_index_key[MIDI_NOTES];

#define INDEX_MAX 3.0f
#define RADIAN 683565275.6f     /* one radian of phase, 2^32 / 2 pi */

/* After this long with nothing sounding the room has rung out; it is
   emptied and the echoes stop running until the next note. That keeps
   their tails from creeping down into subnormal floats under a film. */
#define QUIET_S 20

struct voice {
    /* One phase per partial, 32-bit fixed point with the table index on
       top. A partial's own step is the fundamental's times its ratio, which
       is the same number the multiply used to make every sample. */
    unsigned phase[PARTIALS], inc[PARTIALS];
    float env;                  /* fundamental's envelope, 1 at the strike */
    float peak;                 /* where the attack is heading */
    float attack;               /* per-block rise while env < peak, 0 = struck */
    float decay;                /* per-block multiplier once past the peak */
    int hold;                   /* blocks left at the peak before it */
    float gain_l, gain_r, send; /* direct each side, and into the room */
    float gain[PARTIALS];       /* this block's level per partial */
    unsigned lfo_phase, lfo_inc;/* the swell, stepped per block */
    float lfo_depth;
    /* FM only: what is left of the strike's index, the breathing, and this
       block's index as phase per unit of modulator. */
    float strike, strike_decay, key, inv_peak;
    unsigned bright_phase, bright_inc;
    float index;
    int pair;                   /* the upper two are under hearing this block */
    const struct timbre *t;
    int active;
    int music;                  /* the tune, as opposed to the interface */
};

static struct voice g_voices[SYNTH_VOICES];
static float g_echo_a[ECHO_A], g_echo_b[ECHO_B];
static int g_echo_pos_a, g_echo_pos_b;
static float g_echo_lp;
static float g_diffuse_a[DIFFUSE_A], g_diffuse_b[DIFFUSE_B], g_diffuse_c[DIFFUSE_C];
static int g_diffuse_pos_a, g_diffuse_pos_b, g_diffuse_pos_c;
static float g_wet_lp;
static int g_quiet;             /* blocks in a row with nothing sounding */

/* The tune's bus: a fixed two-pole low-pass. The interface's sounds do not
   go through it. Keeping this deterministic also avoids turning deliberately
   generated noise into speaker hiss that can be mistaken for crackle. */
static float g_bus_lp[4];       /* l1 l2 r1 r2 */
static float g_bus_filter_a, g_wet_filter_a;

static float midi_hz(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

static void room_clear(void) {
    memset(g_echo_a, 0, sizeof(g_echo_a));
    memset(g_echo_b, 0, sizeof(g_echo_b));
    memset(g_diffuse_a, 0, sizeof(g_diffuse_a));
    memset(g_diffuse_b, 0, sizeof(g_diffuse_b));
    memset(g_diffuse_c, 0, sizeof(g_diffuse_c));
    g_echo_lp = g_wet_lp = 0.0f;
}

void synth_init(int sample_rate) {
    g_rate = sample_rate;
    for (int i = 0; i < TABLE_SIZE; i++)
        g_table[i] = sinf((float)i / TABLE_SIZE * 6.28318530f);
    float blocks_per_s = (float)g_rate / BLOCK;
    for (int note = 0; note < MIDI_NOTES; note++) {
        g_note_inc[note] = midi_hz(note) / g_rate * 4294967296.0f;
        g_index_key[note] = note <= 72 ? 1.0f : powf(0.5f, (note - 72) / 12.0f);
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
    for (int timbre = 0; timbre < TIMBRE_COUNT; timbre++)
        g_index_decay[timbre] = TIMBRES[timbre].index_s > 0.0f
                              ? expf(-1.0f / (TIMBRES[timbre].index_s * blocks_per_s)) : 0.0f;
    g_bus_filter_a = 1.0f - expf(-6.2831853f * 2200.0f / g_rate);
    g_wet_filter_a = 1.0f - expf(-6.2831853f * 1400.0f / g_rate);
    memset(g_voices, 0, sizeof(g_voices));
    room_clear();
    g_echo_pos_a = g_echo_pos_b = 0;
    g_diffuse_pos_a = g_diffuse_pos_b = g_diffuse_pos_c = 0;
    g_quiet = 0;
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
    /* All from zero: a carrier and a modulator at the same pitch that
       start together stay together, and then FM has no DC in it. */
    for (int p = 0; p < PARTIALS; p++) {
        v->phase[p] = 0;
        v->inc[p] = (unsigned)(inc * t->ratio[p]);
    }
    v->peak = velocity;
    v->inv_peak = velocity > 0.0f ? 1.0f / velocity : 0.0f;
    if (t->attack_s > 0.0f) {
        v->env = 0.0f;
        v->attack = velocity / (t->attack_s * blocks_per_s);
    } else {
        v->env = velocity;
        v->attack = 0.0f;
    }
    v->hold = (int)(t->hold_s * blocks_per_s);
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
    /* The same for the brightness of the FM voices. */
    v->strike = t->index_strike;
    v->strike_decay = g_index_decay[timbre];
    v->key = note >= 0 && note < MIDI_NOTES ? g_index_key[note] : 1.0f;
    v->bright_phase = (unsigned)note * 0x5B000000u;
    v->bright_inc = (unsigned)(t->bright_hz * (1.0f + 0.09f * ((note * 5) % 7)) / blocks_per_s * 4294967296.0f);
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

/* Envelopes move once per block; inside a block a voice is two or three
   table lookups and a handful of multiply-adds per sample, which is what
   the CPU can afford. */
static void voice_block(struct voice *v) {
    if (v->attack > 0.0f) {
        v->env += v->attack;
        if (v->env >= v->peak) { v->env = v->peak; v->attack = 0.0f; }
    } else if (v->hold > 0) {
        v->hold--;
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
    if (v->t->fm) {
        /* Darker as it fades, as anything struck or bowed is: at the peak
           the index is the timbre's, at nothing it is four tenths of it. */
        const struct timbre *t = v->t;
        float index = t->index_rest * (0.4f + 0.6f * v->env * v->inv_peak);
        if (t->bright_depth > 0.0f) {
            v->bright_phase += v->bright_inc;
            index *= 1.0f + t->bright_depth * sine(v->bright_phase);
        }
        index += v->strike;
        v->strike *= v->strike_decay;
        index *= v->key;
        v->index = (index < INDEX_MAX ? index : INDEX_MAX) * RADIAN;
        /* The twin fades out of a note's tail, from a third of its peak to
           a tenth, where under the next chord it is no longer heard; a
           voice without it is a lookup cheaper, and the bed is mostly
           tails. */
        float twin = (v->env * v->inv_peak - 0.1f) * 4.0f;
        if (v->attack <= 0.0f && twin < 1.0f) v->gain[2] *= twin > 0.0f ? twin : 0.0f;
    }
}

void synth_render(short *out, int frames) {
    while (frames > 0) {
        int n = frames < BLOCK ? frames : BLOCK;
        float mix_l[BLOCK], mix_r[BLOCK];       /* the interface, straight to the room */
        float bus_l[BLOCK], bus_r[BLOCK];       /* the tune, through the filter first */
        float wet[BLOCK];                       /* the tune's send, mono */
        memset(mix_l, 0, sizeof(mix_l));
        memset(mix_r, 0, sizeof(mix_r));
        memset(bus_l, 0, sizeof(bus_l));
        memset(bus_r, 0, sizeof(bus_r));
        memset(wet, 0, sizeof(wet));

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
        int any_music = 0, any = 0;
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
            any = 1;
            float gl = v->gain_l * level, gr = v->gain_r * level;
            if (v->t->fm) {
                /* The carrier read at its phase pushed on by the modulator;
                   with a twin, that sine on top. */
                unsigned p0 = v->phase[0], p1 = v->phase[1], p2 = v->phase[2];
                unsigned i0 = v->inc[0], i1 = v->inc[1], i2 = v->inc[2];
                float g0 = v->gain[0], g2 = v->gain[2], q = v->index;
                float gs = v->send * level;
                if (g2 > 0.0f) {
                    for (int f = 0; f < n; f++) {
                        float s = sine(p0 + (unsigned)(int)(sine(p1) * q)) * g0 + sine(p2) * g2;
                        p0 += i0; p1 += i1; p2 += i2;
                        ml[f] += s * gl;
                        mr[f] += s * gr;
                        wet[f] += s * gs;
                    }
                } else {
                    for (int f = 0; f < n; f++) {
                        float s = sine(p0 + (unsigned)(int)(sine(p1) * q)) * g0;
                        p0 += i0; p1 += i1;
                        ml[f] += s * gl;
                        mr[f] += s * gr;
                        wet[f] += s * gs;
                    }
                    p2 += (unsigned)n * i2;
                }
                v->phase[0] = p0; v->phase[1] = p1; v->phase[2] = p2;
                v->phase[3] += (unsigned)n * v->inc[3];
                continue;
            }
            unsigned p0 = v->phase[0], p1 = v->phase[1];
            unsigned i0 = v->inc[0], i1 = v->inc[1];
            float g0 = v->gain[0], g1 = v->gain[1];
            /* Two partials retain the body of every timbre. The upper pair
               cost as much again across every sustained chord and contribute
               little after the bus filter and PSP speakers. */
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

        /* Nothing has sounded for long enough that the room is silent too:
           empty it once and leave the echoes standing. */
        int quiet_blocks = QUIET_S * g_rate / BLOCK;
        if (any) g_quiet = 0;
        else if (g_quiet <= quiet_blocks && ++g_quiet > quiet_blocks) room_clear();
        if (g_quiet > quiet_blocks) {
            memset(out, 0, (size_t)n * 2 * sizeof(*out));
            out += n * 2;
            frames -= n;
            continue;
        }

        /* Fixed gentle low-pass; no random "air" under the signal. */
        if (any_music || g_music_level > 0.0005f) {
            float a = g_bus_filter_a;
            float l1 = g_bus_lp[0], l2 = g_bus_lp[1], r1 = g_bus_lp[2], r2 = g_bus_lp[3];
            for (int f = 0; f < n; f++) {
                l1 += (bus_l[f] - l1) * a; l2 += (l1 - l2) * a;
                r1 += (bus_r[f] - r1) * a; r2 += (r1 - r2) * a;
                bus_l[f] = l2;
                bus_r[f] = r2;
            }
            g_bus_lp[0] = l1; g_bus_lp[1] = l2; g_bus_lp[2] = r1; g_bus_lp[3] = r2;
        }

        /* The tune's send, darkened and smeared through the allpasses on its
           way to the echoes. Two points along the chain are heard at once,
           one each side: the near part of the room, a little different left
           and right. Only while the tune sounds, so the filters never ring
           down on nothing into subnormal floats. */
        float room[BLOCK], near_l[BLOCK], near_r[BLOCK];
        if (any_music) {
            float lp = g_wet_lp, a = g_wet_filter_a;
            int pa = g_diffuse_pos_a, pb = g_diffuse_pos_b, pc = g_diffuse_pos_c;
            for (int f = 0; f < n; f++) {
                lp += (wet[f] - lp) * a;
                float d = g_diffuse_a[pa], w = lp + 0.6f * d, y = d - 0.6f * w;
                g_diffuse_a[pa] = w;
                if (++pa == DIFFUSE_A) pa = 0;
                d = g_diffuse_b[pb]; w = y + 0.6f * d; float yb = d - 0.6f * w;
                g_diffuse_b[pb] = w;
                if (++pb == DIFFUSE_B) pb = 0;
                d = g_diffuse_c[pc]; w = yb + 0.6f * d; float yc = d - 0.6f * w;
                g_diffuse_c[pc] = w;
                if (++pc == DIFFUSE_C) pc = 0;
                room[f] = yc;
                near_l[f] = yb * 0.30f;
                near_r[f] = yc * 0.30f;
            }
            g_wet_lp = lp;
            g_diffuse_pos_a = pa; g_diffuse_pos_b = pb; g_diffuse_pos_c = pc;
        } else {
            memset(room, 0, sizeof(room));
            memset(near_l, 0, sizeof(near_l));
            memset(near_r, 0, sizeof(near_r));
        }

        /* Two echoes fed with a mono sum, darkened a little each pass, and
           laid back in: a room with some depth to it. Neither delay wraps
           often, so the run up to the nearer wrap is taken in one go and
           the two tests come out of the inner loop. The tune comes in after
           the room, which it has already been sent to. */
        for (int f = 0; f < n; ) {
            int m = n - f;
            if (m > ECHO_A - g_echo_pos_a) m = ECHO_A - g_echo_pos_a;
            if (m > ECHO_B - g_echo_pos_b) m = ECHO_B - g_echo_pos_b;
            float *da = &g_echo_a[g_echo_pos_a], *db = &g_echo_b[g_echo_pos_b];
            float lp = g_echo_lp;
            for (int k = 0; k < m; k++, f++) {
                float l = mix_l[f] * 0.60f, r = mix_r[f] * 0.60f;
                float mono = (l + r) * 0.5f + room[f];
                float ea = da[k], eb = db[k];
                lp += ((ea + eb) * 0.5f - lp) * 0.30f;
                da[k] = mono + lp * 0.50f;
                db[k] = mono + lp * 0.46f;
                l += ea * 0.30f + eb * 0.20f + bus_l[f] + near_l[f];
                r += eb * 0.30f + ea * 0.20f + bus_r[f] + near_r[f];
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
