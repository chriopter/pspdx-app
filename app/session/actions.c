#include "text.h"
/*
 * What the session does to the stick and the catalog: installs, one or a
 * run of them, removing, starting a package, fetching the catalog again,
 * the sweep, the cache and the reset. Installs are queued; dialogs and
 * completion messages stay on the main thread. The questions
 * that stand before them are session/questions.c's; the options that
 * lead to them, session/options.c's.
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspinit.h>
#include <psploadexec_kernel.h>
#include <systemctrl.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio.h"
#include "audio/cues.h"
#include "gui/entropy_screen.h"
#include "gui/files_view.h"
#include "gui/gfx.h"
#include "gui/osk.h"
#include "gui/netconf.h"
#include "gui/preview.h"
#include "gui/shell.h"
#include "install/install.h"
#include "install/state.h"
#include "pspkit-https/entropy.h"
#include "pspkit-https/https.h"
#include "session/actions.h"
#include "session/cable.h"
#include "session/downloads.h"
#include "session/options.h"
#include "session/questions.h"
#include "session/view.h"
#include "update/assets.h"
#include "update/inbox.h"
#include "update/sources.h"
#include "update/sync.h"
#include "util/runtime.h"
#include "util/storage.h"

/* The catalog is main.c's; every action here works on it through this. */
static struct catalog *g_catalog;

void actions_init(struct catalog *catalog) {
    g_catalog = catalog;
}

struct catalog *actions_catalog(void) {
    return g_catalog;
}

/* The log every time; the catalog's raw response once, after it arrived
   -- it is up to 512 KB and does not change, and writing it every ten seconds
   was a visible hitch. */
static int g_http_dumped;

void dump_diagnostics(void) {
    if (downloads_busy()) { log_dump_later(); return; }
    log_dump();
    if (!g_http_dumped && sync_done()) {
        catalog_dump_http();
        g_http_dumped = 1;
    }
}

/* Until the catalog is here the browser has nothing to browse; it is on
   screen anyway, saying what it waits for. */
static struct catalog empty;

const struct catalog *shown(void) {
    return sync_done() ? g_catalog : &empty;
}

/* Let the transitions finish before photographing the screen, but not
   forever: six seconds covers a film being fetched and started. */
void screenshot_settled(int cursor, const char *path) {
    /* At least one frame, so the shell has seen the catalog it is about
       to be judged on. */
    for (int i = 0; i < 360; i++) {
        shell_shot_sync(shown(), cursor);
        shell_draw(shown(), cursor);
        if (shell_settled()) break;
    }
    gfx_screenshot(path);
}

/* Set when PSPDX has replaced itself: the loop asks to restart once the
   install that did it, or the batch it was the last of, is over. */
static int g_restart_of = -1;
static char g_restart_version[VERSION_SIZE];

/* Main thread, after parking the media worker. A cancelled system dialog
   stays offline; only a later explicit action asks again. */
static int connect_online(void) {
    if (storage_exists(storage_path("PSP/PSPDX/DEBUG/PSPDX.OFFLINE"))) return 0;
    if (https_net_connect() == 0) return 1;
    return netconf_connect() == 0 && https_net_connect() == 0;
}

/* A plugin installed for the first time, and off: the row the loop asks
   about once the downloads are through, as it asks about the restart. */
/* Each of them, in the order they went in: a batch from INBOX installs
   several, and one slot asked about the last alone. */
#define INSTALLED_ASKS 16
static int g_installed_of[INSTALLED_ASKS], g_installed_count;

int plugin_installed_take(void) {
    if (downloads_busy() || !g_installed_count) return -1;
    int of = g_installed_of[0];
    g_installed_count--;
    memmove(g_installed_of, g_installed_of + 1, (size_t)g_installed_count * sizeof(*g_installed_of));
    return of;
}

int restart_take(char *version, size_t size) {
    if (downloads_busy()) return -1;
    int of = g_restart_of;
    if (of < 0) return -1;
    snprintf(version, size, "%s", g_restart_version);
    g_restart_of = -1;
    return of;
}

/* Every install route meets here, including the basket and INBOX. Existing
   apps retain their device so an update cannot strand saves on another one. */
static int install_device(const struct app_entry *entry, int row, char out[5]) {
    struct installed rec;
    if (db_read(entry->id, &rec) == 0) {
        snprintf(out, 5, "%s", rec.device);
        if (!storage_device_available(out)) {
            error_show(T_STORAGE_MISSING, "device %s of %s", out, entry->id);
            return -1;
        }
        return 0;
    }
    snprintf(out, 5, "%s", storage_device());
    if (!storage_is_go()) return 0;
    static struct menu choice;
    memset(&choice, 0, sizeof(choice));
    memset(choice.key, -1, sizeof(choice.key));
    choice.title = T_STORAGE_ASK;
    choice.item[0] = T_STORAGE_INTERNAL;
    choice.item[1] = T_STORAGE_CARD;
    choice.count = 2;
    const char *devices[] = { "ef0:", "ms0:" };
    choice.cursor = !strcmp(out, "ms0:") && storage_device_available("ms0:") ? 1 : 0;
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned last = pad.Buttons;
    shell_menu(&choice);
    int rc = INSTALL_CANCELLED;
    for (;;) {
        for (int i = 0; i < 2; i++) {
            choice.on[i] = storage_device_available(devices[i]);
            choice.value[i] = choice.on[i] ? NULL : T_STORAGE_ABSENT;
        }
        shell_draw(g_catalog, row);
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last;
        last = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) break;
        if (pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) choice.cursor ^= 1;
        if ((pressed & PSP_CTRL_CROSS) && choice.on[choice.cursor]) {
            snprintf(out, 5, "%s", devices[choice.cursor]);
            rc = 0;
            break;
        }
    }
    shell_menu(NULL);
    return rc;
}

