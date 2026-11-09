#include "text.h"
#include "util/storage.h"
#include "install/state.h"
#include "update/pspdx.h"
#include "util/self_manifest.h"
#include "update/presets.h"
#include "util/files.h"
/* PSPDX application controller. Feature code lives behind module APIs. */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio.h"
#include "audio/cues.h"
#include "gui/entropy_screen.h"
#include "gui/files_view.h"
#include "gui/gfx.h"
#include "gui/icons.h"
#include "gui/preview.h"
#include "gui/screen.h"
#include "gui/lattice.h"
#include "gui/osk.h"
#include "gui/netconf.h"
#include "session/cable.h"
#include "gui/shell.h"
#include "install/install.h"
#include "pspkit-https/entropy.h"
#include "pspkit-https/https.h"
#include "pspkit-tunnel/tunnel.h"
#include "network/bench.h"
#include "session/view.h"
#include "session/actions.h"
#include "session/downloads.h"
#include "session/launcher.h"
#include "session/library.h"
#include "session/questions.h"
#include "update/sources.h"
#include "session/options.h"
#include "update/catalog.h"
#include "update/sync.h"
#include "update/wifi.h"
#include "util/runtime.h"

PSP_MODULE_INFO("pspdx", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
/* One screenshot decodes into a megabyte of texture and wolfSSL wants its own
   working set; four megabytes no longer covers both. */
PSP_HEAP_SIZE_KB(12 * 1024);

static struct catalog catalog;
static unsigned g_worst_tick;       /* worst preview tick (fetch, decode) in us */

static void https_log(const char *text) { logline("%s", text); }

static int exit_callback(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    /* The one orderly moment in a run. Everything the session stirred into the
       pool has been sitting in RAM until here, so this is where it reaches the
       stick -- 32 bytes, once, instead of the same sector every few
       seconds. */
    downloads_shutdown();
    tunnel_stop();
    entropy_save(entropy_screen_is_replay());
    options_settings_save();
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int callback = sceKernelCreateCallback("Exit", exit_callback, NULL);
    if (callback >= 0) sceKernelRegisterExitCallback(callback);
    sceKernelSleepThreadCB();
    return 0;
}

static int setup_callbacks(void) {
    int thread = sceKernelCreateThread("exit_thread", callback_thread,
                                       0x11, 0xFA0, THREAD_ATTR_USER, 0);
    if (thread < 0) return thread;
    return sceKernelStartThread(thread, 0, 0);
}

/* ------------------------------------------------------------------- self */

/* PSPDX is an app in its own catalog, so the browser wants a record of it like
   any other package -- and no install ever wrote one: somebody copied this
   onto the stick. The first start writes it, out of where the loader started
   it from and what the build calls itself, and from then on the client is a
   row in its own list, installed and current rather than something to
   download over itself.

   The rev stays zero. A rev is the moment GitHub published a release and a
   build cannot know its own; catalog_check_updates reads the zero and settles
   it against the version string the first time it sees the catalog. */
static void record_self(const char *path) {
    if(storage_exists(storage_path("PSP/PSPDX/TMP/transaction.json")))return;
    struct installed self;
    if (db_read(PSPDX_SELF_ID, &self) == 0) {
        /* A record the catalog has not settled yet is provisional: a
           newer build started over it -- a desk, a stick moved between
           machines -- is what is installed now, and says so. Once the rev
           is set the catalog owns the comparison and this stays out. */
        if (self.rev != 0 || strcmp(self.version, PSPDX_VERSION) == 0) return;
        strncpy(self.version, PSPDX_VERSION, sizeof(self.version) - 1);
        self.version[sizeof(self.version) - 1] = '\0';
        if (db_write_record(&self) == 0)
            logline("self: record now says %s", self.version);
        return;
    }

    memset(&self, 0, sizeof(self));
    strncpy(self.id, PSPDX_SELF_ID, sizeof(self.id) - 1);
    snprintf(self.repo, sizeof(self.repo), "https://github.com/chriopter/pspdx-app");
    strncpy(self.version, PSPDX_VERSION, sizeof(self.version) - 1);
    /* Started from somewhere this cannot read -- a shell, a host debugger --
       leaves the name the release ships under, which is where it would be. */
    if (!path || !storage_game_dir(path, self.dir, sizeof(self.dir)))
        snprintf(self.dir, sizeof(self.dir), "PSPDX");
    if (db_write_record(&self) < 0) {
        logline("self: no record written; PSPDX will list as not installed");
        return;
    }
    logline("self: recorded PSPDX %s in PSP/GAME/%s", self.version, self.dir);
}

/* Scripted input for the test rig: PSPDX.KEYS on the stick holds lines
   of "<ms> <button>", pressed at that many milliseconds after the catalog
   arrived. How the browser gets driven hard without a hand on it. */
static struct { unsigned at; unsigned button; } g_keys[256];
static int g_key_count, g_key_next;
static unsigned g_keys_since;

/* Not a PSP button: a scripted "shot" takes a settled screenshot of what
   the earlier keys led to, into PSPDX1.BMP. */
#define KEY_SHOT 0x80000000u

static unsigned button_named(const char *name) {
    if (strcmp(name, "shot") == 0) return KEY_SHOT;
    if (strcmp(name, "up") == 0) return PSP_CTRL_UP;
    if (strcmp(name, "down") == 0) return PSP_CTRL_DOWN;
    if (strcmp(name, "left") == 0) return PSP_CTRL_LEFT;
    if (strcmp(name, "right") == 0) return PSP_CTRL_RIGHT;
    if (strcmp(name, "cross") == 0) return PSP_CTRL_CROSS;
    if (strcmp(name, "square") == 0) return PSP_CTRL_SQUARE;
    if (strcmp(name, "triangle") == 0) return PSP_CTRL_TRIANGLE;
    if (strcmp(name, "circle") == 0) return PSP_CTRL_CIRCLE;
    if (strcmp(name, "ltrigger") == 0) return PSP_CTRL_LTRIGGER;
    if (strcmp(name, "rtrigger") == 0) return PSP_CTRL_RTRIGGER;
    if (strcmp(name, "select") == 0) return PSP_CTRL_SELECT;
    if (strcmp(name, "start") == 0) return PSP_CTRL_START;
    return 0;
}

/* ------------------------------------------------------------------ files */

/* The catalog knows the names of apps the stick has no record of. */
static const char *files_name_of(const char *id) {
    for (int i = 0; i < catalog.count; i++)
        if (strcmp(catalog.apps[i].id, id) == 0) return catalog.apps[i].name;
    return NULL;
}

static int g_keys_loaded;
static void keys_load(void) {
    static char text[4096];
    /* Once a run: at the first sync, or at the start where none comes. */
    if (g_keys_loaded) return;
    g_keys_loaded = 1;
    g_keys_since = now_ms();
    int fd = sceIoOpen(storage_path("PSP/PSPDX/DEBUG/PSPDX.KEYS"), PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    int n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    text[n] = '\0';
    char *line = text;
    while (line && *line && g_key_count < 256) {
        char *end = strpbrk(line, "\r\n");
        if (end) *end++ = '\0';
        unsigned at = 0;
        char name[16] = "";
        if (sscanf(line, "%u %15s", &at, name) == 2) {
            g_keys[g_key_count].at = at;
            g_keys[g_key_count].button = button_named(name);
            g_key_count++;
        }
        line = end;
        while (line && (*line == '\r' || *line == '\n')) line++;
    }
    logline("keys: %d scripted", g_key_count);
}

/* The system sleeps after its idle minutes whatever is going on: a basket
   of fifty installs left to run, or a catalog still coming, would be cut off
   with the radio. While either is under way, and while a key is down --
   which a key injected by a host tool does not tell the system on its own --
   the console is kept awake. The screen may still dim. */
static void keep_awake(void) {
    static unsigned awake_ms;
    if (now_ms() - awake_ms < 1000) return;
    awake_ms = now_ms();
    scePowerTick(PSP_POWER_TICK_SUSPEND);
}

/* The battery as the system reads it, -1 while it cannot: a battery that
   stops answering -- being detected again, or gone -- is the line a sudden
   power-off leaves behind. */
static int battery_percent(void) {
    int v = scePowerGetBatteryLifePercent();
    return v < 0 ? -1 : v;
}
static int battery_mv(void) {
    int v = scePowerGetBatteryVolt();
    return v < 0 ? -1 : v;
}

/* Where in the main loop the last frame's time went, marked as it passes:
   said with a frame slow enough to be logged. */
static unsigned g_mark[5];
static unsigned span(unsigned from, unsigned to) {
    return (int)(to - from) > 0 ? (to - from) / 1000 : 0;
}

/* A direction held down scrolls: after a third of a second it repeats,
   slowly at first and then, as it is held, at a rate that gets through a
   long list -- twenty-five rows a second -- without ever skipping one. */
static unsigned repeat(unsigned held) {
    static unsigned was, since, fired;
    unsigned now = now_ms();
    if (held != was) { was = held; since = now; fired = 0; return 0; }
    if (!held || now - since < 500) return 0;
    unsigned along = now - since - 500;
    unsigned interval = along < 800 ? 110 : along < 2000 ? 65 : 40;
    if (now - fired < interval) return 0;
    fired = now;
    return held;
}

/* Up or down held: how many rows to move this frame. After a pause that
   leaves a single press a single row, the list picks up speed the longer
   it is held -- from ten rows a second to 480, so that ten thousand
   entries go by in under half a minute, and any row is still reached by
   letting go and tapping. Counted in time, not frames, so 30 and 60 FPS
   scroll alike. */
static int g_scroll_fast;               /* held past the single-row pace */
static int scroll_steps(unsigned held) {
    static unsigned was, since, last;
    static float owed;
    unsigned now = now_ms();
    if (held != was || !held) {
        was = held;
        since = last = now;
        owed = 0;
        g_scroll_fast = 0;
        return 0;
    }
    unsigned t = now - since, dt = now - last;
    g_scroll_fast = t >= 1200;
    last = now;
    float rate = t < 400 ? 0 : t < 1200 ? 10 : t < 2000 ? 20 : t < 3000 ? 60
               : t < 4500 ? 120 : t < 6000 ? 240 : 480;
    owed += rate * (dt < 100 ? dt : 100) / 1000.0f;
    int steps = (int)owed;
    owed -= steps;
    return steps;
}

/* The rig's film of the browser: with PSPDX.RECORD on the stick, every
   fourth frame from the moment the catalog is up goes to PSPDX_REC as a
   BMP, the way the sweep films itself. Fifteen a second, which is what a
   WebP of it is played at; nothing for a console, only for the desk. */
static int g_record;
static void record_start(void) {
    if (!storage_exists(storage_path("PSP/PSPDX/DEBUG/PSPDX.RECORD"))) return;
    sceIoMkdir(storage_path("PSP/PSPDX/DEBUG/PSPDX_REC"), 0777);
    g_record = 1;
    logline("record: every fourth frame to PSPDX_REC");
}
static void record_frame(void) {
    static unsigned frame;
    if (!g_record || frame++ % 4) return;
    char path[96];
    snprintf(path, sizeof(path), "%s/M%05u.BMP", storage_path("PSP/PSPDX/DEBUG/PSPDX_REC"), frame / 4);
    gfx_screenshot(path);
}

static unsigned keys_pressed(void) {
    unsigned pressed = 0;
    while (g_key_next < g_key_count && now_ms() - g_keys_since >= g_keys[g_key_next].at)
        pressed |= g_keys[g_key_next++].button;
    return pressed;
}

/* ------------------------------------------------------------------ typing */

/* One frame while the keyboard is up: the browser as it was, the keyboard
   drawn over it by the firmware. The pad is the keyboard's for the
   duration; the one scripted key still honoured is the rig's shot, so
   that the keyboard can be photographed. */
static void osk_draw(void *ctx) {
    int cursor = *(const int *)ctx;
    /* The keyboard's own threads draw over the frame and want the CPU:
       under it the shell draws a still room, no water, no stars, no
       backdrop, or the keyboard answers a key a second late. */
    shell_light_frame();
    shell_draw(shown(), cursor);
    if (keys_pressed() & KEY_SHOT) {
        gfx_screenshot(storage_path("PSP/PSPDX/DEBUG/PSPDX1.BMP"));
        logline("shot: PSPDX1.BMP with the keyboard up");
    }
}

/* The catalog is back: the repository typed is either in it, and the
   question is asked, or it is not, and the status line says why. */
static int wanted_settled(int *cursor) {
    char message[ENTRY_NAME_SIZE + 32];
    int found = catalog_find_repo(&catalog, wanted_url());
    if (found < 0) {
        refused_line(catalog_refused(wanted_url()), wanted_name(), message, sizeof(message));
        error_show(message, "want %d %.60s", catalog_refused(wanted_url()), wanted_url());
        return -1;
    }
    /* The tab that is open need not show it: Homebrew does, and is at most a
       ring of tabs away. */
    for (int n = view_tab_count(); n > 0 && view_row(found) < 0; n--)
        view_tab_move(1);
    *cursor = view_row(found);
    if (*cursor < 0) *cursor = 0;
    const struct app_entry *entry = &catalog.apps[found];
    if (entry->state == APP_NOT_INSTALLED || entry->state == APP_UPDATE) {
        downloads_enqueue(found);
    } else {
        snprintf(message, sizeof(message), T_WANT_CURRENT, entry->name);
        shell_status(message);
    }
    return found;
}

/* argv[0] is the path the firmware loaded this from -- the main thread's argp,
   which the loader fills with the EBOOT's own name. It is the only thing that
   says which directory under PSP/GAME the client is sitting in. */
/* On the page of an ISO that is one of several versions of its game: the
   next version, step one way or the other, round at the ends. 0 where the
   page is about anything else. */
static int version_step(int *index, int step) {
    int versions[VIEW_VERSIONS], n = view_versions(*index, versions, VIEW_VERSIONS);
    for (int i = 0; n > 1 && i < n; i++)
        if (versions[i] == *index) {
            *index = versions[(i + step + n) % n];
            return 1;
        }
    return 0;
}

/* The network, and the catalogs over it: how to connect asked where that is
   a question, the system's dialog, then the sync on its thread. At the
   start as ever; with the launcher on, only once a tab that lists what is
   published is opened -- the library and the gear need none of it, and a
   console started to start a game has no reason to wait for a radio. */
static void go_online(int midway) {
    int offline = storage_exists(storage_path("PSP/PSPDX/DEBUG/PSPDX.OFFLINE"));
    /* Mid-session the media thread is parked for the dialog, as every
       connection asked for later is. */
    if (midway) preview_quiesce();
    sync_set_profile(0, NULL);
    /* Before anything connects: how, where the release's copy of the
       cable's plugin makes that a question, and the cable set up if so. */
    if (!offline) offline = !cable_at_start();
    if (!offline) offline = netconf_connect() < 0;
    if (!offline) cable_connected();
    if (midway) preview_resume();
    shell_word(T_WORD_CONNECTING);
    sync_set_offline(offline);
    sync_start(&catalog);
}

/* Auto Connect, at the start: the connection chosen under Options joined
   on the sync thread, with no dialog, while the launcher is used; the foot
   says how far it got. Only where nothing would have to be asked first --
   the cable's question, or the gateway of a cable never connected -- and
   only while the connection is still in the system's settings. 1 when
   it has started. */
static int auto_connect(void) {
    char name[64];
    int profile = options_auto_connect(), cable = options_cable();
    if (!profile) return 0;
    if (wifi_profile_name(profile, name, sizeof(name)) < 0) {
        logline("wifi: auto connect: connection %d is not set up (first there is: %d)", profile, wifi_profile_next(0));
        return 0;
    }
    if (cable == CABLE_USB || cable_asks((enum cable)cable)) {
        logline("wifi: auto connect: not with cable choice %d", cable);
        return 0;
    }
    cable_start((enum cable)cable);
    if (cable_restart_needed()) return 0;
    shell_word(T_WORD_CONNECTING);
    sync_set_offline(storage_exists(storage_path("PSP/PSPDX/DEBUG/PSPDX.OFFLINE")));
    sync_set_profile(profile, name);
    sync_start(&catalog);
    return 1;
}

/* Connect, asked for: the system's dialog, then the sync again over the
   network. Not while the last sync still runs. */
static void connect_now(int *online_asked, int *synced) {
    if (!sync_done()) { shell_status(T_CONNECT_WAIT); return; }
    *online_asked = 1;
    *synced = 0;
    go_online(1);
}

/* Disconnect, asked for: the tunnel and every connection closed and the
   radio let go; what was fetched stays, and the Connect row comes back.
   Not while anything is fetching. */
static void disconnect_now(int *online_asked) {
    if (!sync_done() || downloads_busy()) { error_busy(); return; }
    preview_quiesce();
    tunnel_stop();
    https_net_disconnect();
    preview_resume();
    catalog_offline(1);
    *online_asked = 0;
    shell_status(T_DISCONNECTED);
}

/* Whether the open tab has rows to walk: the store's and the stick's are
   the catalog's, the library's and the gear's are there without one. */
static int browsable(void) {
    return shown()->count > 0 || view_tab_kind() == VIEW_TAB_LIBRARY || view_tab_kind() == VIEW_TAB_GEAR;
}

int main(int argc, char *argv[]) {
    /* How long the start takes to its first frame, step by step, for the
       log: the launcher is only worth having if that is short. */
    unsigned start_ms = now_ms(), shell_ms = 0, sweep_ms = 0, net_ms = 0;
    /* Full speed: the film decodes and the piano plays on the same CPU
       the interface draws with. The default is two thirds of it. */
    storage_init(argc > 0 ? argv[0] : NULL);
    options_settings_load();
    /* Back from the home menu the gear's XMB row left for: the launcher on
       again, before anything asks whether it is. */
    launcher_note_start(argc > 0 ? argv[0] : NULL);
    launcher_resume();
    state_load();
    /* A launcher's start: the stick is read on its own thread from here,
       while the fonts and the sound come up, so the library is there with
       the first frame. Started by ARK, that is -- not merely with ARK's
       launcher on, which it also is when PSPDX is opened from the home
       menu. */
    int launcher = launcher_started();
    /* Every start opens on the library, which is the stick's and needs no
       radio: it is read from here on, while the fonts and the sound come
       up. The network waits for a tab that lists what is published. */
    library_rescan();
    scePowerSetClockFrequency(333, 333, 166);
    if (setup_callbacks() < 0) {
        gui_init();
        pspDebugScreenPrintf("exit callback failed; HOME will not work\n");
    }

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    install_recover();
    questions_trust_load();
    sources_passwords_load();
    /* The tunnel: its keys beside the rest, the name the tailnet shows,
       and the hosts reached through it routed there. Nothing starts until
       a request to one of them. */
    tunnel_set_dir(storage_path("PSP/PSPDX"));
    tunnel_set_name("pspdx");
    tunnel_set_log(https_log);
    sources_tunneled_load();
    /* After the recovery, which is what settles what is actually under
       PSP/GAME, and before the sync, which reads every record there is. The
       record PSPDX 0.5 kept of itself goes first: it names this folder, and
       would list the client twice and hand its folder to a Delete. */
    install_retire_legacy();
    record_self(argc > 0 ? argv[0] : 0);
    char self_manifest_path[256];storage_app_path(PSPDX_SELF_ID,self_manifest_path,sizeof(self_manifest_path));
    if (!storage_exists(self_manifest_path) && state_ok() &&
        !storage_exists(storage_path("PSP/PSPDX/TMP/transaction.json"))) {
        char bundled_path[256];
        char *bundled = NULL;
        int n = -1;
        if (argc > 0 && argv[0]) {
            const char *slash = strrchr(argv[0], '/');
            if (slash && (size_t)(slash - argv[0]) + sizeof("/.pspdx") < sizeof(bundled_path)) {
                snprintf(bundled_path, sizeof(bundled_path), "%.*s/.pspdx",
                         (int)(slash - argv[0]), argv[0]);
                n = storage_read(bundled_path, &bundled, PSPDX_FILE_MAX);
            }
        }
        struct pspdx_file file;
        char why[80];
        if (n >= 0 && pspdx_parse(bundled, n, &file, why, sizeof(why)) == 0 &&
            !strcmp(file.source, "https://github.com/chriopter/pspdx-app"))
            storage_write(self_manifest_path, bundled, n);
        else
            storage_write(self_manifest_path, PSPDX_SELF_MANIFEST, strlen(PSPDX_SELF_MANIFEST));
        free(bundled);
    }
    /* The catalogs this release ships with, into sources.txt before the
       first fetch reads it: each once, so a source the user took out stays
       out and one a newer release brings still arrives. */
    presets_merge();
    https_set_log(https_log);
    https_set_user_agent("pspdx/0.0");
    /* A folder where the seed goes, or where its copy steps aside while it
       is written -- a stick put together by hand -- made every save of the
       seed fail, and every start sweep again. None of the three is anybody's
       folder, so it goes before the seed is read. */
    for (int i = 0; i < 3; i++) {
        static const char *const aside[] = {"", ".bak", ".new"};
        char path[256];
        snprintf(path, sizeof(path), "%s%s", storage_path("PSP/PSPDX/CRYPTO/seed.bin"), aside[i]);
        SceUID folder = sceIoDopen(path);
        if (folder >= 0) {
            sceIoDclose(folder);
            logline("seed: %s is a folder; %s", path,
                    storage_remove_tree(path) == 0 ? "removed" : "could not be removed");
        }
    }
    entropy_set_seed_file(storage_path("PSP/PSPDX/CRYPTO/seed.bin"));

    /* The test rig only. An emulator borrows the host's network, which is an
       order of magnitude past what a PSP-1004's 802.11b radio and its own TCP
       stack ever managed -- a download that takes half a minute on the hardware
       is over before the progress bar has moved. PSPDX.SLOW holds a rate in
       kilobytes a second, or nothing for the measured rate of a 1004. A PSP
       nobody has put that file on is paced by its radio, as it should be. The
       library clamps arriving bytes, so every fetch is shaped alike. */
    if (storage_exists(storage_path("PSP/PSPDX/DEBUG/PSPDX.SLOW"))) {
        char *text = NULL;
        int n = storage_read(storage_path("PSP/PSPDX/DEBUG/PSPDX.SLOW"), &text, 15);
        unsigned rate = n > 0 ? (unsigned)atoi(text) : 0;
        free(text);
        rate = rate ? rate : 180;
        https_set_rate_limit(rate);
        logline("network paced to %u KB/s, as a PSP-1004", rate);
    }
    entropy_init();
    entropy_screen_prepare();
    int sweep = entropy_screen_is_replay() || !entropy_load();

    /* The sweep is drawn on the water the browser then stands on, so the GE
       and the font come up before it rather than after. */
    if (!shell_init()) {
        /* No system font to browse with. The log says so, and the log is
           what gets shown. */
        gui_init();
        gui_clear();
        gui_failure();
        sceDisplayWaitVblankStart();
        gfx_screenshot(storage_path("PSP/PSPDX/DEBUG/PSPDX.BMP"));
        dump_diagnostics();
        for (;;) sceDisplayWaitVblankStart();
    }

    /* Boot the Media Engine before the sweep runs. Loading the AV modules
       after entropy_screen_run() has driven the GE wedges ATRAC3PLUS's
       module_start on its ME RPC (seen under PSPLink, and on the console once
       the sweep is not skipped); done here the ME comes up clean and
       audio_start() below just reuses it. */
    audio_load_modules();
    shell_ms = now_ms();

    if (sweep) {
        unsigned since = now_ms();
        int bits = entropy_screen_run();
        logline("entropy: %d bits swept in %u ms%s", bits, now_ms() - since,
                entropy_screen_is_replay() ? " (replay)" : "");
    }
    entropy_save(entropy_screen_is_replay());
    sweep_ms = now_ms();

    /* The tune starts with the shell and keeps going through installs and
       sweeps; it lives on its own thread and never waits for a frame. */
    audio_start();
    actions_init(&catalog);
    /* No connection at the start, the launcher's or any other: the
       system's dialog and the sync come with the first tab that lists what
       is published (or a gear row that needs them). */
    int net_started = 0;
    int online_asked = 0;               /* a connection asked for: Connect, or Auto Connect */
    net_ms = now_ms();
    /* The tabs there are before any catalog -- the store waiting, the
       library, the gear -- and with the launcher on, the library open. */
    view_rebuild(shown());
    int library_asked = 1;
    view_tab_open(TAB_LIBRARY);
    if (launcher) {
        /* The rig drives a launcher without waiting for a sync that does
           not come. */
        keys_load();
    }
    int first_frame = 1;

    /* Connect, fetch and check in the background while the first frames
       go up; the status line follows along. */
    int cursor = 0;
    int synced = 0;
    int rounds = 0;                     /* times the sync has come back */
    int refreshing = 0;                 /* SELECT, with the list already up */
    char keep[PSPDX_ID_SIZE] = "";                 /* the entry to come back to after one */
    int automatic = -1;
    int info = 0;                       /* the band of facts, over the list */
    int resting = 0;                    /* idle: the picture behind the shell */
    int details = 0;                    /* the page about one package */
    int details_of = -1;                /* which package the page is about */
    unsigned idle_since = now_ms();     /* the last time a key was down */
    unsigned last_frame_ms = now_ms();  /* to notice the loop having been away */
    unsigned shell_since = now_ms();
    /* The rig's screenshots, only when it asks with a PSPDX.SHOTS marker:
       each is a BMP written to the stick from this thread, and the one of
       the connecting screen held the first frames for a second and a half
       on every start. */
    int shots = storage_exists(storage_path("PSP/PSPDX/DEBUG/PSPDX.SHOTS"));
    int shot_connecting = !shots;
    unsigned dumped_ms = now_ms();
    unsigned last_buttons = 0;
    /* The newest pad sample already taken, by its timestamp, so that no
       sample counts twice, whichever loop read it. */
    unsigned pad_stamp = 0;
    int pad_seen = 0;
    /* Frame times, so a slow frame is a number and not a feeling: every
       ten seconds the average, worst, and misses against the selected cap. */
    unsigned frame_us = now_us(), frames = 0, worst = 0, late = 0, total = 0;
    /* Over-budget distributions: +0-3, +3-8, +8-18, and +18 ms. The base
       follows the selected 30/60 FPS mode below. */
    unsigned bucket[4] = { 0, 0, 0, 0 };
    osk_frame(osk_draw, &cursor);
    for (;;) {
        if ((net_started && !synced) || downloads_busy()) keep_awake();
        int download_modal = asking() || menu_shown() || popup_shown();
        if (synced && downloads_tick(cursor, download_modal)) {
            view_settled(&cursor);
            /* An install or a removal has changed what is on the stick. */
            if (library_asked) library_rescan();
        }
        /* A reading of the stick finished: the library's rows are the new
           ones, the cursor on the item it was on if it is still there. */
        {
            const struct library_item *was = view_tab_kind() == VIEW_TAB_LIBRARY
                                             ? library_item(library_place_item(cursor)) : 0;
            unsigned key = was ? was->key : 0;
            if (library_poll()) {
                view_library_count(library_places());
                if (view_tab_kind() == VIEW_TAB_LIBRARY) {
                    int at = was ? library_place_of(library_find(key)) : -1;
                    int places = library_places();
                    cursor = at >= 0 ? at : cursor < places ? cursor : places > 0 ? places - 1 : 0;
                    /* The first reading puts the cursor on the first game,
                       past the search's row and a section's sign. */
                    static int placed;
                    if (!placed && !was) {
                        placed = 1;
                        cursor = 0;
                        while (cursor < places - 1 && library_place_item(cursor) < 0) cursor++;
                    }
                }
            }
        }
        /* The unattended rig's second screenshot belongs after completion,
           not after the asynchronous enqueue call. */
        if (automatic >= 0 && !downloads_busy() && !download_modal) {
            screenshot_settled(cursor, storage_path("PSP/PSPDX/DEBUG/PSPDX2.BMP"));
            dump_diagnostics();
            automatic = -1;
        }
        if (downloads_focused() && !download_modal) {
            SceCtrlData focus_pad;
            if (sceCtrlReadBufferPositive(&focus_pad, 1) > 0) {
                unsigned focus_pressed = focus_pad.Buttons & ~last_buttons;
                last_buttons = focus_pad.Buttons;
                pad_stamp = focus_pad.TimeStamp;
                pad_seen = 1;
                /* The rig's scripted keys reach this screen too. */
                if (synced) focus_pressed |= keys_pressed() & ~KEY_SHOT;
                downloads_focus_frame(focus_pressed);
                if ((focus_pressed & PSP_CTRL_CIRCLE) && view_download_count()) {
                    view_remember(cursor, details ? details_of : -1, shell_details_position());
                    shell_details(NULL);
                    details = 0;
                    details_of = -1;
                    menu_details_forget();
                    for (int i = 0; i < view_tab_count() && view_tab_kind() != VIEW_TAB_BASKET; i++)
                        view_tab_move(1);
                    cursor = view_count() > 1 ? 1 : 0;
                }
            }
            frame_us = now_us();
            idle_since = now_ms();
            /* The log still reaches the stick while the downloads have the
               screen: a long batch is when there is most to read back. */
            if (expired(dumped_ms, 10000)) {
                dumped_ms = now_ms();
                logline("downloads: %u queued, battery %d%% %d mV, heap %u KB",
                        (unsigned)view_download_count(), battery_percent(), battery_mv(),
                        (unsigned)mallinfo().uordblks / 1024);
                log_dump_later();
            }
            continue;
        }
        unsigned now = now_us(), took = now - frame_us;
        unsigned began = frame_us;
        frame_us = now;
        frames++; total += took;
        if (took > worst) worst = took;
        if (took > 100000)
            /* And where it went: the telemetry, the fetch's arrival, the
               keys, the draw, the recording, and the top of this loop. */
            logline("frame %u: %u ms, %u ms since the shell came up; %u %u %u %u %u %u ms",
                    gfx_frames(), took / 1000, now_ms() - shell_since,
                    span(began, g_mark[0]), span(g_mark[0], g_mark[1]),
                    span(g_mark[1], g_mark[2]), span(g_mark[2], g_mark[3]),
                    span(g_mark[3], g_mark[4]), span(g_mark[4], now));
        /* A section a frame left early leaves its mark where the frame
           began, and counts nothing. */
        for (int i = 0; i < 5; i++) g_mark[i] = now;
        unsigned budget = gfx_fps_cap30() ? 34000u : 17000u;
        if (took > budget) late++;
        if (took > budget + 18000u) bucket[3]++;
        else if (took > budget + 8000u) bucket[2]++;
        else if (took > budget + 3000u) bucket[1]++;
        else if (took > budget) bucket[0]++;
        /* The emulator only flushes a file on close, and a long session
           should still leave a log behind: once every ten seconds. */
        if (expired(dumped_ms, 10000)) {
            dumped_ms = now_ms();
            char phases[120];
            unsigned audio_output, audio_frames, audio_late, audio_dry, audio_errors;
            int audio_error;
            audio_stats(&audio_output, &audio_frames, &audio_late, &audio_dry,
                        &audio_errors, &audio_error);
            shell_profile(phases, sizeof(phases));
            /* One stdout write instead of three. PSPLink's host stream can
               hold the main thread long enough to miss the very vblank this
               telemetry is measuring; batching keeps the 10-second sample
               from manufacturing a periodic hitch of its own. */
            logperf("perf %d: %u/10s avg %u us worst %u us late %u [%u %u %u %u] "
                    "miss %u; %s; tick %u us audio %u us out %u us %u Hz "
                    "late %u dry %u err %u/%08x atrac %u us free %u KB heap %u KB bat %d%% %d mV",
                    gfx_target_fps(), frames, frames ? total / frames : 0,
                    worst, late, bucket[0], bucket[1], bucket[2], bucket[3],
                    gfx_missed_presentations(), phases, g_worst_tick,
                    audio_worst_us(), audio_output,
                    audio_frames / 10, audio_late, audio_dry, audio_errors,
                    (unsigned)audio_error, audio_decode_worst_us(),
                    (unsigned)sceKernelTotalFreeMemSize() / 1024,
                    (unsigned)mallinfo().uordblks / 1024,
                    battery_percent(), battery_mv());
            g_worst_tick = 0;
            frames = worst = late = total = 0;
            bucket[0] = bucket[1] = bucket[2] = bucket[3] = 0;
            log_dump_later();
        }
        g_mark[0] = now_us();

        if (net_started && !synced) {
            shell_status(sync_message());
            /* The connecting screen, for the rig: a second and a half in,
               while there is still something to connect to. */
            if (!shot_connecting && expired(shell_since, 1500)) {
                shot_connecting = 1;
                gfx_screenshot(storage_path("PSP/PSPDX/DEBUG/PSPDX0.BMP"));
            }
            if (sync_done()) {
                synced = 1;
                /* A catalog is rebuilt every hour whether or not anything
                   changed, so a stamp a day old means the list has stopped
                   being looked after; what it says about updates is then
                   worth less than a look at the repositories themselves. */
                if (catalog.generated && catalog.generated + 24u * 3600u < wall_time()) {
                    unsigned days = (wall_time() - catalog.generated) / 86400u;
                    char stale[128];
                    snprintf(stale, sizeof(stale),
                             T_STALE,
                             catalog.generated_from, days, days == 1 ? "" : "s");
                    shell_status(stale);
                    logline("catalog: %s generated %u days ago", catalog.generated_from, days);
                }
                /* A fresh catalog is a fresh set of tabs, and the icons
                   cached against the old one no longer stand for the same
                   entries. The library's rows are not the catalog's: a
                   cursor there stays where it is. */
                if (view_tab_kind() != VIEW_TAB_LIBRARY) cursor = 0;
                view_rebuild(&catalog);
                if (keep[0]) {
                    /* Back to the package the cursor was on, if the catalog
                       still has it; the top of the list if it does not. */
                    for (int i = 0; i < catalog.count; i++)
                        if (strcmp(catalog.apps[i].id, keep) == 0) {
                            int row = view_row(i);
                            if (row >= 0) cursor = row;
                            break;
                        }
                    keep[0] = '\0';
                }
                if (refreshing) {
                    refreshing = 0;
                    icons_reset();
                    preview_resume();
                }
                /* An app left out because another has its folder would
                   otherwise just be missing: the first one is said. */
                if (sync_state() == SYNC_DONE) shell_status(catalog.collision);
                if (wanted_url()[0]) {
                    /* A fetch that failed altogether says so below; the
                       repository waited for is let go of either way. */
                    if (sync_state() == SYNC_DONE) wanted_settled(&cursor);
                    wanted_forget();
                }
                if (sync_state() != SYNC_DONE) {
                    /* Nothing came: say so, and offer the one thing that
                       can be done about it. */
                    char again[96];
                    snprintf(again, sizeof(again), T_STATUS_RETRY, sync_message());
                    shell_word(T_WORD_OFFLINE);
                    shell_status(again);
                }
                dump_diagnostics();
                /* The rig's hooks, once: a retry that comes through does
                   not get to install or replay the keys a second time. */
                if (rounds++ > 0) continue;
                /* The settled screenshot renders up to 360 frames waiting for
                   the room to stop moving, which is a ~6-8 s freeze the first
                   time the catalog lands -- fine for the test rig that wants
                   the BMP, a bad first impression for anyone else. So it is
                   taken only when the rig asks for it with a PSPDX.SHOTS
                   marker; a normal boot goes straight to a live browser. */
                if (shots)
                    screenshot_settled(cursor, storage_path("PSP/PSPDX/DEBUG/PSPDX.BMP"));
                dump_diagnostics();
                automatic = catalog.count > 0 ? auto_install_index() : -1;
                if (automatic >= 0) {
                    cursor = view_row(automatic);
                    if (cursor < 0) cursor = 0;
                    if (downloads_enqueue(automatic) < 0) automatic = -1;
                    dump_diagnostics();
                    view_settled(&cursor);
                }
                keys_load();
                /* Files names an app by the catalog's word for it, once
                   there is a catalog to ask. */
                files_names(files_name_of);
                record_start();
                int bench = sceIoOpen(storage_path("PSP/PSPDX/DEBUG/PSPDX.BENCH"), PSP_O_RDONLY, 0777);
                if (bench >= 0) {
                    sceIoClose(bench);
                    shell_status(T_STATUS_BENCH);
                    shell_draw(shown(), cursor);
                    preview_quiesce();
                    bench_run(catalog_url());
                    preview_resume();
                    shell_status("");
                    dump_diagnostics();
                }
            }
        }
        g_mark[1] = now_us();

        /* Every sample since the last frame, not only the newest: a tap that
           starts and ends inside one slow frame (a catalog parse, a PNG
           decode, 100 ms and more) still counts. The first frame only
           notes where the buffer stands, so a key held from the XMB is not
           taken for a press. */
        SceCtrlData pads[16];
        int read = sceCtrlReadBufferPositive(pads, 16);
        /* A question was answered since the last read: what the buffer
           holds from it was that answer, not a press for here. */
        if (input_take_resync()) pad_seen = 0;
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (read > 0) pad = pads[read - 1];
        unsigned tapped = 0;
        if (read > 0) {
            /* Keys already down at the first read were pressed before
               PSPDX was listening: down, not pressed. */
            if (!pad_seen) last_buttons = pad.Buttons;
            unsigned prev = last_buttons;
            for (int i = 0; pad_seen && i < read; i++)
                if ((int)(pads[i].TimeStamp - pad_stamp) > 0) {
                    tapped |= pads[i].Buttons & ~prev;
                    prev = pads[i].Buttons;
                }
            pad_stamp = pads[read - 1].TimeStamp;
            pad_seen = 1;
        }
        /* A read that fails is logged once per failure, not once per frame:
           the one thing a report of the console not answering its keys is
           checked against. */
        static int read_seen = 1;
        int read_state = read > 0 ? 1 : read;   /* how many samples is no news */
        if (read_state != read_seen) {
            logline("pad: read %d buttons %08x", read, pad.Buttons);
            read_seen = read_state;
        }
        /* The stick is a hand in the water, whenever it is off centre. */
        lattice_stir((pad.Lx - 128) / 127.0f, (pad.Ly - 128) / 127.0f);
        /* In the band about one package the same hand scrolls what it has
           to say, and the water goes on following it underneath. */
        if (details)
            shell_details_scroll((pad.Ly - 128) / 127.0f);
        unsigned pressed = (pad.Buttons & ~last_buttons) | tapped;
        last_buttons = pad.Buttons;
        if (pad.Buttons) keep_awake();
        shell_hold((pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN)) != 0);
        pressed |= repeat(pad.Buttons & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT |
                                         PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER));
        /* Up and down held move by as many rows as the hold has earned;
           to everything else that reads keys it is one press. */
        unsigned vertical = pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN);
        int steps = vertical == PSP_CTRL_UP || vertical == PSP_CTRL_DOWN ? scroll_steps(vertical)
                                                                          : scroll_steps(0);
        if (steps) pressed |= vertical;
        shell_scroll_fast(g_scroll_fast);
        pressed |= keys_pressed();          /* none before they are loaded */
        /* SELECT turns the store and the UMDs between their icons and their
           list -- the list is where they are searched and browsed by --
           and over the updates turns their order, newest first, by name or
           what is installed first; the foot says which. The cursor goes to
           the head, where the new view starts. */
        int select_here = (pressed & PSP_CTRL_SELECT) && !info && shown()->count > 0 &&
                          !view_cloud() && !asking() && !menu_shown() && !popup_shown() && !details &&
                          !gear_view_shown();
        if (select_here && (view_tab_kind() == VIEW_TAB_HOMEBREW || view_tab_kind() == VIEW_TAB_UMD)) {
            view_grid_toggle();
            cues_post(CUE_MOVE, cursor = 0);
            pressed &= ~(unsigned)PSP_CTRL_SELECT;
        } else if (select_here && view_tab_kind() == VIEW_TAB_STICK) {
            options_sort_next();
            if (view_index(cursor) >= 0) cursor = view_home();
            cues_post(CUE_MOVE, cursor);
            pressed &= ~(unsigned)PSP_CTRL_SELECT;
        }
        /* Everything below counts in rows of the shell's view -- the
           catalog filtered to the active tab -- and there are none of those
           while the catalog is being fetched, but for the library's and the
           gear's, which are not the catalog's. */
        int count = browsable() ? view_count() : 0;
        int on_library = view_tab_kind() == VIEW_TAB_LIBRARY;

        /* With something standing over the browser, the list stays where it
           is: a question that scrolls out from under its answer is a trap,
           up and down belong to the menu while one is open, and a tab
           changing under a band would change what the band is about. */
        int modal = asking() || menu_shown() || popup_shown() || details || gear_view_shown();
        int tab_departure_saved = 0;
        /* L/R leave nested pages for the adjacent tab. D-pad left/right
           keep their local meaning inside an app's details. */
        if ((pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) &&
            (details || gear_view_shown()) &&
            !asking() && !menu_shown() && !popup_shown() && shown()->count > 0) {
            view_remember(cursor, details ? details_of : -1, shell_details_position());
            tab_departure_saved = 1;
            if (details) {
                shell_details(NULL);
                details = 0;
                details_of = -1;
                menu_details_forget();
            }
            gear_views_close();
            info = 0;
            modal = 0;
        }
        /* The same hand over the browser scrolls what the card says about
           the package under the cursor. */
        if (!modal) shell_card_scroll((pad.Ly - 128) / 127.0f);

        /* Twenty-five seconds without a key and the picture of the package under
           the cursor rises behind the interface, which stays where it is
           and goes on working; the next key takes it down again. The stick
           stirs the water and does not count as a key.

           An install or a refetch holds the loop for as long as it takes,
           and that is not idling: the clock starts again when the loop is
           back, or a picture would come up over a wait nobody sat out. */
        unsigned frame_ms = now_ms();
        if (frame_ms - last_frame_ms > 300) idle_since = frame_ms;
        last_frame_ms = frame_ms;
        if (pad.Buttons || pressed || count == 0) idle_since = now_ms();
        if (!modal && !info && now_ms() - idle_since > 25000) {
            if (!resting) shell_rest(resting = 1);
        } else if (resting) {
            shell_rest(resting = 0);
        }

        /* Left alone a few seconds, online and with nothing downloading,
           PSPDX fetches the icon of every app it has not kept yet, so that
           lists scrolled later show theirs at once; the foot says so, and
           any key stops it at the next icon. Options can turn it off. */
        static char fill_text[96];
        int fill = synced && sync_state() == SYNC_DONE && options_fill_cache() && !modal && !info &&
                   !downloads_busy() && now_ms() - idle_since > 4000 && icons_fill_progress() >= 0;
        icons_fill(fill);
        if (fill) {
            keep_awake();
            if (shell_status_is("") || (fill_text[0] && shell_status_is(fill_text))) {
                snprintf(fill_text, sizeof(fill_text), T_FILLING, icons_fill_progress());
                shell_status(fill_text);
            }
        } else if (fill_text[0]) {
            if (shell_status_is(fill_text)) shell_status("");
            fill_text[0] = '\0';
        }

        /* Each tab remembers its category, selected app and detail scroll.
           Triggers leave nested pages; D-pad arrows switch tabs in lists. */
        unsigned tabs = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
        /* In the tag cloud the arrows walk the chips; L and R still turn
           the tab. */
        int cloud = shown()->count > 0 && view_cloud() && !modal;
        if (cloud) tabs &= ~(unsigned)(PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
        /* In the UMDs' grid they go a disc along, and turn the tab only
           off the line's ends, the way they turn it from a list. */
        if (shown()->count > 0 && view_grid() && !modal && count > 0) {
            int col = cursor % shell_grid_cols();
            if (col > 0) tabs &= ~(unsigned)PSP_CTRL_LEFT;
            if (col < shell_grid_cols() - 1 && cursor < count - 1) tabs &= ~(unsigned)PSP_CTRL_RIGHT;
        }
        /* The library's grid the same, a section's line at a time. */
        if (on_library && !modal && count > 0) {
            if (library_step(cursor, -1, 0, 0) != cursor) tabs &= ~(unsigned)PSP_CTRL_LEFT;
            if (library_step(cursor, 1, 0, 0) != cursor) tabs &= ~(unsigned)PSP_CTRL_RIGHT;
        }
        if ((pressed & tabs) && view_tab_count() > 1 && !modal) {
            if (!tab_departure_saved)
                view_remember(cursor, details ? details_of : -1, shell_details_position());
            view_tab_move(pressed & (PSP_CTRL_RTRIGGER | PSP_CTRL_RIGHT) ? 1 : -1);
            float detail_scroll;
            view_recall(&cursor, &details_of, &detail_scroll);
            details = details_of >= 0;
            if (details) {
                shell_details(&catalog.apps[details_of]);
                shell_details_restore_position(detail_scroll);
            }
            modal = details;
            pressed &= ~tabs;
            cues_post(CUE_MOVE, cursor);
            on_library = view_tab_kind() == VIEW_TAB_LIBRARY;
            count = browsable() ? view_count() : 0;
        }
        /* The library read at its first look, where the start did not. */
        if (on_library && !library_asked) {
            library_asked = 1;
            library_rescan();
        }
        /* The start made no connection. Once the first frame is up, Auto
           Connect makes one in the background; without it, what was kept
           from the last time is read, and the store's first row connects
           -- the system's dialog comes when asked for, not with a tab. */
        if (!net_started && !first_frame) {
            net_started = 1;
            synced = 0;
            if (auto_connect()) online_asked = 1;
            else {
                shell_word(T_WORD_OFFLINE);
                sync_set_profile(0, NULL);
                sync_set_offline(1);
                sync_start(&catalog);
            }
        }
        /* The Connect row: there until a connection is asked for, and again
           when one did not come. The cursor stays on the row it was on. */
        {
            int want = synced && (!online_asked || !sync_online());
            if (want != view_connect_shown()) {
                int on_root = view_index(cursor) < 0;
                view_set_connect(want);
                if (view_tab_kind() == VIEW_TAB_HOMEBREW && !(on_root && cursor == 0))
                    cursor += want ? 1 : -1;
                if (cursor < 0) cursor = 0;
                count = browsable() ? view_count() : 0;
            }
        }
        shell_link(!online_asked ? 0 : !synced ? 1 : sync_online() ? 2 : 0);
        int unconnected = !online_asked && !on_library && view_tab_kind() != VIEW_TAB_GEAR;
        shell_unconnected(unconnected);
        if (unconnected && !modal && (pressed & PSP_CTRL_CROSS) && catalog.count <= 0) {
            pressed &= ~(unsigned)PSP_CTRL_CROSS;
            connect_now(&online_asked, &synced);
        }
        modal = modal || info;
        /* The list is a ring for a press: past the last entry comes the
           first. A hold stops at the end instead -- at hundreds of rows a
           second the ring would be gone round before the eye saw it. */
        /* The cloud: a line up or down onto the nearest chip, one chip
           along; a hold repeats a step at a time. */
        if (view_cloud() && count > 0 && !modal && shown()->count > 0) {
            int to = cursor;
            if (pressed & PSP_CTRL_DOWN) to = shell_cloud_step(to, 0, 1);
            if (pressed & PSP_CTRL_UP) to = shell_cloud_step(to, 0, -1);
            if (pressed & PSP_CTRL_RIGHT) to = shell_cloud_step(to, 1, 0);
            if (pressed & PSP_CTRL_LEFT) to = shell_cloud_step(to, -1, 0);
            if (to != cursor) cues_post(CUE_MOVE, cursor = to);
        } else if (view_grid() && count > 0 && !modal && shown()->count > 0) {
            /* The grid: a line up or down, round at the ends for a press;
               a disc along. A hold stops at the edges. */
            int to = cursor;
            if (pressed & PSP_CTRL_DOWN) to = shell_grid_step(to, 0, 1, !steps);
            if (pressed & PSP_CTRL_UP) to = shell_grid_step(to, 0, -1, !steps);
            if (pressed & PSP_CTRL_RIGHT) to = shell_grid_step(to, 1, 0, 0);
            if (pressed & PSP_CTRL_LEFT) to = shell_grid_step(to, -1, 0, 0);
            if (to != cursor) cues_post(CUE_MOVE, cursor = to);
        } else if (on_library && count > 0 && !modal) {
            /* The library's grid: the same, its sections' lines one after
               another. */
            int to = cursor;
            if (pressed & PSP_CTRL_DOWN) to = library_step(to, 0, 1, !steps);
            if (pressed & PSP_CTRL_UP) to = library_step(to, 0, -1, !steps);
            if (pressed & PSP_CTRL_RIGHT) to = library_step(to, 1, 0, 0);
            if (pressed & PSP_CTRL_LEFT) to = library_step(to, -1, 0, 0);
            if (to != cursor) cues_post(CUE_MOVE, cursor = to);
        } else if ((pressed & PSP_CTRL_DOWN) && count > 0 && !modal) {
            int to = steps ? (cursor + steps < count ? cursor + steps : count - 1)
                           : (cursor + 1) % count;
            if (to != cursor) cues_post(CUE_MOVE, cursor = to);
        }
        if ((pressed & PSP_CTRL_UP) && count > 0 && !modal && !view_cloud() && !view_grid() && !on_library) {
            int to = steps ? (cursor - steps > 0 ? cursor - steps : 0) : (cursor + count - 1) % count;
            if (to != cursor) cues_post(CUE_MOVE, cursor = to);
        }
        if (pressed & KEY_SHOT) {
            screenshot_settled(cursor, storage_path("PSP/PSPDX/DEBUG/PSPDX1.BMP"));
            logline("shot: PSPDX1.BMP at cursor %d", cursor);
        }

        if (questions_handle(pressed, &cursor, &count, keep, sizeof(keep), &synced, &refreshing)) {
            /* A question stood: the keys were its. */
        } else if (files_view_shown() && !popup_shown()) {
            /* The one action among the areas: the cache asked about, then
               cleared by the answer; the rest of the keys are the view's. */
            if ((pressed & PSP_CTRL_CROSS) && files_view_action())
                ask(ASK_DISCARD, -1, T_DISCARD_ASK, T_DISCARD_LINE);
            else files_view_keys(pressed, &pad);
        } else if (options_handle(pressed, &cursor, &count, keep, sizeof(keep), &synced, &refreshing, &details)) {
            /* The options or a popup stood: the same. Information taken
               there opens the page about the row the options were about. */
            if (details && menu_details_index() >= 0) details_of = menu_details_index();
        } else if (info) {
            /* The band says what the session is and does nothing else. */
            if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS)) shell_info(info = 0);
        } else if (details) {
            if (pressed & PSP_CTRL_CIRCLE) {
                shell_details(0);
                details = 0;
                /* The card goes back to the tile's version, if another was
                   looked at. */
                shell_reshow_card();
                /* Information was read out of the options, so closing it
                   goes back there, on the row it was opened from. */
                menu_return();
            } else if ((pressed & PSP_CTRL_SQUARE) && details_of >= 0 &&
                       (catalog.apps[details_of].state == APP_NOT_INSTALLED ||
                        view_basket_has(details_of))) {
                /* Square sets aside what the page is about: on an ISO's
                   page the version its flags have chosen. */
                view_basket_toggle(details_of);
                cues_post(CUE_MOVE, cursor);
                count = view_count();
            } else if ((pressed & PSP_CTRL_CROSS) && details_of >= 0) {
                /* On the page X is the one thing to do to the package:
                   have it, have the newer one, or start it, each asked
                   about first. */
                const struct app_entry *e = &catalog.apps[details_of];
                if (downloads_active(details_of)) downloads_focus(details_of);
                else if (e->state == APP_NOT_INSTALLED || e->state == APP_UPDATE)
                    downloads_enqueue(details_of);
                else if (!strcmp(e->type, "plugin")) launch_app(details_of);
                else {
                    char title[ENTRY_NAME_SIZE + 32];
                    snprintf(title, sizeof(title), T_RUN_ASK, e->name);
                    ask(ASK_RUN, details_of, title, T_RUN_LINE);
                }
            } else if ((pressed & PSP_CTRL_TRIANGLE) && details_of >= 0) {
                menu_open(details_of);
            } else if (pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
                /* The pad reads the page too, a line at a time, so a hand
                   that never touches the stick is not left with a page
                   that seems not to answer. */
                shell_details_step(pressed & PSP_CTRL_DOWN ? 1 : -1);
            } else if ((pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) && details_of >= 0 &&
                       version_step(&details_of, pressed & PSP_CTRL_RIGHT ? 1 : -1)) {
                /* A game of several versions turns through its versions,
                   the page and the card following the one chosen. */
                shell_details(&catalog.apps[details_of]);
                preview_show(&catalog.apps[details_of], 0);
                cues_post(CUE_MOVE, cursor);
            } else if ((pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) && count > 0) {
                /* Left and right turn the page to the neighbouring package
                   in the list, skipping the rows that are not packages,
                   and the cursor under the page follows. */
                int step = pressed & PSP_CTRL_RIGHT ? 1 : -1;
                for (int tries = 0; tries < count; tries++) {
                    cursor = (cursor + step + count) % count;
                    int at = view_index(cursor);
                    if (at >= 0) {
                        shell_details(&catalog.apps[at]);
                        details_of = at;
                        cues_post(CUE_MOVE, cursor);
                        break;
                    }
                }
            }
        } else if ((pressed & PSP_CTRL_CIRCLE) && view_network_shown()) {
            /* O out of Network, onto its row. */
            cues_post(CUE_MOVE, cursor = view_network_close());
            count = view_count();
        } else if ((pressed & PSP_CTRL_CIRCLE) && view_search()) {
            /* O out of what a search found: back to the store's first
               rows, on the search's -- in the UMDs' grid, its tile. */
            cues_post(CUE_MOVE, cursor = view_search_close());
            count = view_count();
        } else if ((pressed & PSP_CTRL_CIRCLE) &&
                   (view_group_open_at() >= 0 || view_browse_at() >= 0)) {
            /* O goes back a step, onto the row the step was taken from --
               from an empty category too, which has no row to stand on:
               from a category, tag or source to the way's rows, from those
               to the store's first rows. */
            int was = view_group_open_at() >= 0 ? view_group_close() : view_browse_close();
            cues_post(CUE_MOVE, cursor = was);
            count = view_count();
        } else if (on_library && count > 0) {
            /* X or START starts it at once -- but not with a key still
               down from before the launcher came up -- and SQUARE marks it
               a favourite, first in the list, the cursor going with it. */
            const struct library_item *item = library_item(library_place_item(cursor));
            if (pressed & PSP_CTRL_SELECT) {
                /* SELECT: icons or the list, the cursor staying on its game. */
                const struct library_item *was = library_item(library_place_item(cursor));
                unsigned key = was ? was->key : 0;
                library_list_toggle();
                int at = was ? library_place_of(library_find(key)) : -1;
                cursor = at >= 0 ? at : 0;
                view_library_count(library_places());
                count = library_places();
                cues_post(CUE_MOVE, cursor);
            } else if ((pressed & PSP_CTRL_CROSS) && library_list() &&
                       (library_place_sign(cursor) >= 0 || library_place_favourites(cursor))) {
                /* A row of the list: its games in the list's place. */
                library_list_open(library_place_favourites(cursor) ? -2 : library_place_sign(cursor));
                cursor = 0;
                view_library_count(count = library_places());
                cues_post(CUE_MOVE, cursor);
            } else if ((pressed & PSP_CTRL_CIRCLE) && library_list_opened() && !library_searched()) {
                /* O back to the list's rows, on the one it opened. */
                cursor = library_list_close();
                view_library_count(count = library_places());
                cues_post(CUE_MOVE, cursor);
            } else if ((pressed & PSP_CTRL_CROSS) && library_place_search(cursor)) {
                /* The list's search row: a word typed, and only the games with
                   it in their title stay. With none, the foot says so. */
                char word[LIBRARY_TITLE];
                if (osk_read(T_OSK_SEARCH_GAMES, library_search_last(), word, sizeof(word)) > 0 && word[0]) {
                    if (library_search(word) > 0) {
                        /* Onto the first game found. */
                        view_library_count(library_places());
                        cursor = 0;
                        while (cursor < library_places() - 1 && library_place_item(cursor) < 0) cursor++;
                        cues_post(CUE_MOVE, cursor);
                    } else {
                        char line[96];
                        snprintf(line, sizeof(line), T_SEARCH_NONE_GAMES, word);
                        shell_status(line);
                    }
                }
            } else if ((pressed & PSP_CTRL_CIRCLE) && library_searched()) {
                /* O out of what the search found: everything again, on
                   the search's row. */
                library_search_close();
                view_library_count(library_places());
                cues_post(CUE_MOVE, cursor = 0);
            } else if ((pressed & (PSP_CTRL_CROSS | PSP_CTRL_START)) && item && expired(start_ms, 1000)) {
                launch_local(item->path, item->kind);
            } else if ((pressed & PSP_CTRL_TRIANGLE) && item) {
                /* TRIANGLE: Start, Favorite and Delete, in the popup. */
                library_menu_open(item->key);
            } else if ((pressed & PSP_CTRL_SQUARE) && item) {
                unsigned key = item->key;
                library_favourite_toggle(key);
                shell_status(library_favourite(key) ? T_FAVOURITE_ON : T_FAVOURITE_OFF);
                cues_post(CUE_MOVE, cursor = library_place_of(library_find(key)));
            }
        } else if (count > 0) {
            int at = view_index(cursor);
            if ((pressed & PSP_CTRL_CROSS) && at == VIEW_ROW_SEARCH) {
                /* The search: a word typed, and the store narrowed to the
                   packages it is found in. With none, the foot says so and
                   the store stays. */
                char word[VIEW_SEARCH_SIZE];
                int umds = view_umd_open();
                if (osk_read(umds ? T_OSK_SEARCH_UMD : T_OSK_SEARCH, view_search_last(), word, sizeof(word)) > 0 &&
                    word[0]) {
                    if (view_search_open(word) > 0) {
                        cues_post(CUE_MOVE, cursor = view_home());
                        count = view_count();
                    } else {
                        char line[96];
                        if (umds) snprintf(line, sizeof(line), T_SEARCH_NONE_UMD, word);
                        else snprintf(line, sizeof(line), T_SEARCH_NONE, word);
                        shell_status(line);
                    }
                }
            } else if ((pressed & PSP_CTRL_CROSS) && at == VIEW_ROW_CONNECT) {
                connect_now(&online_asked, &synced);
            } else if ((pressed & PSP_CTRL_CROSS) && VIEW_IS_BROWSE(at)) {
                /* A way to browse by lists its rows in the store's place. */
                view_browse_open(VIEW_ROW_BROWSE - at);
                cues_post(CUE_MOVE, cursor = 0);
                count = view_count();
            } else if ((pressed & PSP_CTRL_CROSS) && at <= VIEW_ROW_GROUP && at > VIEW_ROW_SETTING) {
                /* A category, tag or source row narrows the store to what
                   stands in it; O goes back to the way's rows, on the row
                   it was narrowed from. */
                view_group_open(VIEW_ROW_GROUP - at);
                cues_post(CUE_MOVE, cursor = view_home());
                count = view_count();
            } else if ((pressed & PSP_CTRL_CROSS) && at <= VIEW_ROW_SETTING) {
                /* A row under the gear does what it says: XMB leaves for the
                   home menu, the UI's mode a switch, Sources, Options
                   and Data are views in the list's place, About the band.
                   Sources waits for a sync, since what it lists is what
                   the last fetch read. */
                int which = VIEW_ROW_SETTING - at;
                /* Sources, USBNet and Tailscale need the connection the
                   start did not make: asked for here, they make it, and
                   are there to be taken once the sync is through. */
                if (!online_asked && synced && (which == SET_CABLE || which == SET_TUNNEL))
                    connect_now(&online_asked, &synced);
                if (which == SET_XMB) leave_to_xmb();
                else if (which == SET_NETWORK) {
                    /* Network is a level of its own; O comes back out. */
                    view_network_open();
                    cues_post(CUE_MOVE, cursor = 0);
                    count = view_count();
                } else if (which == SET_CONNECTION) {
                    /* Connected, X disconnects; otherwise it connects, the
                       system's dialog choosing the network. */
                    if (online_asked && synced && sync_online()) disconnect_now(&online_asked);
                    else if (!online_asked || synced) connect_now(&online_asked, &synced);
                } else if (which == SET_AUTO) {
                    /* Which connection, or none: the sidebar asks. */
                    auto_connect_open();
                } else if (which == SET_UI) {
                    /* The UI's mode flips where it stands, and is kept. */
                    options_fps_toggle_saved();
                    cues_post(CUE_MOVE, cursor);
                } else if (which == SET_SOURCES && synced) sources_open();
                else if (which == SET_OPTIONS) system_open();
                else if (which == SET_CABLE && synced) {
                    /* How to connect, asked again as at the first start;
                       it ends in a new connection, so it waits for a sync
                       as Sources does. */
                    cable_set_up_again();
                    count = view_count();
                } else if (which == SET_TUNNEL && synced) {
                    if (tunnel_set_up()) refetch_now(cursor, keep, sizeof(keep), &synced, &refreshing);
                }
                else if (which == SET_FILES) files_view_open();
                else if (which == SET_ABOUT) shell_info(info = 1);
            } else if (pressed & PSP_CTRL_CROSS) {
                /* X opens the package's page; the job rows do their job.
                   The options, with the same things and the rest, are on
                   triangle. */
                if (at == VIEW_ROW_ACTION) {
                    struct view_plan plan;
                    view_action_plan(&plan);
                    if (plan.apps <= 0 && view_tab_kind() == VIEW_TAB_BASKET && view_download_count()) {
                        if (downloads_busy()) {
                            downloads_focus_queue();
                        } else {
                            downloads_clear_finished();
                            view_settled(&cursor);
                        }
                    } else if (plan.apps <= 0 && view_tab_kind() == VIEW_TAB_STICK) {
                        if (synced)
                            refetch_now(cursor, keep, sizeof(keep), &synced, &refreshing);
                    } else ask_all();
                }
                else if (at >= 0) {
                    /* A package opens as a page: the list goes off to the
                       left and the page comes in from the right, the way
                       the system's own screens follow one another. The
                       things to do to the package are on the page. */
                    shell_details(&catalog.apps[at]);
                    details = 1;
                    details_of = at;
                }
            }
            if ((pressed & PSP_CTRL_TRIANGLE) && at >= 0) menu_open(at);
            /* Square sets a package aside for later and takes it out
               again -- including from the basket tab, where the row the
               cursor is on is one that was set aside. The basket appearing
               or emptying is a tab appearing or going, so the tabs are
               worked out again before the next frame draws them. */
            /* Square on the stick's own row: the same check, but every app is
               asked at its own repository now, whatever the catalogs said and
               however recently it was asked. */
            if ((pressed & PSP_CTRL_SQUARE) && at == VIEW_ROW_ACTION &&
                view_tab_kind() == VIEW_TAB_STICK && synced) {
                catalog_force_sources();
                refetch_now(cursor, keep, sizeof(keep), &synced, &refreshing);
            }
            /* A game of several versions on the UMD tab asks which. */
            int several[VIEW_VERSIONS];
            if ((pressed & PSP_CTRL_SQUARE) && at >= 0 && view_grid() &&
                view_versions(at, several, VIEW_VERSIONS) > 1)
                basket_pick_open(at);
            else if ((pressed & PSP_CTRL_SQUARE) && at >= 0 &&
                (catalog.apps[at].state == APP_NOT_INSTALLED || view_basket_has(at))) {
                view_basket_toggle(at);
                cues_post(CUE_MOVE, cursor);
                view_settled(&cursor);
                count = view_count();
            }
            if ((pressed & PSP_CTRL_START) && at >= 0 &&
                catalog.apps[at].state != APP_NOT_INSTALLED)
                launch_app(at);
        } else if ((pressed & PSP_CTRL_CROSS) && sync_state() == SYNC_FAILED) {
            /* Once more from the top: the wait comes back with its word,
               and the frames below carry on as they did the first time. */
            shell_word(T_WORD_CONNECTING);
            if (sync_start(&catalog) == 0) synced = 0;
        }

        unsigned tick0 = now_us();
        g_mark[2] = tick0;
        shell_shot_sync(shown(), cursor);
        audio_duck(preview_playing());
        unsigned tick = now_us() - tick0;
        if (tick > g_worst_tick) g_worst_tick = tick;
        shell_draw(shown(), cursor);
        if (first_frame) {
            first_frame = 0;
            logline("start: first frame at %u ms: settings to shell %u, sweep %u, network %u, to the frame %u%s",
                    now_ms() - start_ms, shell_ms - start_ms, sweep_ms - shell_ms, net_ms - sweep_ms,
                    now_ms() - net_ms, launcher ? " (launcher, no network)" : "");
        }
        g_mark[3] = now_us();
        record_frame();
        g_mark[4] = now_us();
    }
    return 0;
}
