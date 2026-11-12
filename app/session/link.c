#include "text.h"
/*
 * The Wi-Fi connection watched once it is there. The access point's state
 * is asked every couple of seconds -- a word from the driver, no packet
 * sent -- and at once when the console wakes from standby, which takes
 * the radio down under a running program without telling it. Gone, the
 * fetches are told so (sync_set_online), the store's first entry comes
 * back, and with Auto Connect on the connection is joined again on a
 * thread of its own while the browser goes on.
 */

#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <pspwlan.h>
#include <stdio.h>
#include <string.h>

#include "gui/netconf.h"
#include "gui/preview.h"
#include "gui/shell.h"
#include "session/cable.h"
#include "session/link.h"
#include "session/options.h"
#include "update/sync.h"
#include "update/wifi.h"
#include "util/runtime.h"

#define LINK_POLL_MS 2000
#define JOIN_PRIORITY 0x22
#define JOIN_STACK (32 * 1024)

static volatile int g_resumed;
static volatile enum link_state g_state = LINK_NONE;
static int g_was_up;
static unsigned g_polled;

/* The join on its thread: the connection, what came of it, and whether
   it has come to anything yet. */
static SceUID g_thread = -1;
static int g_join_profile;
static volatile int g_join_rc, g_join_done;
static char g_join_name[64];

static int g_offer;
static char g_offer_name[64];

void link_note_resume(void) { g_resumed = 1; }
enum link_state link_state(void) { return g_state; }
int link_was_up(void) { return g_was_up; }
int link_waiting(void) { return g_state == LINK_LOST || g_state == LINK_JOINING; }

/* Wi-Fi, not the cable: only an access point is there to lose. */
static int on_wifi(void) {
    int cable = options_cable();
    return cable != CABLE_USB && cable != CABLE_USB_KNOWN;
}

static int apctl_up(void) {
    int state = 0;
    return sceNetApctlGetState(&state) >= 0 && state == 4;
}

static int join_run(SceSize args, void *argp) {
    (void)args; (void)argp;
    /* With the switch off there is no radio to wait twenty seconds for. */
    g_join_rc = sceWlanGetSwitchState() ? wifi_join(g_join_profile) : -2;
    g_join_done = 1;
    return 0;
}

/* Auto Connect's connection, joined in the background. 0 when there is
   none to join. */
static int join_start(void) {
    int profile = options_auto_connect();
    if (!profile || wifi_profile_name(profile, g_join_name, sizeof(g_join_name)) < 0) return 0;
    g_join_profile = profile;
    g_join_done = 0;
    g_join_rc = -1;
    g_thread = sceKernelCreateThread("rejoin", join_run, JOIN_PRIORITY, JOIN_STACK, PSP_THREAD_ATTR_USER, 0);
    if (g_thread < 0) {
        logline("link: no thread %08x", (unsigned)g_thread);
        g_thread = -1;
        return 0;
    }
    g_state = LINK_JOINING;
    logline("link: joining connection %d again", profile);
    sceKernelStartThread(g_thread, 0, 0);
    return 1;
}

static void back(void) {
    sync_set_online(1);
    g_state = LINK_UP;
    g_polled = now_ms();
    shell_status(T_LINK_BACK);
    logline("link: back");
}

static void lose(const char *why) {
    logline("link: lost (%s)", why);
    sync_set_online(0);
    g_state = LINK_LOST;
    shell_status(T_LINK_LOST);
    join_start();
}

void link_tick(int watching) {
    int resumed = g_resumed;
    g_resumed = 0;
    if (g_state == LINK_JOINING) {
        if (!g_join_done) return;
        sceKernelWaitThreadEnd(g_thread, 0);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
        if (g_join_rc == 0) {
            back();
        } else {
            char line[96];
            g_state = LINK_LOST;
            if (g_join_rc == -2) snprintf(line, sizeof(line), "%s", T_LINK_SWITCH_OFF);
            else snprintf(line, sizeof(line), T_LINK_NOT_JOINED, g_join_name);
            shell_status(line);
        }
        return;
    }
    if (!watching || !on_wifi()) return;
    if (g_state == LINK_NONE || g_state == LINK_LOST) {
        /* Connected by the sync: from the start, or Connect after the
           session had begun without one. */
        if (sync_online()) {
            g_state = LINK_UP;
            g_was_up = 1;
            g_polled = now_ms();
        } else if (g_state == LINK_LOST && resumed) {
            /* Woken again with it still gone: another try of its own. */
            join_start();
        }
        return;
    }
    /* Up: something else let it go knowingly. */
    if (!sync_online()) {
        g_state = LINK_NONE;
        return;
    }
    if (!resumed && now_ms() - g_polled < LINK_POLL_MS) return;
    g_polled = now_ms();
    if (!apctl_up()) lose(resumed ? "standby" : "access point");
}

void link_reconnect(void) {
    if (g_state == LINK_JOINING) return;
    if (join_start()) return;
    /* No Auto Connect: the system's dialog, as the first time. Only the
       link comes back; the catalog fetched before is still the one. */
    preview_quiesce();
    int rc = netconf_connect();
    preview_resume();
    if (rc == 0 && apctl_up()) {
        back();
        link_offer_check();
    } else {
        shell_status(T_LINK_STILL_OFFLINE);
    }
}

void link_forget(void) {
    if (g_state == LINK_JOINING) return;
    g_state = LINK_NONE;
    g_was_up = 0;
}

const char *link_label(void) {
    static char line[96];
    if (g_state == LINK_JOINING) {
        snprintf(line, sizeof(line), T_STATUS_JOINING, g_join_name);
        return line;
    }
    return g_was_up ? T_RECONNECT : T_CONNECT;
}

const char *link_note(void) {
    static char note[192];
    if (!g_was_up) return T_CONNECT_NOTE;
    if (g_state == LINK_JOINING) return T_RECONNECT_JOINING;
    char name[64];
    int profile = options_auto_connect();
    if (profile && wifi_profile_name(profile, name, sizeof(name)) == 0)
        snprintf(note, sizeof(note), T_RECONNECT_NOTE_AUTO, name);
    else
        snprintf(note, sizeof(note), "%s", T_RECONNECT_NOTE);
    return note;
}

void link_offer_check(void) {
    if (options_auto_connect() || options_auto_offered() || !on_wifi()) return;
    union SceNetApctlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROFILE_NAME, &info) < 0 || !info.name[0]) return;
    /* The dialog says by name which it joined; the settings know it by
       number. */
    char name[64];
    for (int p = wifi_profile_next(0); p; p = wifi_profile_next(p))
        if (wifi_profile_name(p, name, sizeof(name)) == 0 && !strcmp(name, info.name)) {
            g_offer = p;
            snprintf(g_offer_name, sizeof(g_offer_name), "%s", name);
            logline("link: connection %d to offer to Auto Connect", p);
            return;
        }
}

int link_offer_take(char *name, size_t size) {
    int p = g_offer;
    g_offer = 0;
    if (!p || options_auto_connect() || options_auto_offered()) return 0;
    snprintf(name, size, "%s", g_offer_name);
    return p;
}
