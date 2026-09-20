#ifndef PSPDX_OPTIONS_H
#define PSPDX_OPTIONS_H

#include <stddef.h>

/* The one menu the shell draws, whichever is up: the package's options on
   triangle, or a popup under the gear. Built and walked here, drawn by
   gui/shell.c through the pointer it is handed. What a row leads to is
   session/questions.h's or session/actions.h's. */

/* The package's options: run, fetch, delete, basket, information. */
void menu_open(int index);

/* The one popup under the gear: the two ways a .pspdx comes in directly. */
enum sub { SUB_NONE, SUB_ADD };
void sub_open(enum sub which);

/* A missing or malformed settings file means the 60 FPS
   default. Saves wait for orderly exit, so a menu choice never stalls a
   frame on Memory Stick I/O. */
void options_settings_load(void);
void options_settings_save(void);
/* SELECT's live 30/60 switch. It updates no persisted setting. */
void options_fps_runtime_toggle(void);
int options_fps_requested(void);
void options_download_mode(int on);

/* Sources, the view the gear's first row opens in place of the list: Add
   Source, Direct Install, and a row per source that X deletes, asked
   first. */
void sources_open(void);

/* 1 while the options or a popup stand over the browser; 1 while Sources
   stands in its place. */
int menu_shown(void);
/* Options' first row: the frame-rate mode flipped and remembered. */
void options_fps_toggle_saved(void);

/* Options, the view the gear's second row opens in place of the list:
   the frame rate, the switches for development, the seed's renewal and
   the way back to the defaults. */
void system_open(void);
/* Which package the page opened from the menu is about, -1 for none. */
int menu_details_index(void);
int popup_shown(void);
int sources_shown(void);
/* 1 while any of the gear's views stands in the list's place -- Sources,
   Options, Data -- or About over it: the ones the triggers leave for the
   next tab. Closes whichever is up. */
int gear_view_shown(void);
void gear_views_close(void);

/* Information closed: read out of the options, it goes back there, on the
   row it was opened from. */
void menu_return(void);

/* One frame's keys while the options, a popup or Sources stand: up
   and down walk the rows, O closes, X takes the row, and the keys the menu
   names do their row's thing from inside it. Returns 1 while one stood, when the
   keys were its; cursor, count, keep, synced, refreshing and details are
   the loop's, for what the rows lead to. */
int options_handle(unsigned pressed, int *cursor, int *count, char *keep,
                   size_t keep_size, int *synced, int *refreshing, int *details);

#endif
