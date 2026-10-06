#include <kubridge.h>
#include <pspkernel.h>
#include <string.h>

#include "network/usbnet.h"
#include "util/runtime.h"

#define MODULE "usbnet"

/* A module every firmware has, asked for by name: the bridge answers with
   that module or it is no bridge. An emulator's stand-in for the call may
   say yes to anything, so the name that comes back is read too. */
int usbnet_kernel(void) {
    static char name[] = "sceSystemMemoryManager";
    SceModule found;
    memset(&found, 0, sizeof(found));
    return kuKernelFindModuleByName(name, &found) == 0 && !strcmp(found.modname, name);
}

int usbnet_loaded(void) {
    static char name[] = MODULE;
    SceModule found;
    memset(&found, 0, sizeof(found));
    return usbnet_kernel() && kuKernelFindModuleByName(name, &found) == 0 &&
           !strcmp(found.modname, name);
}

int usbnet_load(const char *path) {
    int status = 0;
    if (!usbnet_kernel())
        return -1;
    /* Loaded by the firmware as a plugin, that copy is the one at work: a
       second would only find the first and leave again. */
    if (usbnet_loaded())
        return 0;
    SceUID mod = kuKernelLoadModule(path, 0, NULL);
    if (mod < 0) {
        logline("usbnet: %s not loaded %08x", path, (unsigned)mod);
        return -1;
    }
    int rc = sceKernelStartModule(mod, strlen(path) + 1, (void *)path, &status, NULL);
    logline("usbnet: %s started %08x", path, (unsigned)rc);
    return usbnet_loaded() ? 0 : -1;
}
