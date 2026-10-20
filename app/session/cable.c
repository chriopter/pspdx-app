#include "text.h"
#include "session/cable.h"
#include "session/carried.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "install/state.h"
#include "network/usbnet.h"
#include "update/pspdx.h"
#include "util/runtime.h"
#include "util/storage.h"

#define CABLE_SOURCE "https://github.com/chriopter/pspkit-usbnet"
#define CABLE_PRX "usbnet.prx"

/* What the program carries of the plugin: its bytes, and the release it is
   of -- by its tag, and by the SHA-256 of that release's zip, which is what
   a catalog lists the release by and an update is told by. 0 with it. */
static int carried(const unsigned char **prx, size_t *len, char *tag, size_t tag_size,
                   unsigned char zip[32]) {
    const char *t;
    const unsigned char *z;
    if (carried_plugin(prx, len, &t, &z) < 0 || !*len || !t[0])
        return -1;
    snprintf(tag, tag_size, "%s", t);
    memcpy(zip, z, 32);
    return 0;
}

static int recorded(struct installed *rec) {
    return db_read(CABLE_ID, rec) == 0 && rec->plugin[0];
}

int cable_recorded(void) {
    struct installed rec;
    return recorded(&rec);
}

int cable_installed(void) {
    struct installed rec;
    return recorded(&rec) || usbnet_loaded();
}

/* The emulator has no kernel to ask or to load into, so the cable's
   questions would never be seen at the desk. With PSPDX.CABLE on its stick
   the plugin is installed as on a PSP and its loading only played; where
   there is a kernel the file says nothing. */
static int g_played;
static int played(void) {
    return !usbnet_kernel() && storage_exists(storage_path("PSP/PSPDX/DEBUG/PSPDX.CABLE"));
}

int cable_asks(enum cable choice) {
    char tag[VERSION_SIZE];
    unsigned char zip[32];
    const unsigned char *prx;
    size_t len;
    return choice == CABLE_UNASKED && !cable_installed() && (usbnet_kernel() || played()) &&
           carried(&prx, &len, tag, sizeof(tag), zip) == 0;
}

int cable_ready(void) { return usbnet_loaded() || g_played; }

/* Played, the file's first character is the gateway: 1 there, 0 not, and
   anything else a plugin that cannot say. */
enum cable_look cable_look(void) {
    int there = -1;
    if (g_played) {
        char *text = NULL;
        if (storage_read(storage_path("PSP/PSPDX/DEBUG/PSPDX.CABLE"), &text, 16) > 0 &&
            (text[0] == '0' || text[0] == '1'))
            there = text[0] == '1';
        free(text);
    } else {
        there = usbnet_probe(CABLE_LOOK_MS);
    }
    return there > 0 ? CABLE_FOUND : there == 0 ? CABLE_NOT_FOUND : CABLE_NOT_SAID;
}

enum cable_ask cable_after_look(enum cable choice, enum cable_look look) {
    if (choice != CABLE_USB) return CABLE_ASK_NOTHING;
    return look == CABLE_FOUND ? CABLE_ASK_CONNECT : look == CABLE_NOT_FOUND ? CABLE_ASK_RETRY
                                                                             : CABLE_ASK_RUNNING;
}

/* The carried plugin as a file, for a session it is not installed in --
   the installer refused, or it was deleted with the cable still chosen:
   the firmware loads a module from a file and nothing else. In the cache,
   written when what lies there is not it. NULL where it cannot be had. */
static const char *carried_file(void) {
    const char *path = storage_path("PSP/PSPDX/CACHE/usbnet.prx");
    const unsigned char *prx, *zip;
    const char *tag;
    size_t len;
    char *there = NULL;
    if (carried_plugin(&prx, &len, &tag, &zip) < 0 || !len)
        return NULL;
    int n = storage_read(path, &there, len + 1);
    int same = n >= 0 && (size_t)n == len && !memcmp(there, prx, len);
    free(there);
    if (!same && storage_write_cache(path, prx, len) < 0)
        return NULL;
    return path;
}

static int load(void) {
    struct installed rec;
    char path[256] = "";
    if (played()) {
        g_played = 1;
        return 0;
    }
    /* The installed copy where there is one: the one the firmware loads
       from the next start on. */
    if (recorded(&rec))
        plugin_path(&rec, path, sizeof(path));
    if (!path[0] || !storage_exists(path)) {
        const char *file = carried_file();
        snprintf(path, sizeof(path), "%s", file ? file : "");
    }
    return path[0] && storage_exists(path) ? usbnet_load(path) : -1;
}

