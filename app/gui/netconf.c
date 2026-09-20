#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <psppower.h>
#include <psputility.h>
#include <string.h>

#include "gui/gfx.h"
#include "gui/netconf.h"
#include "pspkit-https/https.h"
#include "util/runtime.h"

static pspUtilityNetconfData params;
static int open, seen;

static void overlay(void) {
    int status = sceUtilityNetconfGetStatus();
    if (status != PSP_UTILITY_DIALOG_NONE) seen = 1;
    switch (status) {
    case PSP_UTILITY_DIALOG_VISIBLE:
        sceUtilityNetconfUpdate(1);
        break;
    case PSP_UTILITY_DIALOG_QUIT:
        sceUtilityNetconfShutdownStart();
        break;
    case PSP_UTILITY_DIALOG_NONE:
        if (seen) open = 0;
        break;
    default:
        if (status < 0) open = 0;
        break;
    }
}

int netconf_connect(void) {
    if (https_net_init() < 0) {
        logline("wifi: network stack unavailable");
        return -1;
    }
    memset(&params, 0, sizeof(params));
    params.base.size = sizeof(params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &params.base.buttonSwap);
    params.base.graphicsThread = 0x11;
    params.base.accessThread = 0x13;
    params.base.fontThread = 0x12;
    params.base.soundThread = 0x10;
    params.action = PSP_NETCONF_ACTION_CONNECTAP;
    int rc = sceUtilityNetconfInitStart(&params);
    if (rc < 0) {
        logline("wifi: dialog refused %08x", (unsigned)rc);
        return -1;
    }
    logline("wifi: system connection dialog opened");
    open = 1;
    seen = 0;
    gfx_frame_overlay(overlay);
    while (open) {
        gfx_frame_begin(RGB(12, 18, 30));
        gfx_vgrad(0, 0, SCR_W, SCR_H, RGB(12, 18, 30), RGB(30, 44, 68));
        gfx_frame_end();
    }
    gfx_frame_overlay(NULL);
    scePowerSetClockFrequency(333, 333, 166);
    int state = 0;
    int connected = params.base.result == 0 &&
        sceNetApctlGetState(&state) >= 0 && state == 4;
    logline("wifi: dialog result %08x, state %d", (unsigned)params.base.result, state);
    return connected ? 0 : -1;
}
