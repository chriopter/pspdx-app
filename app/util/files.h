#ifndef PSPDX_FILES_H
#define PSPDX_FILES_H

#include "gui/shell.h"

/* Manage Files: what PSPDX keeps under PSP/PSPDX, as a person would ask
   about it -- the sources, the installed apps, the inbox, the cache, an
   install in progress, the logs, the encryption seed, the developer's files
   -- each with a sentence saying what it is for. Inside an area the rows
   are named for what they are: an app by its name, a source by its address.
   A file's own bytes are one level further down.

   The view is filled here, out of the stick, and drawn by the shell. */

/* Where a name for an app id can be had beyond the stick's own records: the
   catalog in memory, which main.c holds. NULL or "" from it means no name. */
void files_names(const char *(*name_of)(const char *id));

void files_open(struct file_view *v);
void files_move(struct file_view *v, int by);
void files_scroll(struct file_view *v, int lines);

/* Into the row under the cursor. Returns 1 when there was a level to go
   into, 0 when the row is as deep as it goes. */
int files_enter(struct file_view *v);

/* The bytes of what the row stands for, as they are, without the layout a
   record or a picture is otherwise given. Returns 0 when the row stands for
   nothing that has bytes. */
int files_raw(struct file_view *v);

/* One level up. Returns 0 when there was no level left, and the browser
   should close. */
int files_back(struct file_view *v);

#endif
