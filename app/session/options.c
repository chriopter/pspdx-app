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
#include "session/downloads.h"
#include "gui/osk.h"
#include "session/manage_sources.h"
#include "session/cable.h"
#include "session/launcher.h"
#include "session/options.h"
#include "session/questions.h"
#include "session/view.h"
#include "update/reach.h"
#include "update/sources.h"
#include "update/sync.h"
#include "update/wifi.h"
#include "util/storage.h"

#define SETTINGS_PATH "PSP/PSPDX/settings.txt"

static int g_settings_dirty;
static int g_settings_fps_cap30;
static int g_cable;
static int g_fill_cache = 1;             /* fetch every icon ahead while idle */
static int g_auto_connect;               /* Auto Connect: the connection, 1-10, or 0 */
static int g_runtime_fps_cap30, g_download_mode;
static void apply_fps(void) {
    gfx_set_fps_cap30(g_download_mode ? 0 : g_runtime_fps_cap30);
}
int options_fps_requested(void) { return g_runtime_fps_cap30; }
void options_download_mode(int on) {
    g_download_mode = !!on;
    apply_fps();
}

void options_settings_load(void) {
    char *text = NULL;
    int n = storage_read(storage_path(SETTINGS_PATH), &text, 128);
    /* A line a setting. Missing or invalid settings use Mercy UI at 60 FPS
       and fill the cache when idle; an explicit 30 FPS or fill=0 is kept. */
    g_settings_fps_cap30 = n > 0 && strstr(text, "fps=30\n") == text;
    g_fill_cache = !(n > 0 && strstr(text, "\nfill=0\n"));
    view_sort_set(n > 0 && strstr(text, "\nsort=name\n") ? VIEW_SORT_NAME
                  : n > 0 && strstr(text, "\nsort=installed\n") ? VIEW_SORT_INSTALLED : VIEW_SORT_NEWEST);
    /* How to connect, once it has been answered; no line while it has not. */
    g_cable = n <= 0 ? CABLE_UNASKED : strstr(text, "\ncable=wifi\n") ? CABLE_WIFI
              : strstr(text, "\ncable=usb\n") ? CABLE_USB
              : strstr(text, "\ncable=usb,known\n") ? CABLE_USB_KNOWN : CABLE_UNASKED;
    const char *at = n > 0 ? strstr(text, "\nauto=") : NULL;
    g_auto_connect = at ? atoi(at + 6) : 0;
    if (g_auto_connect < 0 || g_auto_connect > WIFI_PROFILES) g_auto_connect = 0;
    g_runtime_fps_cap30 = g_settings_fps_cap30;
    apply_fps();
    free(text);
    g_settings_dirty = 0;
}

void options_settings_save(void) {
    if (!g_settings_dirty) return;
    static const char *const cable[] = {"", "cable=wifi\n", "cable=usb\n", "cable=usb,known\n"};
    char text[96], auto_line[16] = "";
    if (g_auto_connect) snprintf(auto_line, sizeof(auto_line), "auto=%d\n", g_auto_connect);
    int n = snprintf(text, sizeof(text), "fps=%d\nfill=%d\nsort=%s\n%s%s", g_settings_fps_cap30 ? 30 : 60,
                     g_fill_cache,
                     view_sort() == VIEW_SORT_NAME ? "name" : view_sort() == VIEW_SORT_INSTALLED ? "installed" : "new",
                     cable[g_cable], auto_line);
    if (storage_write(storage_path(SETTINGS_PATH), text, (size_t)n) == 0)
        g_settings_dirty = 0;
}

int options_cable(void) { return g_cable; }
void options_cable_set(int cable) {
    if (cable == g_cable) return;
    g_cable = cable;
    g_settings_dirty = 1;
    options_settings_save();
}

void options_sort_next(void) {
    view_sort_set((view_sort() + 1) % VIEW_SORTS);
    g_settings_dirty = 1;
    options_settings_save();
}

int options_fill_cache(void) { return g_fill_cache; }

