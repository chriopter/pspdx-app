#include "util/storage.h"
/*
 * The entropy sweep. The field the browser stands on starts dry; the stick
 * carries a source of water over it, and where the source goes water is
 * born and spreads. When enough unguessed turns have held their heading and
 * crossed new ground, the pool is full and the key can be made; the surface
 * that was just built stays as the backdrop.
 */

#include <math.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "gui/entropy_screen.h"
#include "gui/font.h"
#include "gui/gfx.h"
#include "gui/lattice.h"
#include "gui/palette.h"
#include "gui/title.h"
#include "pspkit-https/entropy.h"
#include "pspkit-https/sweep.h"
#include "util/runtime.h"

#define TRACE_MAX 5000
#define TRACE_FILE  storage_path("PSP/PSPDX/DEBUG/PSPDX.TRACE")
#define REPLAY_FILE storage_path("PSP/PSPDX/DEBUG/PSPDX.REPLAY")
#define REC_DIR storage_path("PSP/PSPDX/DEBUG/PSPDX_REC")
#define REC_EVERY 4

struct trace_sample { unsigned char lx, ly; unsigned short buttons; };

static struct trace_sample trace[TRACE_MAX];
static int trace_len;
static int trace_pos;
static int replaying;
static int recording;

static struct sweep g_sweep = SWEEP_START;