/* The answer's button let go before the caller reads the pad again: under
   Options the O that left a question would leave the view too. */
static void released(void) {
    SceCtrlData pad;
    do {
        shell_draw(g_catalog, 0);
        sceCtrlPeekBufferPositive(&pad, 1);
    } while (pad.Buttons & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE));
}

/* A question of the caller's standing over the room until X or O: 1 for X.
   yes and no are what the two mean in the footer. */
static int answer_with(const char *title, const char *line, const char *yes, const char *no) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned last = pad.Buttons;
    int said = -1;
    shell_ask_with(title, line, yes, no);
    while (said < 0) {
        shell_draw(g_catalog, 0);
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last;
        last = pad.Buttons;
        if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)) said = (pressed & PSP_CTRL_CROSS) != 0;
    }
    shell_ask(NULL, NULL);
    released();
    return said;
}

static int answer(const char *title, const char *line) {
    return answer_with(title, line, T_YES, T_NO);
}

/* The gateway looked for, on a thread of its own since the plugin answers
   only when it knows or the time is up, and the room keeps drawing under
   the notice of it. The notice stands a second at the least: the press
   that said yes to the plugin is often two, and the second would answer
   what comes next unread. O leaves: -1, and the look ends by itself, its
   answer the next look's if that comes before it has. */
static volatile int g_looking;
static volatile enum cable_look g_looked;

static int look_thread(SceSize size, void *arg) {
    (void)size;
    (void)arg;
    g_looked = cable_look();
    g_looking = 0;
    return sceKernelExitDeleteThread(0);
}

static int look(void) {
    if (!g_looking) {
        int thread = sceKernelCreateThread("gateway", look_thread, 0x20, 0x4000, PSP_THREAD_ATTR_USER, NULL);
        if (thread < 0) return CABLE_NOT_SAID;
        g_looking = 1;
        if (sceKernelStartThread(thread, 0, NULL) < 0) {
            g_looking = 0;
            sceKernelDeleteThread(thread);
            return CABLE_NOT_SAID;
        }
    }
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned last = pad.Buttons, since = sceKernelGetSystemTimeLow();
    int left = 0;
    shell_ask_with(T_CABLE_LOOKING, "", NULL, T_HINT_CANCEL);
    while (!left && (g_looking || sceKernelGetSystemTimeLow() - since < 1000000)) {
        shell_draw(g_catalog, 0);
        sceCtrlPeekBufferPositive(&pad, 1);
        left = (pad.Buttons & ~last & PSP_CTRL_CIRCLE) != 0;
        last = pad.Buttons;
    }
    shell_ask(NULL, NULL);
    if (left) released();
    return left ? -1 : (int)g_looked;
}

/* The look and what is asked after it, Retry looking again. 1: to the
   system's dialog; 0: this session stays without a connection. */
static int gateway_asked(enum cable choice) {
    /* Where the gateway is to be had is said before it is looked for:
       whoever has none yet reads it here, not only after a search that
       could not have found one. */
    if (!answer(T_CABLE_SEARCH_ASK, T_CABLE_GATEWAY_LINE)) return 0;
    for (;;) {
        int found = look();
        if (found < 0) return 0;
        switch (cable_after_look(choice, (enum cable_look)found)) {
        case CABLE_ASK_CONNECT:
            return answer(T_CABLE_FOUND_ASK, T_CABLE_CONNECT_LINE);
        case CABLE_ASK_RUNNING:
            return answer(T_CABLE_GATEWAY_ASK, T_CABLE_GATEWAY_LINE) &&
                   answer(T_CABLE_CONNECT_ASK, T_CABLE_CONNECT_LINE);
        case CABLE_ASK_RETRY:
            if (!answer_with(T_CABLE_NONE, T_CABLE_NONE_LINE, T_CABLE_RETRY, T_HINT_CANCEL)) return 0;
            break;
        default:
            return 1;
        }
    }
}

/* The cable chosen: its plugin installed, turned on and loaded, and the
   choice remembered. said: what was refused on the way, or that the module
   is not loaded; empty when the cable is ready. Returns whether it is. */
/* The cable's plugin reaches the stick through cable_use, not through a
   download, so nothing has told the store's row for it: installed, by the
   record just written, and on or off as the list says. Without this the
   row went on offering Install, and the Installed tab left it out, until
   PSPDX was started again. */