int options_auto_connect(void) { return g_auto_connect; }
void options_auto_connect_set(int profile) {
    if (profile < 0 || profile > WIFI_PROFILES || profile == g_auto_connect) return;
    g_auto_connect = profile;
    g_settings_dirty = 1;
    options_settings_save();
}
/* A setting chosen in Options is on the stick at once: a console switched
   off or run flat never passes through the exit that would save it. */
void options_fill_cache_toggle(void) {
    g_fill_cache = !g_fill_cache;
    g_settings_dirty = 1;
    options_settings_save();
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
static int g_menu_download;
int menu_details_index(void) { return g_details_index; }
void menu_details_forget(void) {
    g_details_from_menu = 0;
    g_details_index = -1;
}

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
    memset(g_menu.flag, 0, sizeof(g_menu.flag));
    g_menu_download = downloads_active(index);
    const struct app_entry *entry = &actions_catalog()->apps[index];
    int installed = entry->state != APP_NOT_INSTALLED;
    snprintf(g_menu_title, sizeof(g_menu_title), "%s", entry->name);
    g_menu.title = g_menu_title;
    /* Five rows, the same five for every package, in the same places; what
       a row cannot do to this package it says by being grey. The second
       fetches the package, and says which fetch it would be: Install for
       one not on the stick, Update to the newer version where one waits,
       Reinstall for the one already current. */
    /* A plugin is not run: its first row turns it off, or on again. */
    snprintf(g_choice_text[CHOICE_RUN], sizeof(g_choice_text[0]), "%s",
             strcmp(entry->id, PSPDX_SELF_ID) == 0 ? T_MENU_RESTART
             : strcmp(entry->type, "plugin") ? T_MENU_RUN
             : entry->plugin_off ? T_MENU_PLUGIN_ON : T_MENU_PLUGIN_OFF);
    if (entry->state == APP_UPDATE) {
        /* The row is narrow: twenty bytes of the version, cut between letters. */
        char version[21];
        snprintf(version, sizeof(version), "%s", txt(entry->remote_version));
        pspdx_utf8_mend(version);
        if (catalog_new_build(entry))
            snprintf(g_choice_text[CHOICE_GET], sizeof(g_choice_text[0]), "%s", T_MENU_REBUILD);
        else
            snprintf(g_choice_text[CHOICE_GET], sizeof(g_choice_text[0]), T_MENU_UPDATE, version);
    }
    else
        snprintf(g_choice_text[CHOICE_GET], sizeof(g_choice_text[0]),
                 installed ? T_MENU_REINSTALL : T_MENU_INSTALL);
    if (downloads_active(index)) snprintf(g_choice_text[CHOICE_GET], sizeof(g_choice_text[0]), "Cancel download");
    snprintf(g_choice_text[CHOICE_DELETE], sizeof(g_choice_text[0]), T_MENU_DELETE);
    /* The basket is for what is not on the stick yet, so only such a package
       has the row; one already in the basket keeps it, to come out again.
       The basket is named by its mark, which the shell draws after the
       words. */
    snprintf(g_choice_text[CHOICE_BASKET], sizeof(g_choice_text[0]), "%s\x01%c",
             view_basket_has(index) ? T_MENU_BASKET_OUT : T_MENU_BASKET_IN, (char)(MARK_BASKET + 1));
    snprintf(g_choice_text[CHOICE_DETAILS], sizeof(g_choice_text[0]), T_MENU_INFO);
    /* An ISO is started from the PSP's own game menu, never from here. */
    g_choice_on[CHOICE_RUN] = installed && !downloads_busy();
    /* What cannot be installed yet keeps its row, grey, so the menu has the
       same five rows for every package. */
    g_choice_on[CHOICE_GET] = !entry->unsupported;
    g_choice_on[CHOICE_DELETE] = !downloads_busy() && installed && strcmp(entry->id, PSPDX_SELF_ID) != 0;
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

/* Which version of a game square sets aside: a row each, its flag, disc
   id and size out of the list's summary ("UCES-01059, EU, 1.8 GB"), and
   "In basket" at the end of one that is. X puts it in or takes it out. */
static int g_pick_open, g_pick[VIEW_VERSIONS];
static char g_pick_text[VIEW_VERSIONS][48], g_pick_region[VIEW_VERSIONS][8];

void basket_pick_open(int index) {
    const struct catalog *c = actions_catalog();
    int n = view_versions(index, g_pick, VIEW_VERSIONS);
    if (n > MENU_ROWS) n = MENU_ROWS;
    snprintf(g_menu_title, sizeof(g_menu_title), "%s", T_PICK_VERSION);
    g_menu.title = g_menu_title;
    g_menu.count = n;
    g_menu.cursor = 0;
    for (int i = 0; i < n; i++) {
        const struct app_entry *e = &c->apps[g_pick[i]];
        const char *summary = txt(e->summary), *last = strrchr(summary, ',');
        size_t id = strcspn(summary, ",");
        snprintf(g_pick_text[i], sizeof(g_pick_text[i]), "%.*s%s%s", (int)id, summary,
                 last ? "   " : "", last ? last + 2 : "");
        pspdx_utf8_mend(g_pick_text[i]);
        view_region(e, g_pick_region[i]);
        g_menu.item[i] = g_pick_text[i];
        g_menu.flag[i] = g_pick_region[i];
        g_menu.value[i] = view_basket_has(g_pick[i]) ? T_IN_BASKET : NULL;
        /* What is on the stick has no business in the basket. */
        g_menu.on[i] = e->state == APP_NOT_INSTALLED || view_basket_has(g_pick[i]);
        g_menu.key[i] = -1;
        if (!g_menu.on[g_menu.cursor] && g_menu.on[i]) g_menu.cursor = i;
    }
    g_pick_open = 1;
    menu_push();
}

static void basket_pick_close(void) {
    g_pick_open = 0;
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
        const char *remote = txt(e->remote_version);
        size_t n = strlen(remote);
        int faked = n > 4 && !strcmp(remote + n - 4, "-dev");
        if (on && e->state == APP_CURRENT) {
            char v[VERSION_SIZE];
            snprintf(v, sizeof(v), "%.*s-dev", (int)sizeof(v) - 5, txt(e->local_version));
            text_set(&e->remote_version, v);
            e->state = APP_UPDATE;
        } else if (!on && faked) {
            text_set(&e->remote_version, txt(e->local_version));
            e->state = APP_CURRENT;
        }
    }
    view_tabs_refresh();
}

