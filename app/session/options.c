#include "text.h"
/*
 * The one menu the shell draws, whichever is up: the package's options,
 * opened on triangle, and the popups under the gear. The rows are built
 * here and walked here from the pad, in options_handle(); gui/shell.c
 * draws them through the pointer it is handed. What a row leads to is a
 * question, session/questions.c's, or an action, session/actions.c's.
 */

#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/cues.h"
#include "gui/gfx.h"
#include "gui/marks.h"
#include "gui/shell.h"
#include "gui/files_view.h"
#include "gui/sources_view.h"
#include "gui/system_view.h"
#include "session/actions.h"
#include "session/manage_sources.h"
#include "session/options.h"
#include "session/questions.h"
#include "session/view.h"
#include "update/reach.h"
#include "update/sources.h"
#include "update/sync.h"
#include "util/storage.h"

#define SETTINGS_PATH "PSP/PSPDX/settings.txt"

static int g_settings_dirty;
static int g_settings_fps_cap30 = 1;

void options_settings_load(void) {
    char *text = NULL;
    int n = storage_read(storage_path(SETTINGS_PATH), &text, 32);
    /* Be deliberately strict: an interrupted edit or a future format does not
       get to opt old hardware into the more demanding mode. */
    g_settings_fps_cap30 = !(n == 7 && !memcmp(text, "fps=60\n", 7));
    gfx_set_fps_cap30(g_settings_fps_cap30);
    free(text);
    g_settings_dirty = 0;
}

void options_settings_save(void) {
    if (!g_settings_dirty) return;
    const char *text = g_settings_fps_cap30 ? "fps=30\n" : "fps=60\n";
    if (storage_write(storage_path(SETTINGS_PATH), text, 7) == 0)
        g_settings_dirty = 0;
}

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
/* The one menu the shell draws, whichever is up: the package's options, or
   a popup under the gear. */
static struct menu g_menu;
/* The rows the panel is handed: the shown choices, in order. */
static int g_row[CHOICE_COUNT];
static char g_menu_title[48];
static int g_menu_open, g_menu_of, g_details_from_menu, g_details_index = -1;
int menu_details_index(void) { return g_details_index; }

/* The keys that do a row's thing without the menu, named at the row: the
   menu is where they are learned. */
static const signed char g_choice_key[CHOICE_COUNT] = {
    [CHOICE_RUN] = MARK_START, [CHOICE_GET] = -1, [CHOICE_DELETE] = -1,
    [CHOICE_BASKET] = MARK_SQUARE, [CHOICE_DETAILS] = -1,
};

static void menu_push(void) {
    shell_menu(&g_menu);
}

static int row_of(enum choice c) {
    for (int r = 0; r < g_menu.count; r++)
        if (g_row[r] == c) return r;
    return 0;
}