static void cable_row(void) {
    struct installed rec;
    if (!g_catalog || db_read(CABLE_ID, &rec) < 0 || !rec.plugin[0]) return;
    int at = catalog_find_repo(g_catalog, rec.repo);
    if (at < 0) return;
    struct app_entry *entry = &g_catalog->apps[at];
    if (entry->state == APP_NOT_INSTALLED) {
        entry->local_rev = rec.rev;
        text_set(&entry->local_version, rec.version);
        memcpy(entry->local_sha256, rec.sha256, sizeof(entry->local_sha256));
        entry->local_has_sha = 1;
        entry->state = entry->has_release && memcmp(entry->release.sha256, rec.sha256, 32)
                           ? APP_UPDATE : APP_CURRENT;
        snprintf(entry->type, sizeof(entry->type), "plugin");
    }
    entry->plugin_off = plugin_enabled(CABLE_ID) != 1;
    view_rebuild(g_catalog);
}

static int cable_set_up(char *said, size_t size) {
    int loaded = cable_use(said, size) == 0;
    cable_row();
    if (options_cable() < CABLE_USB) options_cable_set(CABLE_USB);
    if (!loaded && !said[0]) snprintf(said, size, "%s", T_CABLE_NOT_LOADED);
    return loaded;
}

/* How to connect, asked: the menu, and what the answer leads to. The
   first start asks it, and the gear's row asks it again. */
enum { CABLE_LEFT = -1,     /* O: nothing answered, nothing changed */
       CABLE_STAY,          /* refused on the way, and not to connect now */
       CABLE_GO_ON,         /* the cable is set up: the gateway next */
       CABLE_PLAIN };       /* Wi-Fi, or on without the cable after all */
static int cable_asked(void) {
    static struct menu choice;
    memset(&choice, 0, sizeof(choice));
    memset(choice.key, -1, sizeof(choice.key));
    choice.title = T_CABLE_ASK;
    choice.item[0] = T_CABLE_WIFI;
    choice.item[1] = T_CABLE_USB;
    choice.on[0] = choice.on[1] = 1;
    choice.count = 2;
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned last = pad.Buttons;
    shell_menu(&choice);
    for (;;) {
        shell_draw(g_catalog, 0);
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last;
        last = pad.Buttons;
        if (pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) choice.cursor ^= 1;
        if (!(pressed & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE))) continue;
        shell_menu(NULL);
        if (!(pressed & PSP_CTRL_CROSS)) return CABLE_LEFT;
        if (!choice.cursor) {
            options_cable_set(CABLE_WIFI);
            return CABLE_PLAIN;
        }
        /* Said before anything is put on the stick; No is back at the
           choice. A plugin that is installed already is only turned on;
           one that is merely loaded, deleted in this session, is not
           installed. */
        if (!cable_recorded() && !answer(T_CABLE_INSTALL_ASK, T_CABLE_INSTALL_LINE)) {
            sceCtrlPeekBufferPositive(&pad, 1);
            last = pad.Buttons;
            shell_menu(&choice);
            continue;
        }
        /* What was refused is said where it is read, the system's dialog
           being the next thing on the screen; and the cable still asked
           about where the session has it after all. No to that takes the
           choice back too: a cable that was refused and not wanted is not
           what the next start should find chosen. */
        int before = options_cable();
        char said[64];
        int loaded = cable_set_up(said, sizeof(said));
        if (said[0]) {
            int yes = answer(said, loaded ? T_CABLE_GO_ON_LINE : T_CABLE_CONNECT_ASK);
            if (!yes && before < CABLE_USB) options_cable_set(before);
            if (!yes) return CABLE_STAY;
            if (!loaded) return CABLE_PLAIN;
        }
        return CABLE_GO_ON;
    }
}

int cable_at_start(void) {
    if (cable_asks(options_cable())) {
        /* O answers nothing: Wi-Fi this once, and asked again next time. */
        int chosen = cable_asked();
        if (chosen == CABLE_STAY) return 0;
        if (chosen != CABLE_GO_ON) return 1;
    } else {
        cable_start((enum cable)options_cable());
    }
    /* Until the cable has connected once, the two things nobody can know
       are settled before the system's dialog opens: that the cable leads
       nowhere without the gateway on the PC, which is looked for, and where
       that is to be had; then what to choose in the dialog, which has no
       ready connection for the cable. */
    if (options_cable() != CABLE_USB || !cable_ready()) return 1;
    return gateway_asked(CABLE_USB);
}

