#include "audio/cues.h"
#include "audio/synth.h"
#include "util/runtime.h"

#define RING 16

static volatile struct { unsigned char cue; signed char index; } g_ring[RING];
static volatile unsigned g_head, g_tail;

void cues_post(enum cue cue, int index) {
    unsigned next = (g_head + 1) % RING;
    if (next == g_tail) return;         /* full: drop, a sound is not data */
    g_ring[g_head].cue = (unsigned char)cue;
    g_ring[g_head].index = (signed char)(index > 60 ? 60 : index < 0 ? 0 : index);
    g_head = next;
}

/* Rows step down a pentatonic scale from E5, so scrolling down a list
   plays it down. */
static const unsigned char SCALE[5] = { 0, 2, 4, 7, 9 };

/* Every key is a bell: the same glass the tune rings now and then, struck
   damped: half the ring and a third of the hall, quieter than the tune's
   own, so a run of presses is a run and not a wash. One sound for the whole
   interface, so nothing pressed sounds like a different instrument from
   the room it is pressed in. */
static void bell(int note, float velocity, float pan) {
    synth_strike(note, velocity, SYNTH_CHIME, pan, 0);
}

static void play(enum cue cue, int index) {
    switch (cue) {
    case CUE_MOVE: {
        /* A held direction runs through rows many times a second; then
           each row is a quieter, single strike, so a run is a run of bells
           and not a pile. Sides alternate with the row. */
        static unsigned last_ms;
        unsigned now = now_ms();
        int run = now - last_ms < 90;
        last_ms = now;
        int note = 76 - SCALE[index % 5] - 12 * (index / 5);
        if (note < 64) note = 64;
        bell(note, run ? 0.02f : 0.045f, index & 1 ? 0.35f : -0.35f);
        break;
    }
    case CUE_OPEN:
        bell(71, 0.06f, -0.4f);
        bell(78, 0.045f, 0.4f);
        break;
    case CUE_DONE:
        bell(71, 0.055f, -0.5f);
        bell(75, 0.055f, -0.2f);
        bell(78, 0.06f, 0.2f);
        bell(83, 0.045f, 0.5f);
        break;
    case CUE_FAIL:
        bell(59, 0.07f, -0.3f);
        bell(60, 0.055f, 0.3f);
        break;
    }
}

void cues_drain(void) {
    while (g_tail != g_head) {
        play((enum cue)g_ring[g_tail].cue, g_ring[g_tail].index);
        g_tail = (g_tail + 1) % RING;
    }
}
