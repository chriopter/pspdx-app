#include "text.h"
/*
 * The questions that stand over the browser before anything writes to the
 * stick: put into words here, drawn by the shell, answered through the pad
 * in questions_handle(), which then calls the action -- session/actions.c
 * -- the answer was for.
 */

#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

#include "gui/preview.h"
#include "gui/shell.h"
#include "install/state.h"
#include "pspkit-https/https.h"
#include "session/actions.h"
#include "session/questions.h"
#include "session/view.h"
#include "update/inbox.h"
#include "util/storage.h"

/* Nothing that writes to the stick starts on one press any more. The shell
   draws the question and the footer that answers it; the answer arrives
   through the pad, which is read down in the loop, so the two halves meet
   in these two variables and nowhere else. */
static enum question g_question;
static int g_question_of;

/* The catalogs this console reads, as the popup under the gear last listed
   them. ASK_CATALOG's index points into this list, and the answer comes
   after the popup has gone, so the list is kept with the question. */
static struct sources g_sources;

struct sources *question_sources(void) {
    return &g_sources;
}

void ask_install(int index) {
    const struct app_entry *entry = &actions_catalog()->apps[index];
    const char *version = entry->remote_version[0] ? entry->remote_version
                                                   : entry->release.version;
    char title[64], line[200];
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
    /* A release that names another directory than the one installed is
       said so, after the version: the app is going to live elsewhere. A
       name that differs only in case is the same directory to the stick
       (and to PPSSPP, which once wrote pspdx where PSPDX was meant). */
    if (recorded && strcasecmp(previous.dir, entry->release.dir)) {
        size_t at = strlen(line);
        snprintf(line + at, sizeof(line) - at, T_INSTALL_MOVES, previous.dir, entry->release.dir);
    }
    shell_ask(title, line);
    g_question = ASK_INSTALL;
    g_question_of = index;
}

void ask_remove(int index) {
    const struct app_entry *entry = &actions_catalog()->apps[index];
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
void ask_all(void) {
    struct view_plan plan;
    view_action_plan(&plan);
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

void ask_inbox(void){
    preview_quiesce();catalog_offline(https_net_connect()<0);
    shell_word(T_WORD_INBOX);
    int n=inbox_scan(actions_catalog());preview_resume();view_rebuild(actions_catalog());
    if(n<=0){shell_status(T_INBOX_EMPTY);return;}
    char title[64],line[96];snprintf(title,sizeof(title),T_INBOX_ASK,n);
    snprintf(line,sizeof(line),"%s",inbox_summary());shell_ask(title,line);g_question=ASK_INBOX;
}

/* A question the caller has put into words itself: drawn by the shell and
   remembered here, for questions_handle() to answer. */
void ask(enum question q, int of, const char *title, const char *line) {
    shell_ask(title, line);
    g_question = q;
    g_question_of = of;
}

int asking(void) {
    return g_question != ASK_NOTHING;
}

int questions_handle(unsigned pressed, int *cursor, int *count, char *keep,
                     size_t keep_size, int *synced, int *refreshing) {
    /* PSPDX has just replaced itself. The copy running is the old one
       until the console loads the new, so that is offered as soon as
       nothing else is being asked. */
    if (g_question == ASK_NOTHING) {
        char version[32];
        int of = restart_take(version, sizeof(version));
        if (of >= 0) {
            char line[64];
            snprintf(line, sizeof(line), T_RESTART_LINE, version);
            shell_ask(T_RESTART_ASK, line);
            g_question = ASK_RESTART;
            g_question_of = of;
        }
    }

    /* A handshake turned down on a doubt -- a run-out certificate, an
       issuer not carried -- is put to the person, once, as soon as
       nothing else is being asked: a console that has lain in a drawer
       still has to connect, on their word. */
    if (g_question == ASK_NOTHING) {
        char host[128];
        enum https_doubt d = https_doubt_take(host, sizeof(host));
        if (d != HTTPS_DOUBT_NONE) {
            char line[200];
            snprintf(line, sizeof(line), d == HTTPS_DOUBT_EXPIRED ? T_TRUST_EXPIRED : T_TRUST_ISSUER, host);
            shell_ask(T_TRUST_ASK, line);
            g_question = ASK_TRUST;
            g_question_of = -1;
        }
    }

    if (g_question == ASK_NOTHING) return 0;
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
        else if (asked == ASK_DISCARD) clear_cache();
        else if (asked == ASK_RUN || asked == ASK_RESTART) launch_app(index);
        else if (asked == ASK_TRUST) {
            https_doubt_accept();
            refetch_now(*cursor, keep, keep_size, synced, refreshing);
        }
        else if (asked == ASK_CATALOG) {
            if (index >= 0 && index < g_sources.count &&
                sources_remove(g_sources.url[index]) > 0)
                refetch_now(*cursor, keep, keep_size, synced, refreshing);
            else shell_status(T_SOURCE_DELETE_FAILED);
        } else uninstall_app(index);
        dump_diagnostics();
        /* What was just done can have emptied a tab. */
        view_settled(cursor);
        *count = view_count();
    } else if (pressed & PSP_CTRL_CIRCLE) {
        int later = g_question == ASK_RESTART, declined = g_question == ASK_TRUST;
        ask_forget();
        shell_status(later ? T_RESTART_LATER : declined ? T_TRUST_DECLINED : "");
    }
    return 1;
}