void menu_open(int index) {
    g_details_index = -1;
    const struct app_entry *entry = &actions_catalog()->apps[index];
    int installed = entry->state != APP_NOT_INSTALLED;
    snprintf(g_menu_title, sizeof(g_menu_title), "%s", entry->name);
    g_menu.title = g_menu_title;
    /* Five rows, the same five for every package, in the same places; what
       a row cannot do to this package it says by being grey. The second
       fetches the package, and says which fetch it would be: Install for
       one not on the stick, Update to the newer version where one waits,
       Reinstall for the one already current. */
    snprintf(g_choice_text[CHOICE_RUN], sizeof(g_choice_text[0]), "%s",
             strcmp(entry->id, PSPDX_SELF_ID) == 0 ? T_MENU_RESTART : T_MENU_RUN);
    if (entry->state == APP_UPDATE) {
        /* The row is narrow: twenty bytes of the version, cut between letters. */
        char version[21];
        snprintf(version, sizeof(version), "%s", entry->remote_version);
        pspdx_utf8_mend(version);
        if (catalog_new_build(entry))
            snprintf(g_choice_text[CHOICE_GET], sizeof(g_choice_text[0]), "%s", T_MENU_REBUILD);
        else
            snprintf(g_choice_text[CHOICE_GET], sizeof(g_choice_text[0]), T_MENU_UPDATE, version);
    }
    else
        snprintf(g_choice_text[CHOICE_GET], sizeof(g_choice_text[0]),
                 installed ? T_MENU_REINSTALL : T_MENU_INSTALL);
    snprintf(g_choice_text[CHOICE_DELETE], sizeof(g_choice_text[0]), T_MENU_DELETE);
    /* The basket is for what is not on the stick yet, so only such a package
       has the row; one already in the basket keeps it, to come out again.
       The basket is named by its mark, which the shell draws after the
       words. */
    snprintf(g_choice_text[CHOICE_BASKET], sizeof(g_choice_text[0]), "%s\x01%c",
             view_basket_has(index) ? T_MENU_BASKET_OUT : T_MENU_BASKET_IN, (char)(MARK_BASKET + 1));
    snprintf(g_choice_text[CHOICE_DETAILS], sizeof(g_choice_text[0]), T_MENU_INFO);
    g_choice_on[CHOICE_RUN] = installed;
    /* What cannot be installed yet keeps its row, grey, so the menu has the
       same five rows for every package. */
    g_choice_on[CHOICE_GET] = !entry->unsupported;
    g_choice_on[CHOICE_DELETE] = installed && strcmp(entry->id, PSPDX_SELF_ID) != 0;
    g_choice_on[CHOICE_BASKET] = !entry->unsupported || view_basket_has(index);
    g_choice_on[CHOICE_DETAILS] = 1;
    for (int i = 0; i < CHOICE_COUNT; i++) g_choice_shown[i] = 1;
    g_choice_shown[CHOICE_BASKET] = !installed || view_basket_has(index);
    g_menu.count = 0;
    for (int i = 0; i < CHOICE_COUNT; i++) {
        if (!g_choice_shown[i]) continue;
        g_row[g_menu.count] = i;
        g_menu.item[g_menu.count] = g_choice_text[i];
        g_menu.value[g_menu.count] = NULL;
        g_menu.on[g_menu.count] = g_choice_on[i];
        g_menu.key[g_menu.count] = g_choice_key[i];
        g_menu.count++;
    }
    g_menu.cursor = row_of(entry->state == APP_NOT_INSTALLED || entry->state == APP_UPDATE
                           ? CHOICE_GET : CHOICE_RUN);
    g_menu_of = index;
    g_menu_open = 1;
    menu_push();
}

static void menu_close(void) {
    g_menu_open = 0;
    shell_menu(NULL);
}

/* The one popup under the gear, in the same panel the options use: the
   two ways a .pspdx comes in directly. Drawn by the shell, driven here. */
static enum sub g_sub;

/* Fake updates, on: every package on the stick that is current is said to
   have a newer release waiting, the installed version with "-dev" on it,
   so the update path can be walked -- the stick tab counts them, the
   arrows turn, the rows say "Update to X-dev", and the fetch that follows
   gets the release the catalog really has. Off: the same packages are
   current again. A fetch of the catalog undoes it too. */
static void fake_updates(int on) {
    struct catalog *c = actions_catalog();
    for (int i = 0; i < c->count; i++) {
        struct app_entry *e = &c->apps[i];
        size_t n = strlen(e->remote_version);
        int faked = n > 4 && !strcmp(e->remote_version + n - 4, "-dev");
        if (on && e->state == APP_CURRENT) {
            char v[VERSION_SIZE];
            snprintf(v, sizeof(v), "%.*s-dev", (int)sizeof(v) - 5, e->local_version);
            snprintf(e->remote_version, sizeof(e->remote_version), "%s", v);
            e->state = APP_UPDATE;
        } else if (!on && faked) {
            snprintf(e->remote_version, sizeof(e->remote_version), "%s", e->local_version);
            e->state = APP_CURRENT;
        }
    }
    view_tabs_refresh();
}

static void sub_push(void);

void options_fps_toggle_saved(void) {
    int cap30 = !gfx_fps_cap30();
    gfx_set_fps_cap30(cap30);
    g_settings_fps_cap30 = cap30;
    g_settings_dirty = 1;
}

void options_fps_runtime_toggle(void) {
    /* SELECT is a session preview. It deliberately changes only gfx's live
       mode; the value loaded from or chosen for settings remains untouched. */
    gfx_set_fps_cap30(!gfx_fps_cap30());
}

