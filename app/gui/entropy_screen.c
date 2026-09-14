#include "util/storage.h"
/*
 * The entropy sweep. The field the browser stands on starts dry; the stick
 * carries a source of water over it, and where the source goes water is
 * born and spreads. When the field is water and the pool is full the key
 * can be made, and the surface that was just built stays as the backdrop.
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
#include "logic/entropy.h"
#include "util/runtime.h"

/* The stick moves the source across the field at the speed it moved the old
   text cursor across its 60 by 28 cells, so a sweep takes as long as it
   ever did. The pool is still fed the position it landed on. */
#define FIELD_W 60.0f
#define FIELD_H 28.0f
#define STEP_X (0.0065f / FIELD_W)
#define STEP_Z (0.0040f / FIELD_H)


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

static float g_fx = 0.5f, g_fz = 0.5f;

/* The stick's direction as one of eight, 45 degrees each, centred on the
   axes and the diagonals; 5/12 stands in for tan 22.5. */
static unsigned heading_of(int dx, int dy) {
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ay * 12 < ax * 5) return dx > 0 ? 0 : 4;
    if (ax * 12 < ay * 5) return dy > 0 ? 2 : 6;
    if (dx > 0) return dy > 0 ? 1 : 7;
    return dy > 0 ? 3 : 5;
}

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
    g_fx = g_fz = 0.5f;
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
    const char *head = "Move the analog stick to collect entropy for TLS 1.3";
    float hw = font_width(FONT_BODY, head);
    font_print(FONT_BODY, (SCR_W - hw) / 2, 104, text, head);

    /* The bar is the shore drawn straight: how much of the field is water. */
    int bar_x = 16, bar_y = 238, bar_w = SCR_W - 32;
    gfx_rect(bar_x, bar_y, bar_w, 5, RGBA(255, 255, 255, 36));
    int filled = bar_w * percent / 100;
    if (filled > 0) gfx_hgrad(bar_x, bar_y, filled, 5, rgb_pack(DEFAULT_TINT, 255), accent);

    char right[48];
    if (ready) snprintf(right, sizeof(right), "%d bits   X to continue", entropy_bits());
    else if (entropy_stashed())
        /* The way out of a sweep the browser asked for, said where the way
           on is said: an escape nobody is told about is not one. */
        snprintf(right, sizeof(right), "%d%%   %d bits   O to leave",
                 percent, entropy_bits());
    else snprintf(right, sizeof(right), "%d%%   %d bits", percent, entropy_bits());
    float w = font_width(FONT_META, right);
    font_print(FONT_META, SCR_W - 16 - w, 232, ready ? accent : dim, right);
}

int entropy_screen_run(void) {
    entropy_screen_reset_cache();
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
        int dx = (int)pad.Lx - 128;
        int dy = (int)pad.Ly - 128;
        int moving = dx * dx + dy * dy > 14 * 14;

        if (moving) {
            g_fx += dx * STEP_X;
            /* Stick down runs the source at the viewer, not at the horizon. */
            g_fz -= dy * STEP_Z;
            if (g_fx < 0) g_fx = 0;
            if (g_fx > 1) g_fx = 1;
            if (g_fz < 0) g_fz = 0;
            if (g_fz > 1) g_fz = 1;
            /* The source pays for a turn onto new ground: a field of the
               invisible 250x250 grid, entered for the first time, under a
               heading other than the last one paid for. A stick against its
               stop earns nothing, and neither does a long straight stroke. */
            int fx = (int)(g_fx * (ENTROPY_FIELD_SIDE - 1) + 0.5f);
            int fz = (int)(g_fz * (ENTROPY_FIELD_SIDE - 1) + 0.5f);
            entropy_absorb_field((unsigned)fz * ENTROPY_FIELD_SIDE + (unsigned)fx,
                                 heading_of(dx, dy));
        }

        /* The water is the picture of the sweep, not its measure: the bar
           tracks the bits alone, and past the mark it stays full. */
        lattice_pour(g_fx, g_fz, moving);
        int bits = entropy_bits();
        int ready = bits >= ENTROPY_BITS;
        int percent = ready ? 100 : bits * 100 / ENTROPY_BITS;

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
        if (ready && (pad.Buttons & PSP_CTRL_CROSS)) break;
        /* O leaves a sweep that has a pool behind it. Without this the only
           way out of this loop is a full sweep: the bar cannot fill without
           a hand on the stick, and a screen reached by two presses from the
           browser would hold the console until the battery did. The first
           sweep of a run has nothing to go back to and is not offered it. */
        if (!ready && entropy_stashed() && (pad.Buttons & PSP_CTRL_CIRCLE)) {
            left = 1;
            break;
        }
    }

    lattice_settle();
    trace_save();
    logline("sweep: %d frames, worst %u ms%s", frame, worst_us / 1000,
            left ? ", left with O" : "");
    return left ? 0 : entropy_bits();
}