static void sub_push(void);

void options_fps_toggle_saved(void) {
    int cap30 = !g_runtime_fps_cap30;
    g_runtime_fps_cap30 = cap30;
    apply_fps();
    g_settings_fps_cap30 = cap30;
    g_settings_dirty = 1;
    options_settings_save();
    /* The word has flipped; where the picture does not follow, why. */
    if (cap30 && g_download_mode)
        error_show(T_BAKED_PAUSED, "ui 30 held, %d downloading", downloads_pending_count());
    else if (cap30 && gfx_water_missing())
        error_show(T_BAKED_MISSING, "bake %s", gfx_water_missing());
}

void options_fps_runtime_toggle(void) {
    /* SELECT is a session preview. It deliberately changes only gfx's live
       mode; the value loaded from or chosen for settings remains untouched. */
    g_runtime_fps_cap30 = !g_runtime_fps_cap30;
    apply_fps();
}

/* The source the popup is about, while it is SUB_SOURCE. */
static char g_sub_url[SOURCE_URL];
static int g_sub_source;

/* Auto Connect's rows past Off: which connection each is, and its name. */
static int g_auto_profile[WIFI_PROFILES];
static char g_auto_name[WIFI_PROFILES][64];

static void sub_push(void) {
    g_menu.title = g_sub == SUB_SOURCE ? g_sub_url + sources_web(g_sub_url)
                 : g_sub == SUB_AUTO ? T_SYS_AUTO
                 : g_sub == SUB_RESTORE ? T_SYS_RESET_ALL : T_SUB_DIRECT;
    memset(g_menu.flag, 0, sizeof(g_menu.flag));
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
    if (which == SUB_SOURCE) {
        /* A source: its address put right, its password, its way through
           Tailscale, a test of it, or gone. */
        g_menu.item[g_menu.count++] = T_SOURCE_EDIT;
        g_menu.item[g_menu.count++] = T_SOURCE_PASSWORD;
        g_menu.item[g_menu.count++] = T_SOURCE_TUNNEL;
        g_menu.item[g_menu.count++] = T_SOURCE_TEST;
        g_menu.item[g_menu.count++] = T_SOURCE_DELETE;
    } else if (which == SUB_AUTO) {
        /* Off, then every connection the system has, by its name; the
           one in use says so at its end and the cursor starts on it. */
        g_menu.item[g_menu.count++] = T_SUB_AUTO_OFF;
        for (int p = wifi_profile_next(0), n = 0; p && n < WIFI_PROFILES && g_menu.count < MENU_ROWS;
             p = wifi_profile_next(p), n++) {
            g_auto_profile[n] = p;
            if (wifi_profile_name(p, g_auto_name[n], sizeof(g_auto_name[n])) < 0) break;
            g_menu.item[g_menu.count++] = g_auto_name[n];
        }
        if (g_menu.count == 1) shell_status(T_SYS_NO_NETWORKS);
    } else if (which == SUB_RESTORE) {
        g_menu.item[g_menu.count++] = T_RESET_SETTINGS;
        g_menu.item[g_menu.count++] = T_SYS_SWEEP;
    } else {
        g_menu.item[g_menu.count++] = T_SUB_FROM_GITHUB;
        g_menu.item[g_menu.count++] = T_SUB_FROM_INBOX;
    }
    for (int i = 0; i < g_menu.count; i++) { g_menu.on[i] = 1; g_menu.key[i] = -1; g_menu.value[i] = NULL; }
    if (which == SUB_SOURCE) g_menu.value[2] = sources_tunneled(g_sub_url) ? T_VALUE_ON : T_VALUE_OFF;
    g_menu.cursor = 0;
    if (which == SUB_AUTO) {
        int at = 0;
        for (int i = 1; i < g_menu.count; i++)
            if (g_auto_profile[i - 1] == g_auto_connect) at = i;
        g_menu.value[at] = T_VALUE_ON;
        g_menu.cursor = at;
    }
    sub_push();
}