static void sub_push(void) {
    g_menu.title = T_SUB_DIRECT;
    shell_menu(&g_menu);
}

/* Sources: not a popup but a view in the list's place, with the
   gear's two columns. The rows are session/manage_sources.c's, over the
   list questions.c keeps, since the question that deletes a source points
   into it and is answered after the key that asked it. */
static struct manage_sources g_manage;
static int g_manage_open;
/* What the rows were built under: the loop's synced, and whether an action
   since may have changed sources.txt. Either moving builds them again, so a
   source added or deleted shows at once and how its fetch went once that
   fetch is through. */
static int g_manage_synced, g_manage_stale;

static void manage_reload(void) {
    sources_load(question_sources());
    /* What the last fetch could not load is taken only once that fetch is
       through; while one runs, the rows keep what the one before said. */
    if (sync_done())
        reach_take();
    manage_build(&g_manage, question_sources());
}

void sources_open(void) {
    g_manage.cursor = 0;
    manage_reload();
    /* The gear's row opens this only once a sync is through. */
    g_manage_synced = 1;
    g_manage_stale = 0;
    g_manage_open = 1;
    sources_view_set(&g_manage);
}

int sources_shown(void) {
    return g_manage_open;
}

static void sources_close(void) {
    g_manage_open = 0;
    sources_view_set(NULL);
}

void sub_open(enum sub which) {
    g_sub = which;
    g_menu.count = 0;
    g_menu.item[g_menu.count++] = T_SUB_FROM_GITHUB;
    g_menu.item[g_menu.count++] = T_SUB_FROM_INBOX;
    for (int i = 0; i < g_menu.count; i++) { g_menu.on[i] = 1; g_menu.key[i] = -1; g_menu.value[i] = NULL; }
    g_menu.cursor = 0;
    sub_push();
}

static void sub_close(void) {
    g_sub = SUB_NONE;
    shell_menu(NULL);
}

void system_open(void) {
    system_view_open();
}

int gear_view_shown(void) {
    return g_manage_open || system_view_shown() || files_view_shown() || shell_info_shown();
}

void gear_views_close(void) {
    if (g_manage_open) sources_close();
    system_view_close();
    files_view_close();
    shell_info(0);
}

/* A greyed row is stepped over rather than landed on: the cursor only ever
   sits where X would do something. */
static void menu_move(int by) {
    for (int i = 0; i < g_menu.count; i++) {
        g_menu.cursor = (g_menu.cursor + by + g_menu.count) % g_menu.count;
        if (g_menu.on[g_menu.cursor]) break;
    }
    menu_push();
}

int menu_shown(void) {
    return g_menu_open;
}

int popup_shown(void) {
    return g_sub != SUB_NONE;
}

/* Information closed: read out of the options, it goes back there, on the
   row it was opened from. */
void menu_return(void) {
    if (g_details_from_menu) {
        g_details_from_menu = 0;
        menu_open(g_menu_of);
        g_menu.cursor = row_of(CHOICE_DETAILS);
        menu_push();
    }
}

