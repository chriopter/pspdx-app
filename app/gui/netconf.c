#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <pspiofilemgr.h>
#include <kubridge.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <pspwlan.h>
#include <psppower.h>
#include <psputility.h>
#include <string.h>

#include "gui/gfx.h"
#include "gui/netconf.h"
#include "pspkit-https/https.h"
#include "util/runtime.h"

/* Read-only fingerprints distinguish a mixed/modified firmware from a
   dialog state-machine problem without ever copying firmware into the log. */
static void firmware_fingerprint(void) {
    static int done;
    static const char *paths[] = {
        "flash0:/kd/utility.prx", "flash0:/kd/pspnet_apctl.prx",
        "flash0:/vsh/module/netconf_plugin.prx", "flash0:/vsh/module/dialogmain.prx",
        "flash0:/vsh/module/common_gui.prx", "flash0:/vsh/module/common_util.prx",
        "flash0:/vsh/module/pafmini.prx", "flash0:/vsh/resource/netconf_dialog.rco"
    };
    if (done) return;
    done = 1;
    logline("wifi: model %d, devkit %08x, CPU %d, bus %d",
            kuKernelGetModel(), (unsigned)sceKernelDevkitVersion(),
            scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int fd = sceIoOpen(paths[i], PSP_O_RDONLY, 0);
        if (fd < 0) {
            logline("wifi: firmware %s open %08x", paths[i], (unsigned)fd);
            continue;
        }
        wc_Sha256 hash;
        unsigned char buf[2048], digest[32];
        char hex[65];
        unsigned size = 0;
        int rc = wc_InitSha256(&hash), n = 0;
        int initialized = rc == 0;
        while (rc == 0 && (n = sceIoRead(fd, buf, sizeof(buf))) > 0) {
            size += (unsigned)n;
            rc = wc_Sha256Update(&hash, buf, n);
        }
        sceIoClose(fd);
        if (rc == 0 && n >= 0) rc = wc_Sha256Final(&hash, digest);
        else if (rc == 0) rc = n;
        if (initialized) wc_Sha256Free(&hash);
        if (rc != 0) {
            logline("wifi: firmware %s hash %08x", paths[i], (unsigned)rc);
            continue;
        }
        for (int j = 0; j < 32; j++) {
            static const char digits[] = "0123456789abcdef";
            hex[j * 2] = digits[digest[j] >> 4];
            hex[j * 2 + 1] = digits[digest[j] & 15];
        }
        hex[64] = 0;
        logline("wifi: firmware %s %u bytes SHA256 %s", paths[i], size, hex);
    }
}

static pspUtilityNetconfData params;
static int dialog_open, seen, last_status, last_update;
static int track_modules, saw_netconf, failed_to_open;
static unsigned started, visible_frames;

int netconf_failed_to_open(void) { return failed_to_open; }

static int module_present(char *name) {
    SceModule mod;
    memset(&mod, 0, sizeof(mod));
    return kuKernelFindModuleByName(name, &mod) == 0 && !strcmp(mod.modname, name);
}

static void dialog_modules(void) {
    static char names[][32] = {
        "scePaf_Module", "sceVshCommonGui_Module", "sceVshCommonUtil_Module",
        "sceDialogmain_Module", "sceVshNetconf_Module"
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        SceModule mod;
        memset(&mod, 0, sizeof(mod));
        int rc = kuKernelFindModuleByName(names[i], &mod);
        if (rc == 0 && !strcmp(mod.modname, names[i]))
            logline("wifi: module %s version %u.%u, text %u bytes", names[i],
                    mod.version[1], mod.version[0], (unsigned)mod.text_size);
        else
            logline("wifi: module %s unavailable %08x", names[i], (unsigned)rc);
    }
}

static void overlay(void) {
    int status = sceUtilityNetconfGetStatus();
    if (status != last_status) {
        logline("wifi: dialog status %d at %u ms, %u visible frames",
                status, now_ms() - started, visible_frames);
        if (status == PSP_UTILITY_DIALOG_VISIBLE) dialog_modules();
        log_dump_later();
        last_status = status;
    }
    if (status != PSP_UTILITY_DIALOG_NONE) seen = 1;
    switch (status) {
    case PSP_UTILITY_DIALOG_VISIBLE: {
        /* VISIBLE describes the outer utility, not successful loading of
           netconf_plugin. Its asynchronous load can finish later. */
        static char name[] = "sceVshNetconf_Module";
        if (track_modules && !saw_netconf && module_present(name)) {
            saw_netconf = 1;
            logline("wifi: network dialog module loaded at %u ms", now_ms() - started);
            log_dump_later();
        }
        int rc = sceUtilityNetconfUpdate(1);
        visible_frames++;
        if (rc != last_update) {
            logline("wifi: dialog update %08x", (unsigned)rc);
            log_dump_later();
            last_update = rc;
        }
        break;
    }
    case PSP_UTILITY_DIALOG_QUIT:
        sceUtilityNetconfShutdownStart();
        break;
    case PSP_UTILITY_DIALOG_NONE:
        if (seen) dialog_open = 0;
        break;
    default:
        if (status < 0) dialog_open = 0;
        break;
    }
}

int netconf_connect(void) {
    failed_to_open = 0;
    saw_netconf = 0;
    static char utility[] = "sceUtility_Driver";
    track_modules = module_present(utility);
    firmware_fingerprint();
    logline("wifi: initializing network stack");
    log_dump();
    if (https_net_init() < 0) {
        logline("wifi: network stack unavailable");
        log_dump();
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
    int profiles = 0;
    for (int id = 1; id <= 10; id++)
        if (sceUtilityCheckNetParam(id) == 0) profiles++;
    logline("wifi: profiles %d, language %d, button swap %d, WLAN switch %d",
            profiles, params.base.language, params.base.buttonSwap, sceWlanGetSwitchState());
    logline("wifi: starting system connection dialog");
    log_dump();
    int rc = sceUtilityNetconfInitStart(&params);
    if (rc < 0) {
        failed_to_open = 1;
        logline("wifi: dialog refused %08x", (unsigned)rc);
        log_dump();
        return -1;
    }
    logline("wifi: system connection dialog opened");
    log_dump();
    last_status = last_update = -999;
    started = now_ms();
    visible_frames = 0;
    dialog_open = 1;
    seen = 0;
    gfx_frame_overlay(overlay);
    while (dialog_open) {
        gfx_frame_begin(RGB(12, 18, 30));
        gfx_vgrad(0, 0, SCR_W, SCR_H, RGB(12, 18, 30), RGB(30, 44, 68));
        gfx_frame_end();
    }
    gfx_frame_overlay(NULL);
    scePowerSetClockFrequency(333, 333, 166);
    int state = 0;
    int state_rc = sceNetApctlGetState(&state);
    int connected = params.base.result == 0 && state_rc >= 0 && state == 4;
    logline("wifi: dialog result %08x, apctl query %08x, state %d",
            (unsigned)params.base.result, (unsigned)state_rc, state);
    logline("wifi: network dialog module %s", !track_modules ? "not observable"
                                                            : saw_netconf ? "loaded" : "never loaded");
    /* Sony leaves result at its initial zero when its plugin fails to load.
       This is neither a successful connection nor an ordinary cancellation. */
    if (!connected && params.base.result == 0 && track_modules && !saw_netconf) {
        failed_to_open = 1;
        logline("wifi: system network dialog failed to load");
    }
    log_dump();
    return connected ? 0 : -1;
}
