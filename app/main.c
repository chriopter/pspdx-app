#include "text.h"
#include "util/storage.h"
#include "install/state.h"
#include "update/pspdx.h"
#include "update/inbox.h"
#include "util/self_manifest.h"
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
#include "gui/gfx.h"
#include "gui/icons.h"
#include "gui/preview.h"
#include "gui/screen.h"
#include "gui/lattice.h"
#include "gui/marks.h"
#include "gui/osk.h"
#include "gui/shell.h"
#include "install/install.h"
#include "logic/entropy.h"
#include "network/bench.h"
#include "update/catalog.h"
#include "update/sources.h"
#include "update/sync.h"
#include "util/runtime.h"

PSP_MODULE_INFO("pspdx", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
/* One screenshot decodes into a megabyte of texture and wolfSSL wants its own
   working set; four megabytes no longer covers both. */
PSP_HEAP_SIZE_KB(12 * 1024);

static struct catalog catalog;
static unsigned g_worst_tick;       /* worst preview tick (fetch, decode) in us */

static int exit_callback(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    /* The one orderly moment in a run. Everything the session stirred into the
       pool has been sitting in RAM until here, so this is where it reaches the
       stick -- twenty bytes, once, instead of the same sector every few
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

/* The log every time; the catalog's raw response once, after it arrived
   -- it is 200 KB and does not change, and writing it every ten seconds
   was a visible hitch. */
static int g_http_dumped;

static void dump_diagnostics(void) {
    log_dump();
    if (!g_http_dumped && sync_done()) {
        catalog_dump_http();
        g_http_dumped = 1;
    }
}

/* ------------------------------------------------------------------- self */

/* ms0:/PSP/GAME/PSPDX/EBOOT.PBP -> PSPDX. FAT32 keeps no case worth trusting
   and the path arrives from the loader rather than from this program, so the
   marker is matched without it. Returns 0 when the path names no directory
   under PSP/GAME. */
static int dir_under_game(const char *path, char *out, size_t size) {
    static const char mark[] = "PSP/GAME/";
    for (const char *p = path; *p; p++) {
        size_t i = 0;
        while (mark[i] && toupper((unsigned char)p[i]) == mark[i]) i++;
        if (mark[i]) continue;
        const char *start = p + i;
        const char *slash = strchr(start, '/');
        if (!slash) return 0;
        size_t n = (size_t)(slash - start);
        if (n == 0 || n >= size) return 0;
        memcpy(out, start, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

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
    snprintf(self.repo, sizeof(self.repo), "https://github.com/chriopter/pspdx");
    strncpy(self.version, PSPDX_VERSION, sizeof(self.version) - 1);
    /* Started from somewhere this cannot read -- a shell, a host debugger --
       leaves the name the release ships under, which is where it would be. */
    if (!path || !dir_under_game(path, self.dir, sizeof(self.dir)))
        snprintf(self.dir, sizeof(self.dir), "PSPDX");
    if (db_write_record(&self) < 0) {
        logline("self: no record written; PSPDX will list as not installed");
        return;
    }
    logline("self: recorded PSPDX %s in PSP/GAME/%s", self.version, self.dir);
}

/* Until the catalog is here the browser has nothing to browse; it is on
   screen anyway, saying what it waits for. */
static struct catalog empty;

static const struct catalog *shown(void) {
    return sync_done() ? &catalog : &empty;
}

/* Let the transitions finish before photographing the screen, but not
   forever: six seconds covers a film being fetched and started. */
static void screenshot_settled(int cursor, const char *path) {
    /* At least one frame, so the shell has seen the catalog it is about
       to be judged on. */
    for (int i = 0; i < 360; i++) {
        shell_shot_sync(shown(), cursor);
        shell_draw(shown(), cursor);
        if (shell_settled()) break;
    }
    gfx_screenshot(path);
}

/* index is a catalog index; the row of the shell's view it sits on -- the
   catalog filtered to the active tab -- is what the frames drawn around
   the install show, and the entry is on the active tab, since that is
   where it was chosen.

   at and of place this install in a run of them, for the band to say; both
   zero for an install that is only itself. */
/* The install's progress, and the one place a single install can be left:
   circle, read between the pieces the network and the unpack hand over. */
static void install_progress(void *ctx, size_t done, size_t total) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    if (pad.Buttons & PSP_CTRL_CIRCLE) install_abort();
    shell_install_progress(ctx, done, total);
}

/* Set when PSPDX has replaced itself: the loop asks to restart once the
   install that did it, or the batch it was the last of, is over. */
static int g_restart_of = -1;
static char g_restart_version[32];

static int install_app(int index, int screenshot, int at, int of) {
    if (index < 0 || index >= catalog.count) return -1;
    int row = shell_view_row(index);
    if (row < 0) row = 0;
    struct app_entry *entry = &catalog.apps[index];
    struct install_report report;

    shell_install_begin(entry->name, at, of);
    cues_post(CUE_OPEN, 0);
    /* The installer and the media thread share one HTTPS stack and one
       asset buffer; only one of them talks to the network at a time. */
    preview_quiesce();
    /* The handshake and the checksum run flat out on this thread, and the
       audio thread sits one step under it by design -- see audio.c -- so
       for the length of the install this thread steps under the audio
       thread instead. The tune keeps playing; the progress bar, drawn from
       the installer's callbacks, gets what is left, which is nearly all. */
    SceUID self = sceKernelGetThreadId();
    sceKernelChangeThreadPriority(self, 0x22);
    unsigned start = now_ms();
    catalog_offline(net_up()<0);
    int rc = entry->has_release ? catalog_prepare(entry) : -1;
    if(rc==0)rc=install_release(&entry->release, &report, shell_install_phase,
                             install_progress, NULL);
    unsigned seconds = (now_ms() - start) / 1000;
    sceKernelChangeThreadPriority(self, 0x20);
    preview_resume();

    char message[96];
    if (rc == 0) {
        entry->state = APP_CURRENT;
        entry->local_rev = report.rev;
        snprintf(entry->local_version, sizeof(entry->local_version), "%s", report.version);
        entry->local_version[sizeof(entry->local_version) - 1] = '\0';
        /* The client can fetch itself, and just has: the EBOOT that is running
           is the one in RAM, and the file it was loaded from has been renamed
           aside and replaced underneath it. Nothing on screen is the new
           version until the console loads it, so the band says which button
           does that rather than reporting a file count nobody needs. */
        if (strcmp(entry->id, PSPDX_SELF_ID) == 0) {
            snprintf(message, sizeof(message), T_UPDATED_SELF, report.version);
            snprintf(g_restart_version, sizeof(g_restart_version), "%s", report.version);
            g_restart_of = index;
        } else
            snprintf(message, sizeof(message), T_INSTALLED, entry->name, report.version);
            logline("installed %s %s: %d files, %luK, %us", entry->name, report.version,
                    report.files, (unsigned long)(report.bytes / 1024), seconds);
    } else if (rc == INSTALL_CANCELLED) {
        snprintf(message, sizeof(message), T_CANCELLED, entry->name);
    } else {
        snprintf(message, sizeof(message), T_INSTALL_FAILED, entry->name, rc);
    }
    shell_install_end(message);
    cues_post(rc == 0 ? CUE_DONE : CUE_FAIL, 0);
    shell_draw(&catalog, row);
    if (screenshot) screenshot_settled(row, storage_path("PSP/PSPDX/DEBUG/PSPDX2.BMP"));
    return rc;
}

static void uninstall_app(int index) {
    struct app_entry *entry = &catalog.apps[index];
    char message[96];

    cues_post(CUE_OPEN, 0);
    int rc = uninstall(entry->id);
    if (rc == 0) {
        /* The catalog entry is what the browser reads; the record it was
           built from has just stopped existing. */
        entry->state = APP_NOT_INSTALLED;
        entry->local_rev = 0;
        entry->local_version[0] = '\0';
        snprintf(message, sizeof(message), T_REMOVED, entry->name);
    } else {
        snprintf(message, sizeof(message), T_REMOVE_FAILED, entry->name, rc);
    }
    logline("%s", message);
    shell_status(message);
    cues_post(rc == 0 ? CUE_DONE : CUE_FAIL, 0);
}

/* After anything that changes what is in the tabs rather than what is in the
   catalog -- an install that was the last update waiting, a basket filled or
   emptied. The cursor stays on the package it was on for as long as that
   package is still shown; a tab that has gone out from under it puts it back
   on All at the top, which is the only row that is certainly there. */
static void view_settled(int *cursor) {
    int at = shell_view_index(*cursor);
    if (!shell_tabs_refresh()) { *cursor = 0; return; }
    int row = at >= 0 ? shell_view_row(at) : -1;
    int count = shell_view_count();
    if (row >= 0) *cursor = row;
    else if (*cursor >= count) *cursor = count > 0 ? count - 1 : 0;
}

/* Hands the PSP over to the package the cursor is on. Nothing comes back from
   this call, so everything the session was holding has to be on the stick
   before it: the pool above all, which otherwise only reaches the seed file
   when the user quits through HOME. */
static void launch_app(int index) {
    const struct app_entry *entry = &catalog.apps[index];
    struct installed record;
    char path[160];

    if (db_read(entry->id, &record) < 0 || !record.dir[0]) {
        shell_status(T_NO_RECORD);
        return;
    }
    snprintf(path, sizeof(path), storage_path("PSP/GAME/%s/EBOOT.PBP"), record.dir);
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        shell_status(T_NO_EBOOT);
        logline("launch: %s is not there", path);
        return;
    }
    sceIoClose(fd);

    cues_post(CUE_OPEN, 0);
    logline("launching %s", path);
    entropy_save(entropy_screen_is_replay());
    audio_stop();
    log_dump();

    struct SceKernelLoadExecParam param;
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.args = strlen(path) + 1;
    param.argp = path;
    param.key = "game";
    int rc = sceKernelLoadExec(path, &param);
    /* Only reached when the firmware refused it. */
    logline("launch: refused %08x", rc);
    shell_status(T_START_REFUSED);
}

/* --------------------------------------------------------------- questions */

/* Nothing that writes to the stick starts on one press any more. The shell
   draws the question and the footer that answers it; the answer arrives
   through the pad, which is read down in the loop, so the two halves meet
   in these two variables and nowhere else. */
enum question { ASK_NOTHING, ASK_INSTALL, ASK_ASIDE, ASK_REMOVE, ASK_ALL, ASK_INBOX, ASK_CATALOG, ASK_RESET, ASK_DISCARD, ASK_RUN, ASK_RESTART };
static enum question g_question;
static int g_question_of;

static void ask_install(int index) {
    const struct app_entry *entry = &catalog.apps[index];
    const char *version = entry->remote_version[0] ? entry->remote_version
                                                   : entry->release.version;
    char title[64], line[96];
    if (entry->state == APP_UPDATE)
        snprintf(title, sizeof(title), T_UPDATE_ASK, entry->name);
    else
        snprintf(title, sizeof(title), T_INSTALL_ASK, entry->name);
    if (entry->has_release && entry->release.size) {
        /* Tenths: whole megabytes call everything under one of them nothing,
           and a count of bytes is not a size anybody reads. */
        unsigned long long size = entry->release.size;
        snprintf(line, sizeof(line), T_INSTALL_LINE, version,
                 (unsigned long)(size >> 20), (unsigned long)((size * 10 >> 20) % 10));
    } else {
        snprintf(line, sizeof(line), T_INSTALL_LINE_NOSIZE, version);
    }
    struct installed previous;
    int recorded = db_read(entry->id, &previous) == 0;
    /* Something already under the name the release wants, and not this
       app's own directory. The installer would refuse it; the shell says
       so first, and for the one case it can do something about -- a
       directory somebody copied there by hand -- offers to park it under
       .bak. That is a single rename, so nothing is half-done if the
       battery comes out; the transaction's own backup name, .old, is left
       to the transaction. A .bak is the user's and is never touched. */
    const char *dir = entry->release.dir;
    if (dir[0] && !(recorded && !strcasecmp(previous.dir, dir))) {
        char dest[160], bak[160];
        snprintf(dest, sizeof(dest), storage_path("PSP/GAME/%s"), dir);
        snprintf(bak, sizeof(bak), storage_path("PSP/GAME/%s.bak"), dir);
        if (state_target_owner(dir, entry->id) == 1) {
            snprintf(line, sizeof(line), T_DIR_OTHER_APP, dir);
            shell_status(line);
            return;
        }
        if (storage_exists(dest)) {
            if (storage_exists(bak)) {
                snprintf(line, sizeof(line), T_DIR_BAK_EXISTS, dir);
                shell_status(line);
                return;
            }
            snprintf(title, sizeof(title), T_DIR_EXISTS_ASK, dir);
            snprintf(line, sizeof(line), T_DIR_EXISTS_LINE, dir);
            shell_ask(title, line);
            g_question = ASK_ASIDE;
            g_question_of = index;
            return;
        }
    }
    if(recorded && strcmp(previous.dir,entry->release.dir))
        snprintf(line,sizeof(line),T_INSTALL_MOVES,entry->release.dir);
    shell_ask(title, line);
    g_question = ASK_INSTALL;
    g_question_of = index;
}

/* The answer to ASK_ASIDE: one rename, then the install it was asked for.
   The new name is a bare name because that is what sceIoRename takes for
   its second argument, the same way the installer moves its own directories. */
static int set_aside(int index) {
    const char *dir = catalog.apps[index].release.dir;
    char dest[160], name[80], line[96];
    snprintf(dest, sizeof(dest), storage_path("PSP/GAME/%s"), dir);
    snprintf(name, sizeof(name), "%s.bak", dir);
    if (sceIoRename(dest, name) < 0) {
        snprintf(line, sizeof(line), T_RENAME_FAILED, dir);
        logline("%s", line);
        shell_status(line);
        cues_post(CUE_FAIL, 0);
        return -1;
    }
    logline("install: moved PSP/GAME/%s to %s.bak", dir, dir);
    return 0;
}

static void ask_remove(int index) {
    const struct app_entry *entry = &catalog.apps[index];
    struct installed record;
    /* The one package on the list that this program will not delete. The
       directory it would delete is the one the running EBOOT came out of:
       what is in RAM would go on running with nothing left to restart, and
       the update that is the point of listing PSPDX at all would have
       nowhere to land. */
    if (strcmp(entry->id, PSPDX_SELF_ID) == 0) {
        shell_status(T_SELF_DELETE);
        return;
    }
    if (db_read(entry->id, &record) < 0 || !record.dir[0]) {
        /* Without a record there is no directory to name, and nothing here
           guesses at one. */
        shell_status(T_NO_RECORD);
        return;
    }
    char title[64], line[96];
    snprintf(title, sizeof(title), T_REMOVE_ASK, entry->name);
    snprintf(line, sizeof(line), "%s", T_REMOVE_LINE);
    shell_ask(title, line);
    g_question = ASK_REMOVE;
    g_question_of = index;
}

/* The action row's question, over the whole tab rather than one package. The
   shell has already worked out what would be fetched -- the rows are its
   business, not this loop's -- so all that happens here is putting the tally
   into words. */
static void ask_all(void) {
    struct shell_plan plan;
    shell_action_plan(&plan);
    if (plan.apps <= 0) {
        shell_status(T_NOTHING_TO_DOWNLOAD);
        return;
    }
    char title[64], line[96], size[24];
    unsigned long long bytes = plan.bytes;
    snprintf(size, sizeof(size), "%lu.%lu MB",
             (unsigned long)(bytes >> 20), (unsigned long)((bytes * 10 >> 20) % 10));
    snprintf(title, sizeof(title), plan.updates ? T_ALL_ASK_UPDATE : T_ALL_ASK_INSTALL,
             plan.apps, plan.apps == 1 ? "" : "s");
    int n = snprintf(line, sizeof(line), T_ALL_SIZE, size);
    /* A package already on the stick and already current can be put in the
       basket, and fetching it again is a reinstall rather than nothing: that
       is worth one clause here rather than a surprise afterwards. */
    if (plan.again > 0 && n < (int)sizeof(line))
        n += snprintf(line + n, sizeof(line) - n, T_ALL_AGAIN, plan.again);
    if (plan.skipped > 0 && n < (int)sizeof(line))
        snprintf(line + n, sizeof(line) - n, T_ALL_SKIPPED,
                 plan.skipped);
    shell_ask(title, line);
    g_question = ASK_ALL;
    g_question_of = -1;
}

static void ask_forget(void) {
    g_question = ASK_NOTHING;
    shell_ask(NULL, NULL);
}

/* The action row taken: everything the tab holds, one after another, in the
   order it is listed. The rows are read into a list before the first fetch --
   an install moves the entry's state, and on the updates tab that takes the
   row out from under a loop still walking the view. A failure is said and the
   rest still go: one package the server has lost is not a reason to leave the
   others unfetched. The basket keeps what did not arrive and lets go of what
   did. */
static void install_all(void) {
    int list[MAX_APPS], n = 0;
    for (int row = 0; row < shell_view_count() && n < MAX_APPS; row++) {
        int at = shell_view_index(row);
        if (at < 0) continue;
        const struct app_entry *entry = &catalog.apps[at];
        if (!entry->has_release || !entry->release.size) continue;
        /* On the stick the job is the updates alone. */
        if (shell_tab_kind() == SHELL_TAB_STICK && entry->state != APP_UPDATE) continue;
        list[n] = at;
        n++;
    }
    for(int i=0;i<n-1;i++)if(!strcmp(catalog.apps[list[i]].id,PSPDX_SELF_ID)){int self=list[i];memmove(list+i,list+i+1,(n-i-1)*sizeof(int));list[n-1]=self;break;}
    int done = 0;
    for (int i = 0; i < n; i++) {
        SceCtrlData pad;sceCtrlPeekBufferPositive(&pad,1);if(pad.Buttons&PSP_CTRL_CIRCLE)break;
        if (install_app(list[i], 0, i + 1, n) == 0) {
            shell_basket_forget(list[i]);
            done++;
        }
        dump_diagnostics();
    }
    char message[96];
    snprintf(message, sizeof(message), T_ALL_DONE, done, n);
    logline("%s", message);
    shell_status(message);
    /* One note for the run being over. Each install that failed sounded its
       own at the time it did, so this is not saying they all worked. */
    cues_post(CUE_DONE, 0);
}

/* ------------------------------------------------------------------- menu */

/* An installed package has more than one thing that can be done to it, so X
   opens the short list of them rather than a yes/no. Exactly one of the two
   ways of fetching it is ever available: the catalog carries the current
   release and nothing else, so a package with an update waiting cannot be
   reinstalled at the version it has, and one already current has nothing to
   update to. The unavailable one stays on screen, greyed, because which of
   the two is greyed is itself the answer to "is there an update". */
enum choice { CHOICE_RUN, CHOICE_GET, CHOICE_DELETE, CHOICE_BASKET, CHOICE_DETAILS,
              CHOICE_COUNT };

static char g_choice_text[CHOICE_COUNT][32];
static unsigned char g_choice_on[CHOICE_COUNT], g_choice_shown[CHOICE_COUNT];
/* The rows the panel is handed: the shown choices, in order. */
static int g_rows, g_row[CHOICE_COUNT];
static const char *g_row_text[CHOICE_COUNT];
static unsigned char g_row_on[CHOICE_COUNT];
static signed char g_row_key[CHOICE_COUNT];
static char g_menu_title[48];
static int g_menu_open, g_menu_cursor, g_menu_of, g_details_from_menu;

/* The keys that do a row's thing without the menu, named at the row: the
   menu is where they are learned. */
static const signed char g_choice_key[CHOICE_COUNT] = {
    [CHOICE_RUN] = MARK_START, [CHOICE_GET] = -1, [CHOICE_DELETE] = -1,
    [CHOICE_BASKET] = MARK_SQUARE, [CHOICE_DETAILS] = -1,
};

static void menu_push(void) {
    shell_menu(g_menu_title, g_row_text, g_row_on, g_row_key, g_rows, g_menu_cursor);
}

static int row_of(enum choice c) {
    for (int r = 0; r < g_rows; r++)
        if (g_row[r] == c) return r;
    return 0;
}

static void menu_open(int index) {
    const struct app_entry *entry = &catalog.apps[index];
    int installed = entry->state != APP_NOT_INSTALLED;
    snprintf(g_menu_title, sizeof(g_menu_title), "%s", entry->name);
    /* Five rows, the same five for every package, in the same places; what
       a row cannot do to this package it says by being grey. The second
       fetches the package, and says which fetch it would be: Install for
       one not on the stick, Update to the newer version where one waits,
       Reinstall for the one already current. */
    snprintf(g_choice_text[CHOICE_RUN], sizeof(g_choice_text[0]), "%s",
             strcmp(entry->id, PSPDX_SELF_ID) == 0 ? T_MENU_RESTART : T_MENU_RUN);
    if (entry->state == APP_UPDATE)
        snprintf(g_choice_text[CHOICE_GET], sizeof(g_choice_text[0]), T_MENU_UPDATE,
                 entry->remote_version);
    else
        snprintf(g_choice_text[CHOICE_GET], sizeof(g_choice_text[0]),
                 installed ? T_MENU_REINSTALL : T_MENU_INSTALL);
    snprintf(g_choice_text[CHOICE_DELETE], sizeof(g_choice_text[0]), T_MENU_DELETE);
    /* The basket is for what is not on the stick yet, so only such a package
       has the row; one already in the basket keeps it, to come out again.
       The basket is named by its mark, which the shell draws after the
       words. */
    snprintf(g_choice_text[CHOICE_BASKET], sizeof(g_choice_text[0]), "%s\x01%c",
             shell_basket_has(index) ? T_MENU_BASKET_OUT : T_MENU_BASKET_IN, (char)(MARK_BASKET + 1));
    snprintf(g_choice_text[CHOICE_DETAILS], sizeof(g_choice_text[0]), T_MENU_INFO);
    g_choice_on[CHOICE_RUN] = installed;
    g_choice_on[CHOICE_GET] = 1;
    g_choice_on[CHOICE_DELETE] = installed && strcmp(entry->id, PSPDX_SELF_ID) != 0;
    g_choice_on[CHOICE_BASKET] = 1;
    g_choice_on[CHOICE_DETAILS] = 1;
    for (int i = 0; i < CHOICE_COUNT; i++) g_choice_shown[i] = 1;
    g_choice_shown[CHOICE_BASKET] = !installed || shell_basket_has(index);
    g_rows = 0;
    for (int i = 0; i < CHOICE_COUNT; i++) {
        if (!g_choice_shown[i]) continue;
        g_row[g_rows] = i;
        g_row_text[g_rows] = g_choice_text[i];
        g_row_on[g_rows] = g_choice_on[i];
        g_row_key[g_rows] = g_choice_key[i];
        g_rows++;
    }
    g_menu_cursor = row_of(entry->state == APP_NOT_INSTALLED || entry->state == APP_UPDATE
                           ? CHOICE_GET : CHOICE_RUN);
    g_menu_of = index;
    g_menu_open = 1;
    menu_push();
}

static void menu_close(void) {
    g_menu_open = 0;
    shell_menu(NULL, NULL, NULL, NULL, 0, 0);
}

/* The two popups under the gear, in the same panel the options use: the
   catalogs this console reads, one a row with "Add" last; and the two ways
   a .pspdx comes in directly. Drawn by the shell, driven here. */
enum sub { SUB_NONE, SUB_CATALOGS, SUB_ADD, SUB_RESET, SUB_FILES };
static enum sub g_sub;
static int g_sub_cursor, g_sub_count;
static struct sources g_sources;
static char g_sub_short[SOURCES_MAX][48];
static const char *g_sub_item[SOURCES_MAX + 1];
static unsigned char g_sub_on[SOURCES_MAX + 1];
static signed char g_sub_key[SOURCES_MAX + 1];

static void sub_push(void) {
    shell_menu(g_sub == SUB_CATALOGS ? T_SUB_SOURCES : g_sub == SUB_ADD ? T_SUB_DIRECT
               : g_sub == SUB_FILES ? T_SUB_FILES : T_SUB_RESET,
               g_sub_item, g_sub_on, g_sub_key, g_sub_count, g_sub_cursor);
}

static void sub_open(enum sub which) {
    g_sub = which;
    g_sub_count = 0;
    if (which == SUB_CATALOGS) {
        sources_load(&g_sources);
        for (int i = 0; i < g_sources.count; i++) {
            /* The scheme goes; every source has it, and the panel is narrow. */
            const char *u = g_sources.url[i];
            if (!strncmp(u, "https://", 8)) u += 8;
            snprintf(g_sub_short[i], sizeof(g_sub_short[i]), "%s", u);
            g_sub_item[g_sub_count++] = g_sub_short[i];
        }
        g_sub_item[g_sub_count++] = T_SUB_ADD_SOURCE;
    } else if (which == SUB_ADD) {
        g_sub_item[g_sub_count++] = T_SUB_FROM_GITHUB;
        g_sub_item[g_sub_count++] = T_SUB_FROM_INBOX;
    } else if (which == SUB_FILES) {
        g_sub_item[g_sub_count++] = T_SUB_VIEW_RAW;
    } else {
        g_sub_item[g_sub_count++] = T_SUB_DISCARD;
        g_sub_item[g_sub_count++] = T_SUB_SWEEP;
        g_sub_item[g_sub_count++] = T_SUB_RESET_ALL;
    }
    for (int i = 0; i < g_sub_count; i++) { g_sub_on[i] = 1; g_sub_key[i] = -1; }
    /* The row is there for the one case recovery could not settle, and
       greyed while there is nothing unfinished. */
    if (which == SUB_RESET)
        g_sub_on[2] = storage_exists(storage_path("PSP/PSPDX/TMP/transaction.json"));
    g_sub_cursor = 0;
    sub_push();
}

static void sub_close(void) {
    g_sub = SUB_NONE;
    shell_menu(NULL, NULL, NULL, NULL, 0, 0);
}

/* A greyed row is stepped over rather than landed on: the cursor only ever
   sits where X would do something. */
static void menu_move(int by) {
    for (int i = 0; i < g_rows; i++) {
        g_menu_cursor = (g_menu_cursor + by + g_rows) % g_rows;
        if (g_row_on[g_menu_cursor]) break;
    }
    menu_push();
}

/* The unattended install for the rig: PSPDX.INSTALL on the stick names
   a repository, as a URL or as owner/repo, and it is asked at the origin
   like one typed on the gear tab. Returns its index in the catalog. */
static int auto_install_index(void) {
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
    int index = catalog_add_repo(&catalog, url);
    preview_resume();
    if (index < 0) logline("PSPDX.INSTALL: nothing to install at %s", url);
    else shell_view_rebuild(&catalog);
    return index;
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

/* Manage Files: the view util/files.c fills out of the stick, and whether
   it is on screen. */
static struct file_view g_files;
static int g_files_open;

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

/* What the gear tab's two typing rows lead to: a source added and the
   catalog fetched again, and for "Install from GitHub" the one repository
   typed, found in the new catalog by its URL and put under the cursor
   with the install question already asked. */
static char g_wanted_url[SOURCE_URL];     /* that repository, while one is waited for */
static char g_wanted_name[64];            /* owner/repo, for the status line */

/* The catalog fetched again: the list gives way to the word and the
   status line and comes back with what is now published, the cursor on the
   package it was on if that package is still there. The sync thread and
   the media thread share the one HTTPS stack and the one asset buffer, so
   the media thread steps aside for the length of it. */
static void refetch_now(int cursor, char *keep, size_t keep_size, int *synced,
                        int *refreshing) {
    int was = shell_view_index(cursor);
    snprintf(keep, keep_size, "%s", was >= 0 ? catalog.apps[was].id : "");
    preview_quiesce();
    shell_word(T_WORD_CHECKING);
    if (sync_start(&catalog) == 0) {
        *synced = 0;
        *refreshing = 1;
    } else {
        keep[0] = '\0';
        g_wanted_url[0] = '\0';
        preview_resume();
    }
}

/* The field drains and is swept again, in the room the browser was already
   standing in. The old pool is set aside rather than thrown away until the
   new one is made: a sweep takes half a minute of stick work and this one
   is two presses from the list, so it has to be possible to leave, and
   leaving may not hand the rest of the session a pool of nothing to make
   its keys from. */
static void sweep_again(void) {
    entropy_stash();
    entropy_init();
    if (entropy_screen_run() == 0) entropy_restore();
    entropy_save(entropy_screen_is_replay());
}

/* Back to the first start: everything this client keeps on the stick goes,
   and the client with it, so that what comes up next is a first start --
   the sweep, the built-in list, nothing installed as far as it knows. The
   apps under PSP/GAME are not its to remove. */
static void reset_completely(void) {
    log_dump();
    if (storage_remove_tree(storage_path("PSP/PSPDX")) < 0)
        logline("reset: some of PSP/PSPDX would not go");
    sceKernelExitGame();
}

/* A journal recovery could not finish blocks every install; this is the
   hand that clears it. What is under PSP/GAME stays, and the line says so. */
static void discard_unfinished(void) {
    char line[128];
    if (install_discard(line, sizeof(line)) < 0)
        shell_status(T_DISCARD_FAILED);
    else
        shell_status(line);
}

/* Reads a source from the keyboard and adds it. Returns 1 when the catalog
   should be fetched again, 0 when there is nothing new. */
static int type_source(int install) {
    char text[SOURCE_URL], url[SOURCE_URL];
    int rc = osk_read(install ? T_OSK_GITHUB : T_OSK_SOURCE, "", text, sizeof(text));
    if (rc <= 0 || !text[0]) return 0;
    rc = sources_normalize(text,url,sizeof(url));
    struct source_repo parsed;
    if(rc<0 || (install && !sources_parse_repo(url,&parsed))) {shell_status(T_BAD_ADDRESS);return 0;}
    preview_quiesce();catalog_offline(net_up()<0);
    rc=catalog_validate_source(url,install);
    preview_resume();
    if(rc<0){shell_status(T_SOURCE_UNAVAILABLE);return 0;}
    rc=sources_add(url,url,sizeof(url));
    if(rc<0){shell_status(T_SOURCE_SAVE_FAILED);return 0;}
    if (!install) {
        if (rc == 0) { shell_status(T_SOURCE_EXISTS); return 0; }
        return 1;
    }
    struct source_repo repo;
    if (!sources_parse_repo(url, &repo)) {
        shell_status(T_GITHUB_FORMAT);
        return 0;
    }
    sources_repo_url(&repo, g_wanted_url, sizeof(g_wanted_url));
    snprintf(g_wanted_name, sizeof(g_wanted_name), "%.24s/%.36s", repo.owner, repo.name);
    return 1;
}

/* The catalog is back: the repository typed is either in it, and the
   question is asked, or it is not, and the status line says why. */
static int wanted_settled(int *cursor) {
    char message[96];
    int found = catalog_find_repo(&catalog, g_wanted_url);
    if (found < 0) {
        int why = catalog_refused(g_wanted_url);
        if (why == REFUSED_PSPDX)
            snprintf(message, sizeof(message), T_WANT_NO_PSPDX, g_wanted_name);
        else if (why == REFUSED_REPO)
            snprintf(message, sizeof(message), T_WANT_NO_REPO, g_wanted_name);
        else if (why == REFUSED_RELEASE)
            snprintf(message, sizeof(message), T_WANT_NO_RELEASE, g_wanted_name);
        else
            snprintf(message, sizeof(message), T_WANT_FAILED,
                     g_wanted_name);
        shell_status(message);
        return -1;
    }
    /* The tab that is open need not show it: All does, and is at most a
       ring of tabs away. */
    for (int n = shell_tab_count(); n > 0 && shell_view_row(found) < 0; n--)
        shell_tab_move(1);
    *cursor = shell_view_row(found);
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

static void ask_inbox(void){
    preview_quiesce();catalog_offline(net_up()<0);
    shell_word(T_WORD_INBOX);
    int n=inbox_scan(&catalog);preview_resume();shell_view_rebuild(&catalog);
    if(n<=0){shell_status(T_INBOX_EMPTY);return;}
    char title[64],line[96];snprintf(title,sizeof(title),T_INBOX_ASK,n);
    snprintf(line,sizeof(line),"%s",inbox_summary());shell_ask(title,line);g_question=ASK_INBOX;
}
static void install_inbox(void){
    int count=inbox_count(),done=0;
    for(int i=0;i<count;i++){
        SceCtrlData pad;sceCtrlPeekBufferPositive(&pad,1);if(pad.Buttons&PSP_CTRL_CIRCLE)break;
        int at=inbox_index(i);
        if(install_app(at,0,i+1,count)==0){inbox_installed(i);done++;}
    }
    char message[96];snprintf(message,sizeof(message),T_INBOX_DONE,done,count);shell_status(message);
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
       PSP/GAME, and before the sync, which reads every record there is. */
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
            !strcmp(file.source, "https://github.com/chriopter/pspdx"))
            storage_write(self_manifest_path, bundled, n);
        else
            storage_write(self_manifest_path, PSPDX_SELF_MANIFEST, strlen(PSPDX_SELF_MANIFEST));
        free(bundled);
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
    sync_start(&catalog);

    /* Connect, fetch and check in the background while the first frames
       go up; the status line follows along. */
    int cursor = 0;
    int synced = 0;
    int rounds = 0;                     /* times the sync has come back */
    int refreshing = 0;                 /* SELECT, with the list already up */
    char keep[96] = "";                 /* the entry to come back to after one */
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
                    char stale[96];
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
                shell_view_rebuild(&catalog);
                if (keep[0]) {
                    /* Back to the package the cursor was on, if the catalog
                       still has it; the top of the list if it does not. */
                    for (int i = 0; i < catalog.count; i++)
                        if (strcmp(catalog.apps[i].id, keep) == 0) {
                            int row = shell_view_row(i);
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
                if (sync_state() == SYNC_DONE) shell_status("");
                if (g_wanted_url[0]) {
                    /* A fetch that failed altogether says so below; the
                       repository waited for is let go of either way. */
                    if (sync_state() == SYNC_DONE) wanted_settled(&cursor);
                    g_wanted_url[0] = '\0';
                }
                if (sync_state() != SYNC_DONE) {
                    /* Nothing came: say so, and offer the one thing that
                       can be done about it. */
                    char again[96];
                    snprintf(again, sizeof(again), "%s   X to try again", sync_message());
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
                    cursor = shell_view_row(automatic);
                    if (cursor < 0) cursor = 0;
                    install_app(automatic, 1, 0, 0);
                    dump_diagnostics();
                    view_settled(&cursor);
                }
                keys_load();
                g_keys_since = now_ms();
                int bench = sceIoOpen(storage_path("PSP/PSPDX/DEBUG/PSPDX.BENCH"), PSP_O_RDONLY, 0777);
                if (bench >= 0) {
                    sceIoClose(bench);
                    shell_status("benchmarking ciphers");
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
        unsigned pressed = pad.Buttons & ~last_buttons;
        last_buttons = pad.Buttons;
        pressed |= repeat(pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN |
                                         PSP_CTRL_LEFT | PSP_CTRL_RIGHT |
                                         PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER));
        if (synced) pressed |= keys_pressed();
        /* Everything below counts in rows of the shell's view -- the
           catalog filtered to the active tab -- and there are none of those
           while the catalog is being fetched. */
        int count = shown()->count > 0 ? shell_view_count() : 0;

        /* With something standing over the browser, the list stays where it
           is: a question that scrolls out from under its answer is a trap,
           up and down belong to the menu while one is open, and a tab
           changing under a band would change what the band is about. */
        int modal = g_question != ASK_NOTHING || g_menu_open || g_sub || details || g_files_open;

        /* Ten seconds without a key and the picture of the package under
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
           them starts again at the top. The leftmost tab is the band about
           the session: walking onto it opens the band, walking off it
           closes it, so the band is left the way any tab is. */
        unsigned tabs = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
        if ((pressed & tabs) && count > 0 && !modal) {
            shell_tab_move(pressed & (PSP_CTRL_RTRIGGER | PSP_CTRL_RIGHT) ? 1 : -1);
            cues_post(CUE_MOVE, cursor = 0);
            count = shell_view_count();

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

        /* PSPDX has just replaced itself. The copy running is the old one
           until the console loads the new, so that is offered as soon as
           nothing else is being asked. */
        if (g_question == ASK_NOTHING && g_restart_of >= 0) {
            char line[64];
            snprintf(line, sizeof(line), T_RESTART_LINE, g_restart_version);
            shell_ask(T_RESTART_ASK, line);
            g_question = ASK_RESTART;
            g_question_of = g_restart_of;
            g_restart_of = -1;
        }

        if (g_question != ASK_NOTHING) {
            /* The answer, and only then the thing that was asked about. */
            if (pressed & PSP_CTRL_CROSS) {
                enum question asked = g_question;
                int index = g_question_of;
                ask_forget();
                if (asked == ASK_INSTALL) install_app(index, 0, 0, 0);
                else if (asked == ASK_ASIDE) { if (set_aside(index) == 0) install_app(index, 0, 0, 0); }
                else if (asked == ASK_ALL) install_all();
                else if (asked == ASK_INBOX) install_inbox();
                else if (asked == ASK_RESET) reset_completely();
                else if (asked == ASK_DISCARD) discard_unfinished();
                else if (asked == ASK_RUN || asked == ASK_RESTART) launch_app(index);
                else if (asked == ASK_CATALOG) {
                    if (index >= 0 && index < g_sources.count &&
                        sources_remove(g_sources.url[index]) > 0)
                        refetch_now(cursor, keep, sizeof(keep), &synced, &refreshing);
                    else shell_status(T_SOURCE_DELETE_FAILED);
                } else uninstall_app(index);
                dump_diagnostics();
                /* What was just done can have emptied a tab. */
                view_settled(&cursor);
                count = shell_view_count();
            } else if (pressed & PSP_CTRL_CIRCLE) {
                int later = g_question == ASK_RESTART;
                ask_forget();
                shell_status(later ? T_RESTART_LATER : "");
            }
        } else if (g_files_open && !g_sub) {
            struct file_view *v = &g_files;
            if ((pressed & PSP_CTRL_DOWN) && v->count) { files_move(v, 1); cues_post(CUE_MOVE, v->cursor); }
            else if ((pressed & PSP_CTRL_UP) && v->count) { files_move(v, -1); cues_post(CUE_MOVE, v->cursor); }
            else if ((pressed & PSP_CTRL_CROSS) && v->count) { if (files_enter(v)) cues_post(CUE_OPEN, 0); }
            else if (pressed & PSP_CTRL_CIRCLE) {
                if (!files_back(v)) { g_files_open = 0; shell_files(NULL); }
            }
            else if ((pressed & PSP_CTRL_TRIANGLE) && v->count && !v->deeper) sub_open(SUB_FILES);
            else if ((pressed & PSP_CTRL_TRIANGLE) && v->count && v->level == 1 && v->area >= 0) sub_open(SUB_FILES);
            else if (pressed & PSP_CTRL_RTRIGGER) files_scroll(v, shell_files_page());
            else if (pressed & PSP_CTRL_LTRIGGER) files_scroll(v, -shell_files_page());
        } else if (g_sub) {
            if (pressed & PSP_CTRL_DOWN) { g_sub_cursor = (g_sub_cursor + 1) % g_sub_count; sub_push(); cues_post(CUE_MOVE, 0); }
            else if (pressed & PSP_CTRL_UP) { g_sub_cursor = (g_sub_cursor + g_sub_count - 1) % g_sub_count; sub_push(); cues_post(CUE_MOVE, 0); }
            else if (pressed & PSP_CTRL_CIRCLE) sub_close();
            else if (pressed & PSP_CTRL_CROSS) {
                int chosen = g_sub_cursor;
                enum sub kind = g_sub;
                sub_close();
                if (kind == SUB_CATALOGS) {
                    if (chosen < g_sources.count) {
                        shell_ask(T_SOURCE_DELETE_ASK, g_sub_short[chosen]);
                        g_question = ASK_CATALOG;
                        g_question_of = chosen;
                    } else if (synced && type_source(0)) {
                        refetch_now(cursor, keep, sizeof(keep), &synced, &refreshing);
                    }
                } else if (kind == SUB_FILES) {
                    if (chosen == 0) files_raw(&g_files);
                } else if (kind == SUB_ADD) {
                    if (chosen == 0 && synced && type_source(1))
                        refetch_now(cursor, keep, sizeof(keep), &synced, &refreshing);
                    else if (chosen == 1 && synced) ask_inbox();
                } else {
                    if (chosen == 1) sweep_again();
                    else if (chosen == 2) {
                        shell_ask(T_RESET_ASK, T_RESET_LINE);
                        g_question = ASK_RESET;
                        g_question_of = -1;
                    } else {
                        shell_ask(T_DISCARD_ASK, T_DISCARD_LINE);
                        g_question = ASK_DISCARD;
                        g_question_of = -1;
                    }
                }
            }
        } else if (g_menu_open) {
            /* The keys the menu names work from inside it too, so that what
               is read there can be pressed there: square, START and SELECT
               do their row's thing and take the menu with them. */
            int index = g_menu_of;
            if ((pressed & PSP_CTRL_SQUARE) &&
                (catalog.apps[index].state == APP_NOT_INSTALLED || shell_basket_has(index))) {
                menu_close();
                shell_basket_toggle(index);
                cues_post(CUE_MOVE, cursor);
                view_settled(&cursor);
                count = shell_view_count();
            } else if ((pressed & PSP_CTRL_START) &&
                       catalog.apps[index].state != APP_NOT_INSTALLED) {
                menu_close();
                launch_app(index);
            }
            if (!g_menu_open) { /* taken by one of the keys above */ }
            else if (pressed & PSP_CTRL_DOWN) { menu_move(1); cues_post(CUE_MOVE, 0); }
            else if (pressed & PSP_CTRL_UP) { menu_move(-1); cues_post(CUE_MOVE, 0); }
            else if (pressed & PSP_CTRL_CIRCLE) menu_close();
            else if (pressed & PSP_CTRL_CROSS) {
                int chosen = g_row[g_menu_cursor], index = g_menu_of;
                menu_close();
                /* Deleting cannot be undone by pressing the same button
                   again, and a first install is a download worth a look at
                   the size, so both are asked about. */
                if (chosen == CHOICE_DELETE) ask_remove(index);
                else if (chosen == CHOICE_GET && (catalog.apps[index].state == APP_NOT_INSTALLED ||
                                                  catalog.apps[index].state == APP_UPDATE))
                    ask_install(index);
                else if (chosen == CHOICE_RUN) launch_app(index);
                else if (chosen == CHOICE_BASKET) {
                    shell_basket_toggle(index);
                    cues_post(CUE_MOVE, cursor);
                    view_settled(&cursor);
                    count = shell_view_count();
                } else if (chosen == CHOICE_DETAILS) {
                    shell_details(&catalog.apps[index]);
                    details = 1;
                    g_details_from_menu = 1;
                } else {
                    install_app(index, 0, 0, 0);
                    dump_diagnostics();
                    view_settled(&cursor);
                    count = shell_view_count();
                }
            }
        } else if (info) {
            /* The band says and does nothing: what there is to do sits in
               the list behind it, under the gear. */
            if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS)) shell_info(info = 0);
        } else if (details) {
            if (pressed & PSP_CTRL_CIRCLE) {
                shell_details(0);
                details = 0;
                /* Information was read out of the options, so closing it
                   goes back there, on the row it was opened from. */
                if (g_details_from_menu) {
                    g_details_from_menu = 0;
                    menu_open(g_menu_of);
                    g_menu_cursor = row_of(CHOICE_DETAILS);
                    menu_push();
                }
            }
        } else if (count > 0) {
            int at = shell_view_index(cursor);
            if ((pressed & PSP_CTRL_CROSS) && at <= SHELL_ROW_SETTING) {
                /* A row under the gear does what it says. The three that
                   fetch all end in the same place: the list gives way to
                   the word and the status line and comes back with what is
                   now published, the cursor on the package it was on if
                   that package is still there. The sync thread and the
                   media thread share the one HTTPS stack and the one asset
                   buffer, so the media thread steps aside for the length of
                   it, as it does for an install. */
                int which = SHELL_ROW_SETTING - at;
                if (which == 0 && synced) sub_open(SUB_CATALOGS);
                else if (which == 1 && synced) sub_open(SUB_ADD);
                else if (which == 2) { files_open(&g_files); g_files_open = 1; shell_files(&g_files); }
                else if (which == 3) sub_open(SUB_RESET);
                else if (which == 4) shell_info(info = 1);
            } else if (pressed & PSP_CTRL_CROSS) {
                /* X is the one thing there is to do to the package: have
                   it, have the newer one, or start it -- each asked about
                   first. The options, with the same things and the rest,
                   are on triangle. */
                if (at == SHELL_ROW_ACTION) {
                    struct shell_plan plan;
                    shell_action_plan(&plan);
                    if (plan.apps <= 0 && shell_tab_kind() == SHELL_TAB_STICK) {
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
                    shell_ask(title, T_RUN_LINE);
                    g_question = ASK_RUN;
                    g_question_of = at;
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
            if ((pressed & PSP_CTRL_SQUARE) && at == SHELL_ROW_ACTION &&
                shell_tab_kind() == SHELL_TAB_STICK && synced) {
                catalog_force_sources();
                refetch_now(cursor, keep, sizeof(keep), &synced, &refreshing);
            }
            if ((pressed & PSP_CTRL_SQUARE) && at >= 0 &&
                (catalog.apps[at].state == APP_NOT_INSTALLED || shell_basket_has(at))) {
                shell_basket_toggle(at);
                cues_post(CUE_MOVE, cursor);
                view_settled(&cursor);
                count = shell_view_count();
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
    }
    return 0;
}
