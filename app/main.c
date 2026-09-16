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
#include "gui/shell.h"
#include "install/install.h"
#include "pspkit-https/entropy.h"
#include "pspkit-https/https.h"
#include "network/bench.h"
#include "session/view.h"
#include "session/actions.h"
#include "session/questions.h"
#include "session/options.h"
#include "update/catalog.h"
#include "update/sync.h"
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
    entropy_save(entropy_screen_is_replay());
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

static void keys_load(void) {
    static char text[4096];
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

/* A direction held down scrolls: after a third of a second it repeats,
   slowly at first and then, as it is held, at a rate that gets through a
   long list -- twenty-five rows a second -- without ever skipping one. */
static unsigned repeat(unsigned held) {
    static unsigned was, since, fired;
    unsigned now = now_ms();
    if (held != was) { was = held; since = now; fired = 0; return 0; }
    if (!held || now - since < 330) return 0;
    unsigned along = now - since - 330;
    unsigned interval = along < 800 ? 110 : along < 2000 ? 65 : 40;
    if (now - fired < interval) return 0;
    fired = now;
    return held;
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
    shell_draw(shown(), cursor);
    if (keys_pressed() & KEY_SHOT) {
        gfx_screenshot(storage_path("PSP/PSPDX/DEBUG/PSPDX1.BMP"));
        logline("shot: PSPDX1.BMP with the keyboard up");
    }
}

/* The catalog is back: the repository typed is either in it, and the
   question is asked, or it is not, and the status line says why. */
static int wanted_settled(int *cursor) {
    char message[96];
    int found = catalog_find_repo(&catalog, wanted_url());
    if (found < 0) {
        refused_line(catalog_refused(wanted_url()), wanted_name(), message, sizeof(message));
        shell_status(message);
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
        ask_install(found);
    } else {
        snprintf(message, sizeof(message), T_WANT_CURRENT, entry->name);
        shell_status(message);
    }
    return found;
}

/* argv[0] is the path the firmware loaded this from -- the main thread's argp,
   which the loader fills with the EBOOT's own name. It is the only thing that
   says which directory under PSP/GAME the client is sitting in. */
int main(int argc, char *argv[]) {
    /* Full speed: the film decodes and the piano plays on the same CPU
       the interface draws with. The default is two thirds of it. */
    storage_init(argc > 0 ? argv[0] : NULL);
    state_load();
    scePowerSetClockFrequency(333, 333, 166);
    if (setup_callbacks() < 0) {
        gui_init();
        pspDebugScreenPrintf("exit callback failed; HOME will not work\n");
    }

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    install_recover();
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
    presets_merge(argc > 0 ? argv[0] : NULL);
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

    if (sweep) {
        unsigned since = now_ms();
        int bits = entropy_screen_run();
        logline("entropy: %d bits swept in %u ms%s", bits, now_ms() - since,
                entropy_screen_is_replay() ? " (replay)" : "");
    }
    entropy_save(entropy_screen_is_replay());

    /* The tune starts with the shell and keeps going through installs and
       sweeps; it lives on its own thread and never waits for a frame. */
    audio_start();
    actions_init(&catalog);
    sync_start(&catalog);

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
    int details = 0;                    /* the band about one package */
    unsigned idle_since = now_ms();     /* the last time a key was down */
    unsigned last_frame_ms = now_ms();  /* to notice the loop having been away */
    unsigned shell_since = now_ms();
    int shot_connecting = 0;
    unsigned dumped_ms = now_ms();
    unsigned last_buttons = 0;
    /* Frame times, so a slow frame is a number and not a feeling: every
       ten seconds the average, the worst, and how many missed 60 Hz. */
    unsigned frame_us = now_us(), frames = 0, worst = 0, late = 0, total = 0;
    unsigned bucket[4] = { 0, 0, 0, 0 };    /* 17-20, 20-25, 25-35, >35 ms */
    osk_frame(osk_draw, &cursor);
    for (;;) {
        unsigned now = now_us(), took = now - frame_us;
        frame_us = now;
        frames++; total += took / 1000;
        if (took > worst) worst = took;
        if (took > 100000)
            logline("frame %u: %u ms, %u ms since the shell came up",
                    gfx_frames(), took / 1000, now_ms() - shell_since);
        if (took > 17000) late++;
        if (took > 35000) bucket[3]++;
        else if (took > 25000) bucket[2]++;
        else if (took > 20000) bucket[1]++;
        else if (took > 17000) bucket[0]++;
        /* The emulator only flushes a file on close, and a long session
           should still leave a log behind: once every ten seconds. */
        if (expired(dumped_ms, 10000)) {
            dumped_ms = now_ms();
            logline("frames: %u in 10 s, avg %u ms, worst %u ms, %u late "
                    "(17-20 %u, 20-25 %u, 25-35 %u, 35+ %u)",
                    frames, frames ? total / frames : 0, worst / 1000, late,
                    bucket[0], bucket[1], bucket[2], bucket[3]);
            char phases[120];
            shell_profile(phases, sizeof(phases));
            logline("%s", phases);
            logline("outside draw: tick %u us, audio callback %u us, free %u KB",
                    g_worst_tick, audio_worst_us(),
                    (unsigned)sceKernelTotalFreeMemSize() / 1024);
            g_worst_tick = 0;
            frames = worst = late = total = 0;
            bucket[0] = bucket[1] = bucket[2] = bucket[3] = 0;
            log_dump_later();
        }

        if (!synced) {
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
                if (catalog.generated && catalog.generated + 24u * 3600u < (unsigned)time(NULL)) {
                    unsigned days = ((unsigned)time(NULL) - catalog.generated) / 86400u;
                    char stale[128];
                    snprintf(stale, sizeof(stale),
                             T_STALE,
                             catalog.generated_from, days, days == 1 ? "" : "s");
                    shell_status(stale);
                    logline("catalog: %s generated %u days ago", catalog.generated_from, days);
                }
                /* A fresh catalog is a fresh set of tabs, and the icons
                   cached against the old one no longer stand for the same
                   entries. */
                cursor = 0;
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
                screenshot_settled(cursor, storage_path("PSP/PSPDX/DEBUG/PSPDX.BMP"));
                dump_diagnostics();
                automatic = catalog.count > 0 ? auto_install_index() : -1;
                if (automatic >= 0) {
                    cursor = view_row(automatic);
                    if (cursor < 0) cursor = 0;
                    install_app(automatic, 1, 0, 0);
                    dump_diagnostics();
                    view_settled(&cursor);
                }
                keys_load();
                g_keys_since = now_ms();
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

        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        /* The stick is a hand in the water, whenever it is off centre. */
        lattice_stir((pad.Lx - 128) / 127.0f, (pad.Ly - 128) / 127.0f);
        /* In the band about one package the same hand scrolls what it has
           to say, and the water goes on following it underneath. */
        if (details)
            shell_details_scroll((pad.Ly - 128) / 127.0f);
        unsigned pressed = pad.Buttons & ~last_buttons;
        last_buttons = pad.Buttons;
        shell_hold((pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN)) != 0);
        pressed |= repeat(pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN |
                                         PSP_CTRL_LEFT | PSP_CTRL_RIGHT |
                                         PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER));
        if (synced) pressed |= keys_pressed();
        /* Everything below counts in rows of the shell's view -- the
           catalog filtered to the active tab -- and there are none of those
           while the catalog is being fetched. */
        int count = shown()->count > 0 ? view_count() : 0;

        /* With something standing over the browser, the list stays where it
           is: a question that scrolls out from under its answer is a trap,
           up and down belong to the menu while one is open, and a tab
           changing under a band would change what the band is about. */
        int modal = asking() || menu_shown() || popup_shown() || details || files_view_shown() ||
                    sources_shown();
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

        /* The triggers and left/right walk the tabs, and the list under
           them starts again at the top. They walk whenever there is a
           catalog, not only while there are rows: the UMD tab has none and
           is left the way any tab is. */
        unsigned tabs = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
        if ((pressed & tabs) && shown()->count > 0 && !modal) {
            view_tab_move(pressed & (PSP_CTRL_RTRIGGER | PSP_CTRL_RIGHT) ? 1 : -1);
            cues_post(CUE_MOVE, cursor = 0);
            count = view_count();

        }
        modal = modal || info;
        /* The list is a ring: past the last entry comes the first. */
        if ((pressed & PSP_CTRL_DOWN) && count > 0 && !modal)
            cues_post(CUE_MOVE, cursor = (cursor + 1) % count);
        if ((pressed & PSP_CTRL_UP) && count > 0 && !modal)
            cues_post(CUE_MOVE, cursor = (cursor + count - 1) % count);
        if (pressed & KEY_SHOT) {
            screenshot_settled(cursor, storage_path("PSP/PSPDX/DEBUG/PSPDX1.BMP"));
            logline("shot: PSPDX1.BMP at cursor %d", cursor);
        }

        if (questions_handle(pressed, &cursor, &count, keep, sizeof(keep), &synced, &refreshing)) {
            /* A question stood: the keys were its. */
        } else if (files_view_shown() && !popup_shown()) {
            files_view_keys(pressed, &pad);
        } else if (options_handle(pressed, &cursor, &count, keep, sizeof(keep), &synced, &refreshing, &details)) {
            /* The options or a popup stood: the same. */
        } else if (info) {
            /* The band says what the session is; the one thing to do in it
               is the seed's, next to the entropy it reports. */
            if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS)) shell_info(info = 0);
            else if (pressed & PSP_CTRL_SQUARE) { shell_info(info = 0); sweep_again(); }
        } else if (details) {
            if (pressed & PSP_CTRL_CIRCLE) {
                shell_details(0);
                details = 0;
                /* Information was read out of the options, so closing it
                   goes back there, on the row it was opened from. */
                menu_return();
            }
        } else if (count > 0) {
            int at = view_index(cursor);
            if ((pressed & PSP_CTRL_CROSS) && at <= VIEW_ROW_SETTING) {
                /* A row under the gear does what it says. The three that
                   fetch all end in the same place: the list gives way to
                   the word and the status line and comes back with what is
                   now published, the cursor on the package it was on if
                   that package is still there. The sync thread and the
                   media thread share the one HTTPS stack and the one asset
                   buffer, so the media thread steps aside for the length of
                   it, as it does for an install. */
                int which = VIEW_ROW_SETTING - at;
                if (which == 0 && synced) sources_open();
                else if (which == 1 && synced) sub_open(SUB_ADD);
                else if (which == 2) { files_names(files_name_of); files_view_open(); }
                else if (which == 3) sub_open(SUB_RESET);
                else if (which == 4) shell_info(info = 1);
                else if (which == 5) sub_open(SUB_QUIRKS);
            } else if (pressed & PSP_CTRL_CROSS) {
                /* X is the one thing there is to do to the package: have
                   it, have the newer one, or start it -- each asked about
                   first. The options, with the same things and the rest,
                   are on triangle. */
                if (at == VIEW_ROW_ACTION) {
                    struct view_plan plan;
                    view_action_plan(&plan);
                    if (plan.apps <= 0 && view_tab_kind() == VIEW_TAB_STICK) {
                        if (synced)
                            refetch_now(cursor, keep, sizeof(keep), &synced, &refreshing);
                    } else ask_all();
                }
                else if (at >= 0 && (catalog.apps[at].state == APP_NOT_INSTALLED ||
                                     catalog.apps[at].state == APP_UPDATE))
                    ask_install(at);
                else if (at >= 0) {
                    char title[64];
                    snprintf(title, sizeof(title), T_RUN_ASK, catalog.apps[at].name);
                    ask(ASK_RUN, at, title, T_RUN_LINE);
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
            if ((pressed & PSP_CTRL_SQUARE) && at >= 0 &&
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
        shell_shot_sync(shown(), cursor);
        audio_duck(preview_playing());
        unsigned tick = now_us() - tick0;
        if (tick > g_worst_tick) g_worst_tick = tick;
        shell_draw(shown(), cursor);
        record_frame();
    }
    return 0;
}
