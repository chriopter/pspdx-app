#include "text.h"
/*
 * What the session does to the stick and the catalog: installs, one or a
 * run of them, removing, starting a package, fetching the catalog again,
 * the sweep, the cache and the reset. Each is a call that holds the loop
 * until it is over; the shell draws what it says meanwhile. The questions
 * that stand before them are session/questions.c's; the options that
 * lead to them, session/options.c's.
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio.h"
#include "audio/cues.h"
#include "gui/entropy_screen.h"
#include "gui/gfx.h"
#include "gui/osk.h"
#include "gui/preview.h"
#include "gui/shell.h"
#include "install/install.h"
#include "install/state.h"
#include "pspkit-https/entropy.h"
#include "pspkit-https/https.h"
#include "session/actions.h"
#include "session/options.h"
#include "session/view.h"
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
static char g_restart_version[VERSION_SIZE];

int restart_take(char *version, size_t size) {
    int of = g_restart_of;
    if (of < 0) return -1;
    snprintf(version, size, "%s", g_restart_version);
    g_restart_of = -1;
    return of;
}

int install_app(int index, int screenshot, int at, int of) {
    if (index < 0 || index >= g_catalog->count) return -1;
    /* A plugin or an ISO is listed and not installed, and that is said
       before anything is fetched. */
    if (g_catalog->apps[index].unsupported) {
        char message[96];
        snprintf(message, sizeof(message), T_INSTALL_UNSUPPORTED, g_catalog->apps[index].name);
        logline("%s", message);
        shell_status(message);
        cues_post(CUE_FAIL, 0);
        return -1;
    }
    int row = view_row(index);
    if (row < 0) row = 0;
    struct app_entry *entry = &g_catalog->apps[index];
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
    catalog_offline(https_net_connect()<0);
    int rc = entry->has_release ? catalog_prepare(entry) : -1;
    if(rc==0)rc=install_release(&entry->release, &report, shell_install_phase,
                             install_progress, NULL);
    unsigned seconds = (now_ms() - start) / 1000;
    sceKernelChangeThreadPriority(self, 0x20);
    preview_resume();

    /* A name and a version of 64 characters whole; the status line cuts it
       to its own room, between letters. */
    char message[sizeof(entry->name) + VERSION_SIZE + 64];
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
        } else {
            /* Said, since the next check will not find the author's words
               there either. */
            snprintf(message, sizeof(message),
                     entry->no_pspdx == PSPDX_FROM_FILE         ? T_INSTALLED_FROM_FILE
                     : entry->no_pspdx == PSPDX_FROM_REPOSITORY ? T_INSTALLED_NO_PSPDX
                                                                : T_INSTALLED,
                     entry->name, report.version);
        }
        pspdx_utf8_mend(message);
        logline("installed %s %s: %d files, %luK, %us", entry->name, report.version,
                report.files, (unsigned long)(report.bytes / 1024), seconds);
    } else if (rc == INSTALL_CANCELLED) {
        snprintf(message, sizeof(message), T_CANCELLED, entry->name);
    } else if (rc == INSTALL_NO_SPACE) {
        /* Tenths, rounded up: what is said to be needed has to be enough. */
        unsigned long long tenths = (report.needed * 10 + (1u << 20) - 1) >> 20;
        snprintf(message, sizeof(message), T_NO_SPACE, entry->name, (unsigned long)(tenths / 10),
                 (unsigned long)(tenths % 10));
    } else {
        snprintf(message, sizeof(message), T_INSTALL_FAILED, entry->name, rc);
    }
    shell_install_end(message);
    cues_post(rc == 0 ? CUE_DONE : CUE_FAIL, 0);
    shell_draw(g_catalog, row);
    if (screenshot) screenshot_settled(row, storage_path("PSP/PSPDX/DEBUG/PSPDX2.BMP"));
    return rc;
}

