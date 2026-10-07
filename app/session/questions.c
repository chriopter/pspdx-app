#include "text.h"
/*
 * The questions that stand over the browser before anything writes to the
 * stick: put into words here, drawn by the shell, answered through the pad
 * in questions_handle(), which then calls the action -- session/actions.c
 * -- the answer was for.
 */

#include <pspctrl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gui/preview.h"
#include "gui/shell.h"
#include "install/state.h"
#include "pspkit-https/https.h"
#include "session/actions.h"
#include "session/downloads.h"
#include "session/questions.h"
#include "session/view.h"
#include "update/inbox.h"
#include "util/runtime.h"
#include "util/storage.h"
#include "util/version.h"

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

/* What went wrong and has not been read yet, the oldest first. A handful:
   more than that at once is one cause, said often enough by its first. */
#define ERRORS 4
static struct { char text[200], detail[96]; } g_errors[ERRORS];
static int g_error_count;

void error_show(const char *text, const char *tag, ...) {
    char detail[96];
    va_list ap;
    va_start(ap, tag);
    int n = vsnprintf(detail, sizeof(detail), tag, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(detail)) n = sizeof(detail) - 1;
    snprintf(detail + n, sizeof(detail) - (size_t)n, "%sPSPDX %s", n ? "  " : "", PSPDX_VERSION);
    logline("error: %s [%s]", text, detail);
    /* Said once while it waits: the same refusal pressed for again. */
    for (int i = 0; i < g_error_count; i++)
        if (!strcmp(g_errors[i].text, text) && !strcmp(g_errors[i].detail, detail)) return;
    if (g_error_count == ERRORS) {
        /* The one on screen stays; the oldest behind it makes room. */
        memmove(&g_errors[1], &g_errors[2], (ERRORS - 2) * sizeof(*g_errors));
        g_error_count--;
    }
    snprintf(g_errors[g_error_count].text, sizeof(g_errors[0].text), "%s", text);
    snprintf(g_errors[g_error_count].detail, sizeof(g_errors[0].detail), "%s", detail);
    g_error_count++;
}

void error_busy(void) {
    error_show("Wait for Downloads, or cancel them first", "busy %d", downloads_pending_count());
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
        error_show(T_SELF_DELETE, "remove self");
        return;
    }
    if (db_read(entry->id, &record) < 0 || (!record.dir[0] && !record.plugin[0])) {
        /* Without a record there is no directory to name, and nothing here
           guesses at one. */
        error_show(T_NO_RECORD, "remove %s", entry->id);
        return;
    }
    char title[sizeof(entry->name) + 32], line[96];
    snprintf(title, sizeof(title), T_REMOVE_ASK, entry->name);
    snprintf(line, sizeof(line), "%s", record.plugin[0] ? T_PLUGIN_OFF_LINE : T_REMOVE_LINE);
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
    if (downloads_busy()) { error_busy(); return; }
    downloads_reset();
    preview_quiesce();catalog_offline(https_net_connect()<0);
    shell_word(T_WORD_INBOX);
    int n=inbox_scan(actions_catalog());preview_resume();view_rebuild(actions_catalog());
    if(n<=0){shell_status(T_INBOX_EMPTY);return;}
    char title[64],line[96];snprintf(title,sizeof(title),T_INBOX_ASK,n,n==1?"":"s");
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
        /* Twenty bytes of the version at most, and never half a letter. */
        char version[21];
        int of = restart_take(version, sizeof(version));
        if (of >= 0) {
            char line[64];
            pspdx_utf8_mend(version);
            snprintf(line, sizeof(line), T_RESTART_LINE, version);
            shell_ask(T_RESTART_ASK, line);
            g_question = ASK_RESTART;
            g_question_of = of;
        }
    }

    /* A plugin has gone in for the first time. It is on the stick and
       off, and nothing is written to PLUGINS.TXT unless the answer is yes. */
    if (g_question == ASK_NOTHING) {
        int of = plugin_installed_take();
        if (of >= 0) {
            char title[sizeof(actions_catalog()->apps[of].name) + 32];
            snprintf(title, sizeof(title), T_PLUGIN_INSTALLED_ASK, actions_catalog()->apps[of].name);
            pspdx_utf8_mend(title);
            shell_ask(title, T_PLUGIN_TURN_ON_LINE);
            g_question = ASK_PLUGIN;
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

    /* What went wrong, once nothing else is asked: it stands until X. */
    if (g_question == ASK_NOTHING && g_error_count) {
        shell_ask_error(g_errors[0].text, g_errors[0].detail);
        g_question = ASK_ERROR;
        g_question_of = -1;
    }

    if (g_question == ASK_NOTHING) return 0;
    if (g_question == ASK_ERROR) {
        /* Read: O does as well as X, there being nothing to choose. */
        if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)) {
            g_error_count--;
            memmove(&g_errors[0], &g_errors[1], (size_t)g_error_count * sizeof(*g_errors));
            ask_forget();
        }
        return 1;
    }
    /* The answer, and only then the thing that was asked about. */
    if (pressed & PSP_CTRL_CROSS) {
        enum question asked = g_question;
        int index = g_question_of;
        ask_forget();
        if (asked == ASK_ALL) install_all();
        else if (asked == ASK_INBOX) install_inbox();
        else if (asked == ASK_RESET) reset_completely();
        else if (asked == ASK_DISCARD) clear_cache();
        else if (asked == ASK_RUN || asked == ASK_RESTART) launch_app(index);
        else if (asked == ASK_PLUGIN) switch_plugin(index);
        else if (asked == ASK_TRUST) {
            https_doubt_accept();
            refetch_now(*cursor, keep, keep_size, synced, refreshing);
        }
        else if (asked == ASK_CATALOG) {
            if (downloads_busy()) { error_busy(); return 1; }
            if (index >= 0 && index < g_sources.count &&
                sources_remove(g_sources.url[index]) > 0)
                refetch_now(*cursor, keep, keep_size, synced, refreshing);
            else error_show(T_SOURCE_DELETE_FAILED, "source remove %d", index);
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