static int restart_needed;
static char startup_notice[128];

int cable_restart_needed(void) { return restart_needed; }
const char *cable_start_notice(void) { return startup_notice; }

/* Only comparable release versions may replace an installed release offline.
   Unknown/custom versions and a newer store release are kept. */
static int release_version(const char *s, unsigned v[3]) {
    if (*s == 'v') s++;
    for (int i = 0; i < 3; i++) {
        unsigned n = 0;
        if (*s < '0' || *s > '9') return -1;
        do {
            unsigned digit = (unsigned)(*s++ - '0');
            if (n > (UINT32_MAX - digit) / 10) return -1;
            n = n * 10 + digit;
        } while (*s >= '0' && *s <= '9');
        v[i] = n;
        if (i < 2 && *s++ != '.') return -1;
    }
    return *s ? -1 : 0;
}

static int refresh_needed(const struct installed *rec) {
    const unsigned char *prx;
    unsigned char zip[32];
    char tag[VERSION_SIZE];
    size_t len;
    unsigned installed[3], bundled[3];
    if (carried(&prx, &len, tag, sizeof(tag), zip) < 0 ||
        !memcmp(rec->sha256, zip, 32) ||
        release_version(rec->version, installed) < 0 || release_version(tag, bundled) < 0)
        return 0;
    for (int i = 0; i < 3; i++) {
        if (installed[i] > bundled[i]) return 0;
        if (installed[i] < bundled[i]) return 1;
    }
    return 1; /* republished release, still verified by the embedded zip hash */
}

static int cable_prepare(char *said, size_t size, int enable);

void cable_start(enum cable choice) {
    struct installed rec;
    if (choice != CABLE_USB && choice != CABLE_USB_KNOWN) return;
    if (recorded(&rec)) {
        if (refresh_needed(&rec)) {
            cable_prepare(startup_notice, sizeof(startup_notice), 0);
        }
    } else if (!usbnet_loaded()) {
        load();
    }
}

static int cable_prepare(char *said, size_t size, int enable) {
    struct installed rec;
    struct manifest m;
    struct install_report report;
    said[0] = '\0';
    memset(&m, 0, sizeof(m));
    memset(&report, 0, sizeof(report));
    int has = recorded(&rec);
    int resident = usbnet_loaded();
    if (!has || refresh_needed(&rec)) {
        /* The record a store install of this release would have written:
           the catalog's id, the tag for a version and the zip's hash, by
           which the same release is current and a newer one an update. */
        static char raw[] = "{\"schema\":\"" PSPDX_SCHEMA "\",\"source\":\"" CABLE_SOURCE
                            "\",\"name\":\"USBNet\",\"type\":\"plugin\",\"category\":\"plugin\"}";
        char tag[VERSION_SIZE] = "";
        const unsigned char *prx;
        size_t len;
        int rc = carried(&prx, &len, tag, sizeof(tag), m.sha256);
        snprintf(m.id, sizeof(m.id), "%s", CABLE_ID);
        snprintf(m.repo, sizeof(m.repo), "%s", CABLE_SOURCE);
        snprintf(m.version, sizeof(m.version), "%s", tag + (tag[0] == 'v' && tag[1]));
        m.raw = raw;
        if (rc == 0)
            rc = install_bundled(&m, CABLE_PRX, prx, len, storage_device(), &report);
        if (rc == 0 && resident) {
            restart_needed = 1;
            snprintf(said, size, "%s", T_CABLE_RESTART);
            logline("usbnet: installed %s; resident module kept, full PSP restart required", tag);
        }
        if (rc < 0) {
            snprintf(said, size, T_PLUGIN_NOT_INSTALLED, rc == INSTALL_NO_SPACE ? T_WHY_NO_ROOM
                     : report.why[0] ? report.why : T_WHY_STICK);
            logline("cable: %s", said);
        }
    }
    /* Choosing the cable is the word to turn it on. */
    if (enable && recorded(&rec) && plugin_switch(CABLE_ID, 1) != 1 && !said[0]) {
        if (plugin_refused()[0])
            snprintf(said, size, T_PLUGIN_FAILED_WHY, plugin_refused());
        else
            snprintf(said, size, "%s", T_PLUGIN_NOT_OURS);
        logline("cable: %s", said);
    }
    if (!enable && plugin_enabled(CABLE_ID) != 1) return 0;
    return load();
}

int cable_use(char *said, size_t size) {
    return cable_prepare(said, size, 1);
}
