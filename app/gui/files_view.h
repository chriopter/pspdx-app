#ifndef PSPDX_FILES_VIEW_H
#define PSPDX_FILES_VIEW_H

#include <pspctrl.h>

#include "util/files.h"

/* Manage Files on screen: what PSPDX keeps on the stick, in two columns
   where the list and the card otherwise stand. The view is filled by
   util/files.c, which reads the stick; this draws it, takes its keys, and
   hands a film or a sound to the media thread while a row stands for one. */

/* Up, on the areas. The gear row opens it; O at the top closes it. */
void files_view_open(void);

/* 1 while the browser is on screen, when the room is its and the list's
   keys are not read. */
int files_view_shown(void);

/* The two columns, or the band over them, drawn where the shell draws the
   list. */
void files_view_draw(float t);

/* The media thread's frame while the browser is up: the film or sound
   under the cursor takes the card's place. Returns 1 when it was the
   browser's frame, 0 when the card's entry is the shell's to sync. */
int files_view_sync(void);

/* The keys, while the browser is up and nothing stands over it. */
void files_view_keys(unsigned pressed, const SceCtrlData *pad);

/* The rows the text comes to where it is shown now (column or band), and
   how many of them are in view at once; a long line cut at the width counts
   as the rows it becomes. */
void shell_files_extent(const struct file_view *v, int *rows, int *room);

#endif