void cable_set_up_again(void) {
    if (downloads_busy()) { error_busy(); return; }
    int chosen = cable_asked();
    if (chosen == CABLE_LEFT || chosen == CABLE_STAY) return;
    /* The gateway is looked for whenever the cable was chosen here, known
       from an earlier connection or not: whoever comes back to this row
       has a reason to. */
    if (chosen == CABLE_GO_ON && cable_ready() && !gateway_asked(CABLE_USB)) return;
    /* And the connection made anew, through the system's dialog, since
       the one that stands may be the other kind. */
    preview_quiesce();
    https_net_disconnect();
    int online = netconf_connect() == 0 && https_net_connect() == 0;
    catalog_offline(!online);
    preview_resume();
    if (online) cable_connected();
    shell_status(online ? (options_cable() == CABLE_WIFI ? T_CABLE_OFF : "") : T_STATUS_OFFLINE);
}

void cable_connected(void) {
    if (options_cable() == CABLE_USB && cable_ready()) options_cable_set(CABLE_USB_KNOWN);
}

/* Checked against the selected device and the author's final directory,
   after catalog_prepare, rather than the catalog's provisional folder. */
static int install_folder(const struct app_entry *entry, const char *dev, int row) {
    struct installed rec;
    if (state_target_owner_on(entry->release.dir, entry->id, dev) != 0) return -1;
    if (!strcmp(dev, storage_device()) &&
        !strcasecmp(entry->release.dir, storage_self_dir()) && strcmp(entry->id, PSPDX_SELF_ID))
        return -1;
    if (db_read(entry->id, &rec) == 0 && !strcmp(rec.device, dev) &&
        !strcasecmp(rec.dir, entry->release.dir)) return 0;
    char dest[160], bak[168], title[96], line[200];
    snprintf(dest, sizeof(dest), "%s/PSP/GAME/%s", dev, entry->release.dir);
    if (!storage_exists(dest)) return 0;
    snprintf(bak, sizeof(bak), "%s.bak", dest);
    if (storage_exists(bak)) return -1;
    snprintf(title, sizeof(title), T_DIR_EXISTS_ASK, entry->release.dir);
    snprintf(line, sizeof(line), T_DIR_EXISTS_LINE, entry->release.dir);
    shell_ask(title, line);
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned last = pad.Buttons;
    int rc = INSTALL_CANCELLED;
    for (;;) {
        shell_draw(g_catalog, row);
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last;
        last = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) break;
        if (pressed & PSP_CTRL_CROSS) {
            char name[80];
            snprintf(name, sizeof(name), "%s.bak", entry->release.dir);
            rc = sceIoRename(dest, name) < 0 ? -1 : 0;
            break;
        }
    }
    shell_ask(NULL, NULL);
    return rc;
}

/* The rule for a zip with more than one EBOOT.PBP, put to whoever is
   installing: which folder of the zip goes where, and what of the rest
   comes along. The installer is sure of the zip but not of what its author
   meant, so nothing is written until X. */
int actions_download_layout(const struct app_entry *entry, const struct install_layout *layout,
                            int row) {
    (void)row;
    logline("layout: asked for %s: %s to PSP/GAME/%s, %d along, %d groups left out",
            entry->id, layout->root[0] ? layout->root : "the top", layout->dir, layout->nested,
            layout->left_groups);
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned last = pad.Buttons;
    int rc = -1;
    for (;;) {
        shell_layout_draw(entry, layout);
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last;
        last = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) break;
        if (pressed & PSP_CTRL_CROSS) { rc = 0; break; }
    }
    logline("layout: %s", rc == 0 ? "installed so" : "declined");
    return rc;
}

