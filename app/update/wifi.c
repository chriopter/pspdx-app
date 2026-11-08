/* Auto Connect's half of the radio: the system's dialog asks which
   connection to use and joins it; here the connection is the one chosen
   once under Options, and it is joined on the sync thread while the
   launcher goes on being used. */

#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <psputility.h>
#include <stdio.h>
#include <string.h>

#include "pspkit-https/https.h"
#include "update/wifi.h"
#include "util/runtime.h"

#define JOIN_US (20 * 1000000u)

int wifi_profile_name(int profile, char *out, size_t size) {
    netData data;
    if (profile < 1 || profile > WIFI_PROFILES || sceUtilityCheckNetParam(profile) != 0) return -1;
    memset(&data, 0, sizeof(data));
    if (sceUtilityGetNetParam(profile, PSP_NETPARAM_NAME, &data) != 0 || !data.asString[0])
        snprintf(out, size, "%d", profile);
    else
        snprintf(out, size, "%s", data.asString);
    return 0;
}

int wifi_profile_next(int profile) {
    for (int p = profile + 1; p <= WIFI_PROFILES; p++)
        if (sceUtilityCheckNetParam(p) == 0) return p;
    return 0;
}

int wifi_join(int profile) {
    int state = 0;
    if (https_net_init() < 0) return -1;
    if (sceNetApctlGetState(&state) >= 0 && state == 4) return 0;
    if (sceNetApctlConnect(profile) < 0) {
        logline("wifi: connection %d would not start", profile);
        return -1;
    }
    /* The state climbs to 4, an address; back at 0 after it had moved,
       the access point was not there or said no. */
    unsigned start = sceKernelGetSystemTimeLow();
    int moved = 0;
    for (;;) {
        if (sceNetApctlGetState(&state) < 0) break;
        if (state == 4) {
            logline("wifi: joined connection %d in %u ms", profile,
                    (sceKernelGetSystemTimeLow() - start) / 1000);
            return 0;
        }
        if (state > 0) moved = 1;
        else if (moved) break;
        if (sceKernelGetSystemTimeLow() - start > JOIN_US) break;
        sceKernelDelayThread(50 * 1000);
    }
    logline("wifi: connection %d not joined, state %d", profile, state);
    sceNetApctlDisconnect();
    return -1;
}