static void trace_load(void) {
    int fd = sceIoOpen(REPLAY_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    sceIoClose(fd);
    fd = sceIoOpen(TRACE_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    int n = sceIoRead(fd, trace, sizeof(trace));
    sceIoClose(fd);
    if (n <= 0) return;
    trace_len = n / (int)sizeof(struct trace_sample);
    trace_pos = 0;
    replaying = 1;
}

static void trace_save(void) {
    if (replaying || trace_len == 0) return;
    int fd = sceIoOpen(TRACE_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, trace, (SceSize)(trace_len * (int)sizeof(struct trace_sample)));
    sceIoClose(fd);
}

static int next_sample(SceCtrlData *pad) {
    if (replaying) {
        if (trace_pos >= trace_len) return 0;
        struct trace_sample *sample = &trace[trace_pos++];
        memset(pad, 0, sizeof(*pad));
        pad->Lx = sample->lx;
        pad->Ly = sample->ly;
        pad->Buttons = sample->buttons;
        pad->TimeStamp = (unsigned)trace_pos;
        return 1;
    }
    sceCtrlPeekBufferPositive(pad, 1);
    if (trace_len < TRACE_MAX) {
        trace[trace_len].lx = pad->Lx;
        trace[trace_len].ly = pad->Ly;
        trace[trace_len].buttons = (unsigned short)pad->Buttons;
        trace_len++;
    }
    return 1;
}

void entropy_screen_reset_cache(void) {
    lattice_dry();
    g_sweep = (struct sweep)SWEEP_START;
}

void entropy_screen_prepare(void) {
    trace_load();
    int fd = sceIoOpen(storage_path("PSP/PSPDX/DEBUG/PSPDX.RECORD"), PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    sceIoClose(fd);
    recording = 1;
    sceIoMkdir(REC_DIR, 0777);
}

int entropy_screen_is_replay(void) { return replaying; }

static void record_frame(int frame) {
    if (!recording || frame % REC_EVERY) return;
    char path[64];
    snprintf(path, sizeof(path), "%s/F%05d.BMP", REC_DIR, frame / REC_EVERY);
    gfx_screenshot(path);
}

/* What the user has to know, over the water: what this is for, how far it
   has got, and when it can stop. */
static void draw_chrome(float t, int percent, int ready) {
    unsigned text = rgb_pack(rgb_mix(RGB_WHITE, DEFAULT_TINT, 0.05f), 255);
    unsigned accent = rgb_pack(rgb_mix(DEFAULT_TINT, RGB_WHITE, 0.45f), 255);
    unsigned dim = rgb_pack(rgb_mix(DEFAULT_TINT, RGB_WHITE, 0.35f), 200);

    /* The name as the browser sets its words: baked once, on a card in
       perspective, with the light going through it. */
    title_draw(SCR_W / 2.0f, 58.0f, t, DEFAULT_TINT);

    /* Under the name, centred, what the hand is for. */
    /* The same line on a replay: that is a development aid, and the seed
       it would leave is refused anyway. */
    const char *head = "Collect water with the stick or smash buttons";
    float hw = font_width(FONT_BODY, head);
    font_print(FONT_BODY, (SCR_W - hw) / 2, 104, text, head);

    /* The bar is the shore drawn straight: how much of the field is water. */
    int bar_x = 16, bar_y = 238, bar_w = SCR_W - 32;
    gfx_rect(bar_x, bar_y, bar_w, 5, RGBA(255, 255, 255, 36));
    int filled = bar_w * percent / 100;
    if (filled > 0) gfx_hgrad(bar_x, bar_y, filled, 5, rgb_pack(DEFAULT_TINT, 255), accent);

    char right[48];
    if (ready) snprintf(right, sizeof(right), "%d bits   START to continue", entropy_get_bits());
    else if (entropy_get_stashed())
        /* The way out of a sweep the browser asked for, said where the way
           on is said: an escape nobody is told about is not one. */
        snprintf(right, sizeof(right), "%d%%   %d bits   SELECT to leave",
                 percent, entropy_get_bits());
    else snprintf(right, sizeof(right), "%d%%   %d bits", percent, entropy_get_bits());
    float w = font_width(FONT_META, right);
    font_print(FONT_META, SCR_W - 16 - w, 232, ready ? accent : dim, right);
}

int entropy_screen_run(void) {
    entropy_screen_reset_cache();
    /* Every sweep starts its trace afresh: a replay from the first sample,
       a recording from an empty buffer, so a second sweep in the same run
       neither plays the rest of the last one nor appends to it. */
    trace_pos = 0;
    if (!replaying) trace_len = 0;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    int frame = 0, left = 0;
    unsigned worst_us = 0, frame_us = now_us();
    for (;;) {
        SceCtrlData pad;
        if (!next_sample(&pad)) break;
        {
            unsigned now = now_us(), took = now - frame_us;
            frame_us = now;
            if (frame && took > worst_us) worst_us = took;
        }
        /* The stick and every button but START and SELECT feed the sweep. A
           press rains a few stars, and one that paid rains more: the hand
           sees which of its presses counted. */
        int happened = sweep_step(&g_sweep, pad.Lx, pad.Ly, pad.Buttons);
        if (happened & SWEEP_PRESSED) lattice_shower(happened & SWEEP_PRESS_PAID ? 6 : 2);

        /* The water is the picture of the sweep, not its measure: the bar
           tracks the bits alone, and past the mark it stays full. */
        lattice_pour(g_sweep.x, g_sweep.z, g_sweep.moving);
        int percent = sweep_get_percent();
        int ready = percent == 100;

        float t = gfx_frames() * (1.0f / 60.0f);
        title_prepare("PSPDX", DEFAULT_TINT);
        gfx_frame_begin(0xFF000000);
        gfx_vgrad(0, 0, SCR_W, SCR_H,
                  rgb_pack(rgb_mix(NIGHT_TOP, DEFAULT_TINT, 0.05f), 255),
                  rgb_pack(rgb_mix(NIGHT_BOTTOM, DEFAULT_TINT, 0.18f), 255));
        lattice_draw(t, DEFAULT_TINT);
        draw_chrome(t, percent, ready);
        gfx_frame_end();

        frame++;
        record_frame(frame);
        if (ready && (pad.Buttons & PSP_CTRL_START)) break;
        /* O leaves a sweep that has a pool behind it. Without this the only
           way out of this loop is a full sweep: the bar cannot fill without
           a hand on the stick, and a screen reached by two presses from the
           browser would hold the console until the battery did. The first
           sweep of a run has nothing to go back to and is not offered it. */
        if (!ready && entropy_get_stashed() && (pad.Buttons & PSP_CTRL_SELECT)) {
            left = 1;
            break;
        }
    }

    lattice_settle();
    trace_save();
    logline("sweep: %d frames, worst %u ms%s", frame, worst_us / 1000,
            left ? ", left with O" : "");
    return left ? 0 : entropy_get_bits();
}