int actions_download_device(const struct app_entry *entry, char out[5]) {
    int row = view_row((int)(entry - g_catalog->apps));
    return install_device(entry, row < 0 ? 0 : row, out);
}
int actions_download_connect(void) {
    int online = connect_online();
    if (online) catalog_offline(0);
    /* Until now the job only turned to Failed in the list. */
    else error_show(T_DOWNLOAD_OFFLINE, "download connect");
    return online;
}
int actions_download_folder(const struct app_entry *entry, const char *dev, int row) {
    return install_folder(entry, dev, row);
}
void actions_download_complete(int index, struct app_entry *prepared,
                               const struct install_report *result, int rc, unsigned seconds) {
    struct app_entry *entry = &g_catalog->apps[index];
    struct install_report report = *result;
    if (rc == 0) {
        entry_move_release(entry, prepared);
        entry->no_pspdx = prepared->no_pspdx;
        view_basket_forget(index);
    }
    /* A name and a version of 64 characters whole; the status line cuts it
       to its own room, between letters. */
    char message[sizeof(entry->name) + VERSION_SIZE + 64];
    int first = entry->state == APP_NOT_INSTALLED;
    if (rc == 0) {
        entry->state = APP_CURRENT;
        entry->local_rev = report.rev;
        text_set(&entry->local_version, report.version);
        /* The client can fetch itself, and just has: the EBOOT that is running
           is the one in RAM, and the file it was loaded from has been renamed
           aside and replaced underneath it. Nothing on screen is the new
           version until the console loads it, so the band says which button
           does that rather than reporting a file count nobody needs. */
        if (strcmp(entry->id, PSPDX_SELF_ID) == 0) {
            snprintf(message, sizeof(message), T_UPDATED_SELF, report.version);
            snprintf(g_restart_version, sizeof(g_restart_version), "%s", report.version);
            g_restart_of = index;
        } else if (report.plugin[0] && !report.plugin_off) {
            snprintf(message, sizeof(message), T_PLUGIN_UPDATED, entry->name, report.version);
        } else {
            /* Said, since the next check will not find the author's words
               there either. */
            snprintf(message, sizeof(message),
                     entry->no_pspdx == PSPDX_FROM_FILE         ? T_INSTALLED_FROM_FILE
                     : entry->no_pspdx == PSPDX_FROM_REPOSITORY ? T_INSTALLED_NO_PSPDX
                                                                : T_INSTALLED,
                     entry->name, report.version);
        }
        if (report.plugin[0]) {
            snprintf(entry->type, sizeof(entry->type), "plugin");
            entry->plugin_off = report.plugin_off;
            if (first && report.plugin_off && g_installed_count < INSTALLED_ASKS)
                g_installed_of[g_installed_count++] = index;
        }
        pspdx_utf8_mend(message);
        logline("installed %s %s: %d files, %luK, %us", entry->name, report.version,
                report.files, (unsigned long)(report.bytes / 1024), seconds);
    } else if (rc == INSTALL_CANCELLED) {
        snprintf(message, sizeof(message), T_CANCELLED, entry->name);
    } else if (rc == INSTALL_DECLINED) {
        snprintf(message, sizeof(message), T_DECLINED, entry->name);

    } else if (rc == INSTALL_NO_SPACE) {
        /* Tenths, rounded up: what is said to be needed has to be enough. */
        unsigned long long tenths = (report.needed * 10 + (1u << 20) - 1) >> 20;
        snprintf(message, sizeof(message), T_NO_SPACE, entry->name, (unsigned long)(tenths / 10),
                 (unsigned long)(tenths % 10));
    } else if (report.why[0] && !strcmp(prepared->type, "plugin")) {
        /* A plugin's reasons are sentences about the stick; the line has no
           room for its name beside them. */
        snprintf(message, sizeof(message), T_PLUGIN_NOT_INSTALLED, report.why);
    } else if (report.why[0]) {
        snprintf(message, sizeof(message), T_INSTALL_FAILED_WHY, entry->name, report.why);
    } else {
        snprintf(message, sizeof(message), T_INSTALL_FAILED, entry->name, rc);
    }
    /* A cancelled install is what was asked for; anything else that did
       not end in an install is an error and stays until it is read. */
    if (rc == 0 || rc == INSTALL_CANCELLED || rc == INSTALL_DECLINED) shell_status(message);
    else error_show(message, "install %d %s", rc, entry->id);
    cues_post(rc == 0 ? CUE_DONE : CUE_FAIL, 0);
}

void uninstall_app(int index) {
    if (downloads_busy()) { error_busy(); return; }
    struct app_entry *entry = &g_catalog->apps[index];
    /* Room for the longest name whole; the line it is shown in cuts it. */
    char message[sizeof(entry->name) + 32];

    cues_post(CUE_OPEN, 0);
    int plugin = !strcmp(entry->type, "plugin");
    int rc = uninstall(entry->id);
    if (rc >= 0) {
        /* The catalog entry is what the browser reads; the record it was
           built from has just stopped existing. */
        entry->state = APP_NOT_INSTALLED;
        entry->local_rev = 0;
        text_free(&entry->local_version);
        entry->plugin_off = 0;
        if (plugin && rc)
            snprintf(message, sizeof(message), "%s",
                     rc & UNINSTALL_FILE ? T_PLUGIN_LEFT : rc & UNINSTALL_LINES ? T_PLUGIN_REMOVED_LINES
                                                                                  : T_PLUGIN_KEPT);
        else
            snprintf(message, sizeof(message), plugin ? T_PLUGIN_REMOVED : T_REMOVED, entry->name);
    } else if (rc == INSTALL_SELF) {
        snprintf(message, sizeof(message), "%s", T_SELF_DELETE);
    } else if (plugin_refused()[0]) {
        snprintf(message, sizeof(message), T_PLUGIN_NOT_DELETED, plugin_refused());
    } else {
        snprintf(message, sizeof(message), T_REMOVE_FAILED, entry->name, rc);
    }
    logline("%s", message);
    if (rc >= 0) shell_status(message);
    else error_show(message, "remove %d %s", rc, entry->id);
    cues_post(rc >= 0 ? CUE_DONE : CUE_FAIL, 0);
}

/* An installed plugin turned on, or off again: PSPDX's own line in
   PLUGINS.TXT, which the custom firmware reads when the PSP starts. A line
   somebody else wrote for the plugin is theirs, so what was asked for may
   not be what the list says afterwards, and then that is said. */
