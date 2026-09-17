#include <pspkernel.h>
#include <psppower.h>
#include <psputility.h>
#include <stdio.h>
#include <string.h>

#include "gui/gfx.h"
#include "gui/osk.h"
#include "util/runtime.h"

#define OSK_TEXT 256

static void (*g_draw)(void *ctx);
static void *g_draw_ctx;

/* The firmware reads these for as long as the dialog stands, so they
   cannot live on the stack of a call that returns before it is done. */
static SceUtilityOskParams g_params;
static SceUtilityOskData g_data;
static unsigned short g_desc[64], g_in[OSK_TEXT], g_out[OSK_TEXT];
static volatile int g_open;
static int g_seen;                  /* the dialog has been up at least once */

void osk_frame(void (*draw)(void *ctx), void *ctx) {
    g_draw = draw;
    g_draw_ctx = ctx;
}

static void to_ucs2(unsigned short *dst, size_t count, const char *src) {
    size_t n = 0;
    for (; src && src[n] && n + 1 < count; n++) dst[n] = (unsigned char)src[n];
    dst[n] = 0;
}

static void from_ucs2(char *dst, size_t size, const unsigned short *src) {
    size_t n = 0;
    for (size_t i = 0; src[i] && n + 1 < size; i++)
        if (src[i] >= 32 && src[i] < 127) dst[n++] = (char)src[i];
    dst[n] = '\0';
}

/* Between the frame's last command and its swap: the dialog draws over
   what the shell drew, and the swap shows both. */
static void overlay(void) {
    switch (sceUtilityOskGetStatus()) {
    case PSP_UTILITY_DIALOG_VISIBLE:
        g_seen = 1;
        sceUtilityOskUpdate(1);
        break;
    case PSP_UTILITY_DIALOG_QUIT:
        g_seen = 1;
        sceUtilityOskShutdownStart();
        break;
    case PSP_UTILITY_DIALOG_FINISHED:
        g_seen = 1;
        break;
    case PSP_UTILITY_DIALOG_NONE:
        /* Also what the status says for a frame or two before the dialog
           has come up, which is not the dialog being gone. */
        if (g_seen) g_open = 0;
        break;
    default:
        break;
    }
}

int osk_read(const char *title, const char *initial, char *out, size_t size) {
    if (!g_draw) return -1;
    memset(&g_params, 0, sizeof(g_params));
    memset(&g_data, 0, sizeof(g_data));
    to_ucs2(g_desc, sizeof(g_desc) / sizeof(g_desc[0]), title);
    to_ucs2(g_in, OSK_TEXT, initial);
    memset(g_out, 0, sizeof(g_out));

    g_data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    g_data.lines = 1;
    g_data.unk_24 = 1;
    /* What a URL is made of; SELECT on the keyboard switches among them. */
    g_data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT |
                       PSP_UTILITY_OSK_INPUTTYPE_LATIN_SYMBOL |
                       PSP_UTILITY_OSK_INPUTTYPE_LATIN_LOWERCASE |
                       PSP_UTILITY_OSK_INPUTTYPE_LATIN_UPPERCASE |
                       PSP_UTILITY_OSK_INPUTTYPE_URL;
    g_data.desc = g_desc;
    g_data.intext = g_in;
    g_data.outtextlength = OSK_TEXT;
    g_data.outtextlimit = OSK_TEXT - 1;
    g_data.outtext = g_out;

    g_params.base.size = sizeof(g_params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &g_params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &g_params.base.buttonSwap);
    /* The dialog's own threads, placed the way the SDK's samples place
       them: above the main thread, so the keyboard answers the pad even
       while a frame is being drawn. */
    g_params.base.graphicsThread = 0x11;
    g_params.base.accessThread = 0x13;
    g_params.base.fontThread = 0x12;
    g_params.base.soundThread = 0x10;
    g_params.datacount = 1;
    g_params.data = &g_data;

    int rc = sceUtilityOskInitStart(&g_params);
    if (rc < 0) {
        logline("osk: refused %08x", (unsigned)rc);
        return -1;
    }
    g_open = 1;
    g_seen = 0;
    int cpu_before = scePowerGetCpuClockFrequencyInt();
    gfx_frame_overlay(overlay);
    while (g_open) g_draw(g_draw_ctx);
    gfx_frame_overlay(NULL);
    /* The dialog is the firmware's, and the firmware runs at its own
       clock: the console can come back from it at 222 MHz, and everything
       after the keyboard then crawls. Put the clock back where main set
       it, and say so once when it had moved. */
    int cpu_after = scePowerGetCpuClockFrequencyInt();
    if (cpu_after != cpu_before || cpu_after < 333) {
        logline("osk: cpu %d -> %d MHz, set back to 333", cpu_before, cpu_after);
        scePowerSetClockFrequency(333, 333, 166);
    }

    out[0] = '\0';
    if (g_data.result == PSP_UTILITY_OSK_RESULT_CANCELLED) {
        logline("osk: cancelled");
        return 0;
    }
    from_ucs2(out, size, g_out);
    logline("osk: \"%s\"", out);
    return 1;
}