void uninstall_app(int index) {
    struct app_entry *entry = &g_catalog->apps[index];
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
    } else if (rc == INSTALL_SELF) {
        snprintf(message, sizeof(message), "%s", T_SELF_DELETE);
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
    const struct app_entry *entry = &g_catalog->apps[index];
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
    /* LoadExec replaces this process without running the HOME exit callback.
       Persist a changed graphics mode here as well, beside the other session
       state that must survive a normal app launch. */
    options_settings_save();
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

/* The answer to ASK_ASIDE: one rename, then the install it was asked for.
   The new name is a bare name because that is what sceIoRename takes for
   its second argument, the same way the installer moves its own directories. */
int set_aside(int index) {
    const char *dir = g_catalog->apps[index].release.dir;
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

/* The action row taken: everything the tab holds, one after another, in the
   order it is listed. The rows are read into a list before the first fetch --
   an install moves the entry's state, and on the updates tab that takes the
   row out from under a loop still walking the view. A failure is said and the
   rest still go: one package the server has lost is not a reason to leave the
   others unfetched. The basket keeps what did not arrive and lets go of what
   did. */
void install_all(void) {
    int list[MAX_APPS], n = 0;
    for (int row = 0; row < view_count() && n < MAX_APPS; row++) {
        int at = view_index(row);
        if (at < 0) continue;
        const struct app_entry *entry = &g_catalog->apps[at];
        if (!entry->has_release || !entry->release.size || entry->unsupported) continue;
        /* On the stick the job is the updates alone. */
        if (view_tab_kind() == VIEW_TAB_STICK && entry->state != APP_UPDATE) continue;
        list[n] = at;
        n++;
    }
    for(int i=0;i<n-1;i++)if(!strcmp(g_catalog->apps[list[i]].id,PSPDX_SELF_ID)){int self=list[i];memmove(list+i,list+i+1,(n-i-1)*sizeof(int));list[n-1]=self;break;}
    int done = 0;
    for (int i = 0; i < n; i++) {
        SceCtrlData pad;sceCtrlPeekBufferPositive(&pad,1);if(pad.Buttons&PSP_CTRL_CIRCLE)break;
        if (install_app(list[i], 0, i + 1, n) == 0) {
            view_basket_forget(list[i]);
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

/* The unattended install for the rig: PSPDX.INSTALL on the stick names
   a repository, as a URL or as owner/repo, and it is asked at the origin
   like one typed on the gear tab. Returns its index in the catalog. */
int auto_install_index(void) {
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
    int was = view_index(cursor);
    snprintf(keep, keep_size, "%s", was >= 0 ? g_catalog->apps[was].id : "");
    preview_quiesce();
    shell_word(T_WORD_CHECKING);
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
    preview_quiesce();
    int bad = storage_remove_tree(storage_path("PSP/PSPDX/CACHE/catalogs")) < 0;
    bad |= storage_remove_tree(storage_path("PSP/PSPDX/CACHE/media")) < 0;
    sceIoMkdir(storage_path("PSP/PSPDX/CACHE"), 0777);
    sceIoMkdir(storage_path("PSP/PSPDX/CACHE/catalogs"), 0777);
    sceIoMkdir(storage_path("PSP/PSPDX/CACHE/media"), 0777);
    char line[128];
    if (storage_exists(storage_path("PSP/PSPDX/TMP/transaction.json")) &&
        install_discard(line, sizeof(line)) < 0)
        bad = 1;
    preview_resume();
    shell_status(bad ? T_CACHE_CLEAR_FAILED : T_CACHE_CLEARED);
}

/* Back to the first start: everything this client keeps on the stick goes,
   and the client with it, so that what comes up next is a first start --
   the sweep, the built-in list, nothing installed as far as it knows. The
   apps under PSP/GAME are not its to remove. */
void reset_completely(void) {
    log_dump();
    if (storage_remove_tree(storage_path("PSP/PSPDX")) < 0)
        logline("reset: some of PSP/PSPDX would not go");
    sceKernelExitGame();
}

/* Reads a source from the keyboard and adds it. Returns 1 when the catalog
   should be fetched again, 0 when there is nothing new. */
int type_source(int install) {
    char text[SOURCE_URL], url[SOURCE_URL];
    int rc = osk_read(install ? T_OSK_GITHUB : T_OSK_SOURCE, "", text, sizeof(text));
    if (rc <= 0 || !text[0]) return 0;
    rc = sources_normalize(text,url,sizeof(url));
    struct source_repo parsed;
    if(rc<0 || (install && !sources_parse_repo(url,&parsed))) {shell_status(T_BAD_ADDRESS);return 0;}
    preview_quiesce();catalog_offline(https_net_connect()<0);
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
        shell_status(why ? line : T_SOURCE_UNAVAILABLE);
        return 0;
    }
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

void install_inbox(void){
    int count=inbox_count(),done=0;
    for(int i=0;i<count;i++){
        SceCtrlData pad;sceCtrlPeekBufferPositive(&pad,1);if(pad.Buttons&PSP_CTRL_CIRCLE)break;
        int at=inbox_index(i);
        if(install_app(at,0,i+1,count)==0){inbox_installed(i);done++;}
    }
    /* One app's own line -- installed, from the user's file, or why not --
       says more than "1 of 1". */
    if (count != 1) {
        char message[96];
        snprintf(message, sizeof(message), T_INBOX_DONE, done, count);
        shell_status(message);
    }
}