void switch_plugin(int index) {
    if (downloads_busy()) { error_busy(); return; }
    struct app_entry *entry = &g_catalog->apps[index];
    char message[96];
    int want = entry->plugin_off, on = plugin_switch(entry->id, want);
    if (on >= 0) entry->plugin_off = !on;
    if (on < 0 && plugin_refused()[0])
        snprintf(message, sizeof(message), T_PLUGIN_FAILED_WHY, plugin_refused());
    else
        snprintf(message, sizeof(message), "%s",
                 on < 0 ? T_PLUGIN_FAILED : on != want ? T_PLUGIN_NOT_OURS
                 : on ? T_PLUGIN_ON : T_PLUGIN_OFF);
    logline("%s: %s", entry->id, message);
    if (on == want) shell_status(message);
    else error_show(message, "plugin %s %d %s", want ? "on" : "off", on, entry->id);
    cues_post(on == want ? CUE_DONE : CUE_FAIL, 0);
}

/* After anything that changes what is in the tabs rather than what is in the
   catalog -- an install that was the last update waiting, a basket filled or
   emptied. The cursor stays on the package it was on for as long as that
   package is still shown; a tab that has gone out from under it puts it back
   on Homebrew at the top, which is the only row that is certainly there. */
void view_settled(int *cursor) {
    int at = view_index(*cursor);
    if (!view_tabs_refresh()) { *cursor = 0; return; }
    int row = at >= 0 ? view_row(at) : -1;
    int count = view_count();
    if (row >= 0) *cursor = row;
    else if (*cursor >= count) *cursor = count > 0 ? count - 1 : 0;
}

/* Hands the PSP over to the package the cursor is on. Nothing comes back from
   this call, so everything the session was holding has to be on the stick
   before it: the pool above all, which otherwise only reaches the seed file
   when the user quits through HOME. */
void launch_app(int index) {
    if (downloads_busy()) { error_busy(); return; }
    const struct app_entry *entry = &g_catalog->apps[index];
    struct installed record;
    char path[160];

    /* A plugin is not started from here: the custom firmware loads it when
       the PSP starts. What its row does is turn it off and on, asked first,
       since nothing of either shows until then. */
    int known = db_read(entry->id, &record) == 0;
    if (known && record.plugin[0]) {
        ask(ASK_PLUGIN, index, entry->plugin_off ? T_PLUGIN_ON_ASK : T_PLUGIN_OFF_ASK,
            entry->plugin_off ? T_PLUGIN_ON_LINE : T_PLUGIN_OFF_LINE);
        return;
    }
    if (!known || !record.dir[0]) {
        error_show(T_NO_RECORD, "launch record %s", entry->id);
        return;
    }
    snprintf(path, sizeof(path), "%s/PSP/GAME/%s/EBOOT.PBP", record.device, record.dir);
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        error_show(T_NO_EBOOT, "launch open %08x %s", (unsigned)fd, path);
        logline("launch: %s is not there", path);
        return;
    }
    sceIoClose(fd);

    cues_post(CUE_OPEN, 0);
    logline("launching %s", path);
    entropy_save(entropy_screen_is_replay());
    /* LoadExec replaces this process without running the HOME exit callback.
       Persist a changed graphics mode here as well, beside the other session
       state that must survive a normal app launch. */
    options_settings_save();
    audio_stop();
    log_dump();

    /* The firmware lets only a game from a disc start another program:
       sceKernelLoadExec refuses an app that was started from the Memory
       Stick or the Go's own storage (80020149). The custom firmware's own
       call is what starts a homebrew from either, as the XMB does: by the
       device the app is on. */
    struct SceKernelLoadExecVSHParam vsh;
    memset(&vsh, 0, sizeof(vsh));
    vsh.size = sizeof(vsh);
    vsh.args = strlen(path) + 1;
    vsh.argp = path;
    vsh.key = "game";
    int rc = sctrlKernelLoadExecVSHWithApitype(strncmp(path, "ef0:", 4) ? PSP_INIT_APITYPE_MS2 : PSP_INIT_APITYPE_EF2,
                                               path, &vsh);
    logline("launch: the custom firmware refused %08x", rc);

    /* Without that call (an emulator), the firmware's. */
    struct SceKernelLoadExecParam param;
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.args = strlen(path) + 1;
    param.argp = path;
    param.key = "game";
    int plain = sceKernelLoadExec(path, &param);
    /* Only reached when both refused it. */
    logline("launch: refused %08x", plain);
    char said[64];
    snprintf(said, sizeof(said), T_START_REFUSED, (unsigned)rc);
    error_show(said, "launch %08x %08x", (unsigned)rc, (unsigned)plain);
}

/* The action row taken: everything the tab holds, one after another, in the
   order it is listed. The rows are read into a list before the first fetch --
   an install moves the entry's state, and on the updates tab that takes the
   row out from under a loop still walking the view. A failure is said and the
   rest still go: one package the server has lost is not a reason to leave the
   others unfetched. The basket keeps what did not arrive and lets go of what
   did. */
