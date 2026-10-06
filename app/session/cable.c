#include "text.h"
#include "session/cable.h"

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
/* What dev/release writes beside the copy: the release it is of, by its
   tag, and the SHA-256 of that release's zip, which is what a catalog
   lists the release by and an update is told by. */
#define CABLE_NOTE "usbnet.txt"

static char g_dir[200];

void cable_init(const char *argv0) {
    const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (slash && (size_t)(slash - argv0) < sizeof(g_dir))
        snprintf(g_dir, sizeof(g_dir), "%.*s", (int)(slash - argv0), argv0);
}

static void beside(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s/%s", g_dir, name);
}

/* "tag=" and "zip=" out of the note, a line each. 0 with both. */
static int carried(char *tag, size_t tag_size, unsigned char zip[32]) {
    char path[256], *text = NULL, hex[65] = "";
    beside(path, sizeof(path), CABLE_NOTE);
    int n = g_dir[0] ? storage_read(path, &text, 512) : -1, ok = 0;
    tag[0] = '\0';
    for (char *line = n > 0 ? strtok(text, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "tag=", 4))
            snprintf(tag, tag_size, "%s", line + 4);
        if (!strncmp(line, "zip=", 4))
            snprintf(hex, sizeof(hex), "%s", line + 4);
    }
    ok = tag[0] && strlen(hex) == 64 && pspdx_hex(hex, zip, 32) == 0;
    free(text);
    beside(path, sizeof(path), CABLE_PRX);
    return ok && storage_exists(path) ? 0 : -1;
}

static int recorded(struct installed *rec) {
    return db_read(CABLE_ID, rec) == 0 && rec->plugin[0];
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
    return choice == CABLE_UNASKED && !cable_installed() && (usbnet_kernel() || played()) &&
           carried(tag, sizeof(tag), zip) == 0;
}

int cable_ready(void) { return usbnet_loaded() || g_played; }

static int load(void) {
    struct installed rec;
    char path[256];
    if (played()) {
        g_played = 1;
        return 0;
    }
    /* The installed copy where there is one: the one the firmware loads
       from the next start on. */
    if (recorded(&rec))
        snprintf(path, sizeof(path), "%s/seplugins/%s", rec.device, rec.plugin);
    if (!recorded(&rec) || !storage_exists(path))
        beside(path, sizeof(path), CABLE_PRX);
    return storage_exists(path) ? usbnet_load(path) : -1;
}

void cable_start(enum cable choice) {
    struct installed rec;
    if ((choice == CABLE_USB || choice == CABLE_USB_KNOWN) && !recorded(&rec) && !usbnet_loaded())
        load();
}

int cable_use(char *said, size_t size) {
    struct installed rec;
    struct manifest m;
    struct install_report report;
    char prx[256];
    said[0] = '\0';
    memset(&m, 0, sizeof(m));
    memset(&report, 0, sizeof(report));
    if (!recorded(&rec)) {
        /* The record a store install of this release would have written:
           the catalog's id, the tag for a version and the zip's hash, by
           which the same release is current and a newer one an update. */
        static char raw[] = "{\"schema\":\"" PSPDX_SCHEMA "\",\"source\":\"" CABLE_SOURCE
                            "\",\"name\":\"USBNet\",\"type\":\"plugin\",\"category\":\"plugin\"}";
        char tag[VERSION_SIZE];
        beside(prx, sizeof(prx), CABLE_PRX);
        int rc = carried(tag, sizeof(tag), m.sha256);
        snprintf(m.id, sizeof(m.id), "%s", CABLE_ID);
        snprintf(m.repo, sizeof(m.repo), "%s", CABLE_SOURCE);
        snprintf(m.version, sizeof(m.version), "%s", tag + (tag[0] == 'v' && tag[1]));
        m.raw = raw;
        if (rc == 0)
            rc = install_bundled(&m, prx, storage_device(), &report);
        if (rc < 0) {
            snprintf(said, size, T_PLUGIN_NOT_INSTALLED, rc == INSTALL_NO_SPACE ? T_WHY_NO_ROOM
                     : report.why[0] ? report.why : T_WHY_STICK);
            logline("cable: %s", said);
        }
    }
    /* Choosing the cable is the word to turn it on. */
    if (recorded(&rec) && plugin_switch(CABLE_ID, 1) != 1 && !said[0]) {
        if (plugin_refused()[0])
            snprintf(said, size, T_PLUGIN_FAILED_WHY, plugin_refused());
        else
            snprintf(said, size, "%s", T_PLUGIN_NOT_OURS);
        logline("cable: %s", said);
    }
    return load();
}
