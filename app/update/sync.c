#include <pspkernel.h>
#include <stdio.h>

#include "update/sync.h"
#include "network/https.h"
#include "util/runtime.h"

/* Below the main thread's priority: the browser keeps its frame rate and
   the network runs in the time the browser spends waiting for vblank,
   which is most of every frame. A handshake takes a moment longer and
   nothing on screen stutters for it. */
#define SYNC_PRIORITY 0x22
#define SYNC_STACK (128 * 1024)

static struct catalog *g_catalog;
static volatile enum sync_state g_state = SYNC_IDLE;
static char g_message[64];
static SceUID g_thread = -1;

static int run(SceSize args, void *argp) {
    (void)args; (void)argp;
    g_state = SYNC_CONNECTING;
    int online=net_up()>=0;
    catalog_offline(!online);
    if(!online)logline("offline: using saved catalogs and installed apps");
    g_state = SYNC_FETCHING;
    int count = catalog_fetch(g_catalog);
    if (count < 0) {
        snprintf(g_message, sizeof(g_message),
                 catalog_too_large() ? "catalog too large" : "catalog unreachable");
        g_catalog->count = 0;
        g_state = SYNC_FAILED;
        return 0;
    }
    g_catalog->count = count;
    g_state = SYNC_CHECKING;
    if (count > 0) catalog_check_updates(g_catalog);
    snprintf(g_message,sizeof(g_message),"%s",online?"":"Offline: last known releases");
    g_state = SYNC_DONE;
    return 0;
}

int sync_start(struct catalog *catalog) {
    /* Started again after a failure: the last thread has run its course
       and only has to be let go of. */
    if (g_thread >= 0) {
        if (!sync_done()) return -1;
        sceKernelWaitThreadEnd(g_thread, 0);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    g_catalog = catalog;
    g_state = SYNC_IDLE;
    g_thread = sceKernelCreateThread("sync", run, SYNC_PRIORITY, SYNC_STACK,
                                     PSP_THREAD_ATTR_USER, 0);
    if (g_thread < 0) {
        logline("sync: no thread %08x", (unsigned)g_thread);
        return -1;
    }
    sceKernelStartThread(g_thread, 0, 0);
    return 0;
}

enum sync_state sync_state(void) { return g_state; }

int sync_done(void) { return g_state == SYNC_DONE || g_state == SYNC_FAILED; }

const char *sync_message(void) {
    const char *phase = https_phase();
    const char *progress = catalog_progress();
    switch (g_state) {
    case SYNC_IDLE:       return "connecting";
    case SYNC_CONNECTING: return phase[0] ? phase : "connecting";
    /* A list being walked is one fetch per repository, and where it has
       got to says more than which of them is mid-handshake. */
    case SYNC_FETCHING:   return progress[0] ? progress : phase[0] ? phase : "catalog";
    case SYNC_CHECKING:   return "checking for updates";
    case SYNC_FAILED:     return g_message;
    default:              return g_message;
    }
}