void install_all(void) {
    static int list[MAX_APPS];
    int n = 0;
    for (int row = 0; row < view_count() && n < MAX_APPS; row++) {
        int at = view_index(row);
        if (at < 0) continue;
        struct download_status download;
        if (downloads_status(at, &download)) continue;
        const struct app_entry *entry = &g_catalog->apps[at];
        if (!entry->has_release || !entry->release.size || entry->unsupported) continue;
        /* On the stick the job is the updates alone. */
        if (view_tab_kind() == VIEW_TAB_STICK && entry->state != APP_UPDATE) continue;
        list[n] = at;
        n++;
    }
    /* Replace PSPDX last, after the other queued packages. */
    for (int i = 0; i < n - 1; i++) {
        if (strcmp(g_catalog->apps[list[i]].id, PSPDX_SELF_ID)) continue;
        int self = list[i];
        memmove(list + i, list + i + 1, (n - i - 1) * sizeof(*list));
        list[n - 1] = self;
        break;
    }
    int queued = 0;
    for (int i = 0; i < n; i++) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CIRCLE) break;
        int rc = downloads_enqueue(list[i]);
        if (rc == INSTALL_CANCELLED) break;
        if (rc == 0) {
            queued++;
        }
        dump_diagnostics();
    }
    if (queued) downloads_focus_queue();
    char message[96];
    snprintf(message, sizeof(message), "%d of %d added to Downloads", queued, n);
    logline("%s", message);
    shell_status(message);
    /* Acknowledge queueing; each job reports its own completion later. */
    cues_post(CUE_DONE, 0);
}

/* The unattended install for the rig: PSPDX.INSTALL on the stick names
   a repository, as a URL or as owner/repo, and it is asked at the origin
   like one typed on the gear tab. Returns its index in the catalog. */
int auto_install_index(void) {
    if (downloads_busy()) { error_busy(); return -1; }
    char text[SOURCE_URL], url[SOURCE_URL];
    int fd = sceIoOpen(storage_path("PSP/PSPDX/DEBUG/PSPDX.INSTALL"), PSP_O_RDONLY, 0777);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (n <= 0) return -1;
    text[n] = '\0';
    char *newline = strpbrk(text, "\r\n");
    if (newline) *newline = '\0';
    if (strncmp(text, "https://", 8) == 0) snprintf(url, sizeof(url), "%s", text);
    else snprintf(url, sizeof(url), "https://github.com/%.200s", text);
    /* The media thread shares the network stack; it is parked for the
       two requests as it is for the install that follows. */
    preview_quiesce();
    int index = catalog_add_repo(g_catalog, url, 1);
    preview_resume();
    if (index < 0) logline("PSPDX.INSTALL: nothing to install at %s", url);
    else view_rebuild(g_catalog);
    return index;
}

/* What the gear tab's two typing rows lead to: a source added and the
   catalog fetched again, and for "Install from GitHub" the one repository
   typed, found in the new catalog by its URL and put under the cursor
   with the install question already asked. */
static char g_wanted_url[SOURCE_URL];     /* that repository, while one is waited for */
static char g_wanted_name[64];            /* owner/repo, for the status line */

const char *wanted_url(void) { return g_wanted_url; }
void refused_line(int why, const char *name, char *out, size_t size) {
    if (why == REFUSED_PSPDX || why == REFUSED_NO_PSPDX)
        snprintf(out, size, T_WANT_NO_PSPDX, name);
    else if (why == REFUSED_REPO)
        snprintf(out, size, T_WANT_NO_REPO, name);
    else if (why == REFUSED_RELEASE)
        snprintf(out, size, T_WANT_NO_RELEASE, name);
    else if (why == REFUSED_NO_ANSWER)
        snprintf(out, size, T_WANT_NO_ANSWER, name);
    else if (why == REFUSED_FOLDER)
        catalog_folder_line(out, size, name, catalog_refused_folder());
    else
        snprintf(out, size, T_WANT_FAILED, name);
}
const char *wanted_name(void) { return g_wanted_name; }
void wanted_forget(void) { g_wanted_url[0] = '\0'; }

/* The catalog fetched again: the list gives way to the word and the
   status line and comes back with what is now published, the cursor on the
   package it was on if that package is still there. The sync thread and
   the media thread share the one HTTPS stack and the one asset buffer, so
   the media thread steps aside for the length of it. */
void refetch_now(int cursor, char *keep, size_t keep_size, int *synced,
                        int *refreshing) {
    if (!sync_done()) return;
    if (downloads_busy()) { error_busy(); return; }
    downloads_reset();
    int was = view_index(cursor);
    snprintf(keep, keep_size, "%s", was >= 0 ? g_catalog->apps[was].id : "");
    preview_quiesce();
    shell_word(T_WORD_CHECKING);
    sync_set_offline(!connect_online());
    if (sync_start(g_catalog) == 0) {
        *synced = 0;
        *refreshing = 1;
    } else {
        keep[0] = '\0';
        g_wanted_url[0] = '\0';
        preview_resume();
    }
}

/* The field drains and is swept again, in the room the browser was already
   standing in. The pool is not emptied for it, only the count: the sync
   and media threads keep running and a handshake in the middle of the
   sweep still has to draw from a full pool. A sweep takes half a minute
   of stick work and this one is two presses from the list, so it has to
   be possible to leave, and leaving puts the old count back. */