static void sub_close(void) {
    g_sub = SUB_NONE;
    shell_menu(NULL);
}

void auto_connect_open(void) { sub_open(SUB_AUTO); }

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
    return g_menu_open || g_pick_open;
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
            enum sub was = g_sub;
            sub_close();
            if (was == SUB_AUTO) {
                options_auto_connect_set(chosen > 0 ? g_auto_profile[chosen - 1] : 0);
                cues_post(CUE_MOVE, 0);
            } else if (was == SUB_RESTORE) {
                if (chosen == 0) ask(ASK_RESET, -1, T_RESET_ASK, T_RESET_LINE);
                else sweep_again();
            } else if (was == SUB_SOURCE) {
                if (chosen == 0 && *synced && edit_source(g_sub_url))
                    refetch_now(*cursor, keep, keep_size, synced, refreshing);
                else if (chosen == 1 && *synced) {
                    char password[104];
                    sources_password(g_sub_url, password, sizeof(password));
                    /* Empty takes it away. */
                    if (osk_read(T_OSK_PASSWORD, password, password, sizeof(password)) >= 0 &&
                        sources_password_set(g_sub_url, password) == 0) {
                        shell_status(password[0] ? T_SOURCE_PASSWORD_SET : T_SOURCE_PASSWORD_GONE);
                        refetch_now(*cursor, keep, keep_size, synced, refreshing);
                    }
                } else if (chosen == 4)
                    ask(ASK_CATALOG, g_sub_source, T_SOURCE_DELETE_ASK, g_sub_url + sources_web(g_sub_url));
                else if (chosen == 3 && *synced) source_test(g_sub_url);
                else if (chosen == 2 && *synced) {
                    /* The host from now on through the tunnel, or straight
                       again; fetched anew the way it now goes. */
                    int on = !sources_tunneled(g_sub_url);
                    if (sources_tunneled_set(g_sub_url, on) == 0) {
                        shell_status(on ? T_SOURCE_TUNNEL_ON : T_SOURCE_TUNNEL_OFF);
                        refetch_now(*cursor, keep, keep_size, synced, refreshing);
                    } else shell_status(T_SOURCE_TUNNEL_FULL);
                }
            } else if (chosen == 0 && *synced && type_source(1))
                refetch_now(*cursor, keep, keep_size, synced, refreshing);
            else if (chosen == 1 && *synced) ask_inbox();
        }
    } else if (g_pick_open) {
        if (pressed & PSP_CTRL_DOWN) { menu_move(1); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_UP) { menu_move(-1); cues_post(CUE_MOVE, 0); }
        else if (pressed & PSP_CTRL_CIRCLE) basket_pick_close();
        else if (pressed & PSP_CTRL_CROSS) {
            int version = g_pick[g_menu.cursor];
            basket_pick_close();
            if (g_menu.on[g_menu.cursor]) {
                view_basket_toggle(version);
                cues_post(CUE_MOVE, *cursor);
                view_settled(cursor);
                *count = view_count();
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
            if (chosen == CHOICE_GET && g_menu_download) downloads_cancel(index);
            else if (chosen == CHOICE_DELETE) ask_remove(index);
            else if (chosen == CHOICE_GET && (actions_catalog()->apps[index].state == APP_NOT_INSTALLED ||
                                              actions_catalog()->apps[index].state == APP_UPDATE))
                downloads_enqueue(index);
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
                downloads_enqueue(index);
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
        else if ((pressed & PSP_CTRL_CROSS) && row == SYS_RESET) {
            /* The way back: the settings to their defaults, or the seed's
               renewal, whose sweep runs over the view and leaves it there
               again after. */
            sub_open(SUB_RESTORE);
        }
        else if ((pressed & PSP_CTRL_CROSS) && row == SYS_LAUNCHER) {
            /* ARK's files, not ours: the switch moves only once they say
               so, and what stood in the way is said. */
            int on = !launcher_mode_on();
            int rc = launcher_set(on);
            if (rc == 0) {
                shell_status(on ? T_LAUNCHER_ON : T_LAUNCHER_OFF);
                cues_post(CUE_DONE, 0);
            } else {
                error_show(rc == LAUNCHER_NO_ARK ? T_LAUNCHER_NO_ARK
                           : rc == LAUNCHER_NO_SETTINGS ? T_LAUNCHER_NO_SETTINGS
                           : rc == LAUNCHER_NO_SELF ? T_LAUNCHER_NO_SELF : T_LAUNCHER_FAILED,
                           "launcher %s %d", on ? "on" : "off", rc);
                cues_post(CUE_FAIL, 0);
            }
        }
        else if (pressed & PSP_CTRL_CROSS) {
            /* A value or a switch flips and the view stays, its value or
               tick with it. */
            if (row == SYS_SHOW_FPS) shell_toggle_fps();
            else if (row == SYS_FILL_CACHE) options_fill_cache_toggle();
            else if (row == SYS_DEVELOPER) {
                /* Both or neither: unreleased apps listed and every
                   installed one pretending to have an update. */
                int on = !(shell_dev_updates() && view_unreleased_shown());
                view_show_unreleased(on);
                if (shell_dev_updates() != on) shell_toggle_dev();
                fake_updates(on);
            }
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
        else if ((pressed & (PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE)) && *synced &&
                 ((pressed & PSP_CTRL_CROSS) || g_manage.row[g_manage.cursor].kind == MANAGE_SOURCE)) {
            /* Nothing is added or deleted while a fetch runs: the sources
               it is reading are the ones it was started with. Triangle
               only on a source, where it is what X is. */
            const struct manage_row *row = &g_manage.row[g_manage.cursor];
            g_manage_stale = 1;
            if (row->kind == MANAGE_ADD) {
                if (type_source(0)) refetch_now(*cursor, keep, keep_size, synced, refreshing);
            } else if (row->kind == MANAGE_DIRECT) {
                sub_open(SUB_ADD);
            } else {
                /* A source: edit its address, set its password, or delete it. */
                snprintf(g_sub_url, sizeof(g_sub_url), "%s", manage_url(&g_manage, g_manage.cursor));
                g_sub_source = row->source;
                sub_open(SUB_SOURCE);
            }
        }
    } else return 0;
    return 1;
}