int options_handle(unsigned pressed, int *cursor, int *count, char *keep,
                   size_t keep_size, int *synced, int *refreshing, int *details) {
    if (g_sub) {
        if (pressed & PSP_CTRL_DOWN) { g_menu.cursor = (g_menu.cursor + 1) % g_menu.count; sub_push(); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_UP) { g_menu.cursor = (g_menu.cursor + g_menu.count - 1) % g_menu.count; sub_push(); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_CIRCLE) sub_close();
        else if (pressed & PSP_CTRL_CROSS) {
            int chosen = g_menu.cursor;
            sub_close();
            if (chosen == 0 && *synced && type_source(1))
                refetch_now(*cursor, keep, keep_size, synced, refreshing);
            else if (chosen == 1 && *synced) ask_inbox();
        }
    } else if (g_menu_open) {
        /* The keys the menu names work from inside it too, so that what
           is read there can be pressed there: square, START and SELECT
           do their row's thing and take the menu with them. */
        int index = g_menu_of;
        if ((pressed & PSP_CTRL_SQUARE) &&
            ((actions_catalog()->apps[index].state == APP_NOT_INSTALLED &&
              !actions_catalog()->apps[index].unsupported) || view_basket_has(index))) {
            menu_close();
            view_basket_toggle(index);
            cues_post(CUE_MOVE, *cursor);
            view_settled(cursor);
            *count = view_count();
        } else if ((pressed & PSP_CTRL_START) &&
                   actions_catalog()->apps[index].state != APP_NOT_INSTALLED) {
            menu_close();
            launch_app(index);
        }
        if (!g_menu_open) { /* taken by one of the keys above */ }
        else if (pressed & PSP_CTRL_DOWN) { menu_move(1); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_UP) { menu_move(-1); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_CIRCLE) menu_close();
        else if (pressed & PSP_CTRL_CROSS) {
            int chosen = g_row[g_menu.cursor], index = g_menu_of;
            menu_close();
            /* Deleting cannot be undone by pressing the same button
               again, and a first install is a download worth a look at
               the size, so both are asked about. */
            if (chosen == CHOICE_DELETE) ask_remove(index);
            else if (chosen == CHOICE_GET && (actions_catalog()->apps[index].state == APP_NOT_INSTALLED ||
                                              actions_catalog()->apps[index].state == APP_UPDATE))
                ask_install(index);
            else if (chosen == CHOICE_RUN) launch_app(index);
            else if (chosen == CHOICE_BASKET) {
                view_basket_toggle(index);
                cues_post(CUE_MOVE, *cursor);
                view_settled(cursor);
                *count = view_count();
            } else if (chosen == CHOICE_DETAILS) {
                shell_details(&actions_catalog()->apps[index]);
                *details = 1;
                g_details_from_menu = 1;
                g_details_index = index;
            } else {
                install_app(index, 0, 0, 0);
                dump_diagnostics();
                view_settled(cursor);
                *count = view_count();
            }
        }
    } else if (system_view_shown()) {
        int row = system_view_cursor();
        if (pressed & PSP_CTRL_DOWN) { system_view_move(1); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_UP) { system_view_move(-1); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_CIRCLE) system_view_close();
        else if ((pressed & PSP_CTRL_CROSS) && row == SYS_SWEEP) {
            /* The seed's renewal: the sweep runs over the view, which is
               there again after. */
            sweep_again();
        }
        else if ((pressed & PSP_CTRL_CROSS) && row == SYS_RESET) {
            ask(ASK_RESET, -1, T_RESET_ASK, T_RESET_LINE);
        }
        else if (pressed & PSP_CTRL_CROSS) {
            /* A value or a switch flips and the view stays, its value or
               tick with it. The frame rate is the one that is remembered. */
            if (row == SYS_FRAME_RATE) options_fps_toggle_saved();
            else if (row == SYS_SHOW_FPS) shell_toggle_fps();
            else { shell_toggle_dev(); fake_updates(shell_dev_updates()); }
            cues_post(CUE_MOVE, 0);
        }
    } else if (g_manage_open) {
        if (g_manage_synced != *synced || g_manage_stale) {
            g_manage_synced = *synced;
            g_manage_stale = 0;
            manage_reload();
        }
        /* A move takes the last result off the status line, as it does in
           the list, so the keys at the foot come back. */
        if (pressed & PSP_CTRL_DOWN) { manage_move(&g_manage, 1); shell_status(""); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_UP) { manage_move(&g_manage, -1); shell_status(""); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_CIRCLE) sources_close();
        else if ((pressed & PSP_CTRL_CROSS) && *synced) {
            /* Nothing is added or deleted while a fetch runs: the sources
               it is reading are the ones it was started with. */
            const struct manage_row *row = &g_manage.row[g_manage.cursor];
            g_manage_stale = 1;
            if (row->kind == MANAGE_ADD) {
                if (type_source(0)) refetch_now(*cursor, keep, keep_size, synced, refreshing);
            } else if (row->kind == MANAGE_DIRECT) {
                sub_open(SUB_ADD);
            } else {
                /* Named without the scheme; every source has it. */
                const char *u = manage_url(&g_manage, g_manage.cursor);
                if (!strncmp(u, "https://", 8)) u += 8;
                ask(ASK_CATALOG, row->source, T_SOURCE_DELETE_ASK, u);
            }
        }
    } else return 0;
    return 1;
}
