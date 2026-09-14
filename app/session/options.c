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
#include <string.h>

#include "audio/cues.h"
#include "gui/marks.h"
#include "gui/shell.h"
#include "session/actions.h"
#include "session/options.h"
#include "session/questions.h"
#include "session/view.h"
#include "update/sources.h"

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
static int g_menu_open, g_menu_of, g_details_from_menu;

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

/* The two popups under the gear, in the same panel the options use: the
   catalogs this console reads, one a row with "Add" last; and the two ways
   a .pspdx comes in directly. Drawn by the shell, driven here. */
static enum sub g_sub;
static char g_sub_short[SOURCES_MAX][48];

static void sub_push(void) {
    g_menu.title = g_sub == SUB_CATALOGS ? T_SUB_SOURCES : g_sub == SUB_ADD ? T_SUB_DIRECT : T_SUB_RESET;
    shell_menu(&g_menu);
}

void sub_open(enum sub which) {
    g_sub = which;
    g_menu.count = 0;
    if (which == SUB_CATALOGS) {
        sources_load(question_sources());
        for (int i = 0; i < question_sources()->count; i++) {
            /* The scheme goes; every source has it, and the panel is narrow. */
            const char *u = question_sources()->url[i];
            if (!strncmp(u, "https://", 8)) u += 8;
            snprintf(g_sub_short[i], sizeof(g_sub_short[i]), "%s", u);
            g_menu.item[g_menu.count++] = g_sub_short[i];
        }
        g_menu.item[g_menu.count++] = T_SUB_ADD_SOURCE;
    } else if (which == SUB_ADD) {
        g_menu.item[g_menu.count++] = T_SUB_FROM_GITHUB;
        g_menu.item[g_menu.count++] = T_SUB_FROM_INBOX;
    } else {
        g_menu.item[g_menu.count++] = T_SUB_RESET_ALL;
        g_menu.item[g_menu.count++] = T_SUB_CLEAR_CACHE;
    }
    for (int i = 0; i < g_menu.count; i++) { g_menu.on[i] = 1; g_menu.key[i] = -1; }
    g_menu.cursor = 0;
    sub_push();
}

static void sub_close(void) {
    g_sub = SUB_NONE;
    shell_menu(NULL);
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
            enum sub kind = g_sub;
            sub_close();
            if (kind == SUB_CATALOGS) {
                if (chosen < question_sources()->count) {
                    ask(ASK_CATALOG, chosen, T_SOURCE_DELETE_ASK, g_sub_short[chosen]);
                } else if (*synced && type_source(0)) {
                    refetch_now(*cursor, keep, keep_size, synced, refreshing);
                }
            } else if (kind == SUB_ADD) {
                if (chosen == 0 && *synced && type_source(1))
                    refetch_now(*cursor, keep, keep_size, synced, refreshing);
                else if (chosen == 1 && *synced) ask_inbox();
            } else {
                if (chosen == 0) {
                    ask(ASK_RESET, -1, T_RESET_ASK, T_RESET_LINE);
                } else {
                    ask(ASK_DISCARD, -1, T_DISCARD_ASK, T_DISCARD_LINE);
                }
            }
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
            } else {
                install_app(index, 0, 0, 0);
                dump_diagnostics();
                view_settled(cursor);
                *count = view_count();
            }
        }
    } else return 0;
    return 1;
}
