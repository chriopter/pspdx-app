#ifndef PSPDX_OPTIONS_H
#define PSPDX_OPTIONS_H

#include <stddef.h>

/* The one menu the shell draws, whichever is up: the package's options on
   triangle, or a popup under the gear. Built and walked here, drawn by
   gui/shell.c through the pointer it is handed. What a row leads to is
   session/questions.h's or session/actions.h's. */

/* The package's options: run, fetch, delete, basket, information. */
void menu_open(int index);

/* The popups under the gear: the two ways a .pspdx comes in directly, and
   the resets. */
enum sub { SUB_NONE, SUB_ADD, SUB_RESET, SUB_QUIRKS, SUB_GRAPHICS };
void sub_open(enum sub which);

/* A missing or malformed settings file means the hardware-safe 30 FPS
   default. Saves wait for orderly exit, so a menu choice never stalls a
   frame on Memory Stick I/O. */
void options_settings_load(void);
void options_settings_save(void);
/* SELECT's live 30/60 switch. It updates no persisted setting. */
void options_fps_runtime_toggle(void);

/* Manage sources, the view the gear's first row opens in place of the
   list: Add source, and a row per source that X deletes, asked first. */
void sources_open(void);

/* 1 while the options or a popup stand over the browser; 1 while Manage
   sources stands in its place. */
int menu_shown(void);
int popup_shown(void);
int sources_shown(void);

/* Information closed: read out of the options, it goes back there, on the
   row it was opened from. */
void menu_return(void);

/* One frame's keys while the options, a popup or Manage sources stand: up
   and down walk the rows, O closes, X takes the row, and the keys the menu
   names do their row's thing from inside it. Returns 1 while one stood, when the
   keys were its; cursor, count, keep, synced, refreshing and details are
   the loop's, for what the rows lead to. */
int options_handle(unsigned pressed, int *cursor, int *count, char *keep,
                   size_t keep_size, int *synced, int *refreshing, int *details);

#endif