void sweep_again(void) {
    if (downloads_busy()) { error_busy(); return; }
    /* No connection outlives the seed it was made under: the kept ones
       are closed and the media thread parked, so nothing handshakes while
       the field is being swept. */
    preview_quiesce();
    https_close_idle();
    entropy_stash();
    entropy_init();
    if (entropy_screen_run() == 0) entropy_restore();
    entropy_save(entropy_screen_is_replay());
    preview_resume();
}

/* The cache goes and its folders come back empty: catalogs and pictures
   are fetched again the next time they are wanted. An install that never
   finished is cache too -- a download nobody will resume -- and goes with
   it, which is also what unblocks installs after a recovery that could not
   settle. Nothing else moves. The media thread is parked meanwhile, so no
   fetch lands in a folder that is being emptied. */
void clear_cache(void) {
    if (downloads_busy()) { error_busy(); return; }
    preview_quiesce();
    int bad = storage_remove_tree(storage_path("PSP/PSPDX/CACHE/catalogs")) < 0;
    bad |= storage_remove_tree(storage_path("PSP/PSPDX/CACHE/media")) < 0;
    asset_forget();
    sceIoMkdir(storage_path("PSP/PSPDX/CACHE"), 0777);
    sceIoMkdir(storage_path("PSP/PSPDX/CACHE/catalogs"), 0777);
    sceIoMkdir(storage_path("PSP/PSPDX/CACHE/media"), 0777);
    char line[128];
    if (storage_exists(storage_path("PSP/PSPDX/TMP/transaction.json")) &&
        install_discard(line, sizeof(line)) < 0)
        bad = 1;
    preview_resume();
    if (bad) error_show(T_CACHE_CLEAR_FAILED, "cache clear");
    else shell_status(T_CACHE_CLEARED);
    files_view_refresh();
}

/* Back to the first start: everything this client keeps on the stick goes,
   and the client with it, so that what comes up next is a first start --
   the sweep, the built-in list, nothing installed as far as it knows. The
   apps under PSP/GAME are not its to remove. */
void reset_completely(void) {
    if (downloads_busy()) { error_busy(); return; }
    log_dump();
    if (storage_remove_tree(storage_path("PSP/PSPDX")) < 0)
        logline("reset: some of PSP/PSPDX would not go");
    sceKernelExitGame();
}

/* Reads a source from the keyboard and adds it. Returns 1 when the catalog
   should be fetched again, 0 when there is nothing new. */
int type_source(int install) {
    if (downloads_busy()) { error_busy(); return 0; }
    char text[SOURCE_URL], url[SOURCE_URL];
    int rc = osk_read(install ? T_OSK_GITHUB : T_OSK_SOURCE, "", text, sizeof(text));
    if (rc <= 0 || !text[0]) return 0;
    rc = sources_normalize(text,url,sizeof(url));
    struct source_repo parsed;
    if(rc<0 || (install && !sources_parse_repo(url,&parsed))) {error_show(T_BAD_ADDRESS, "source address");return 0;}
    preview_quiesce();
    if (!connect_online()) {
        preview_resume();
        error_show(T_STATUS_OFFLINE, "source connect");
        return 0;
    }
    catalog_offline(0);
    rc=catalog_validate_source(url,install);
    preview_resume();
    if (rc < 0) {
        /* A repository says why: no release, GitHub not answering. */
        struct source_repo named;
        int why = install && sources_parse_repo(url, &named) ? catalog_refused(url) : 0;
        char line[96], name[64];
        if (why) {
            snprintf(name, sizeof(name), "%.24s/%.36s", named.owner, named.name);
            refused_line(why, name, line, sizeof(line));
        }
        error_show(why ? line : T_SOURCE_UNAVAILABLE, "source %d %.60s", why, url);
        return 0;
    }
    rc=sources_add(url,url,sizeof(url));
    if(rc<0){error_show(T_SOURCE_SAVE_FAILED, "sources.txt write %d", rc);return 0;}
    if (!install) {
        if (rc == 0) { shell_status(T_SOURCE_EXISTS); return 0; }
        return 1;
    }
    struct source_repo repo;
    if (!sources_parse_repo(url, &repo)) {
        error_show(T_GITHUB_FORMAT, "source format");
        return 0;
    }
    sources_repo_url(&repo, g_wanted_url, sizeof(g_wanted_url));
    snprintf(g_wanted_name, sizeof(g_wanted_name), "%.24s/%.36s", repo.owner, repo.name);
    return 1;
}

void install_inbox(void) {
    int count = inbox_count(), queued = 0;
    for (int i = 0; i < count; i++) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CIRCLE) break;
        int at = inbox_index(i);
        int rc = downloads_enqueue(at);
        if (rc == INSTALL_CANCELLED) break;
        if (rc == 0) queued++;
    }
    /* One app's own line -- installed, from the user's file, or why not --
       says more than "1 of 1". */
    if (count != 1) {
        char message[96];
        snprintf(message, sizeof(message), "%d of %d added to Downloads", queued, count);
        shell_status(message);
    }
}
