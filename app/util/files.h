#ifndef PSPDX_FILES_H
#define PSPDX_FILES_H

/* Manage Files: what PSPDX keeps under PSP/PSPDX, as a person would ask
   about it -- the sources, the installed apps, the inbox, the cache, an
   install in progress, the logs, the encryption seed, the developer's files
   -- each with a sentence saying what it is for. Inside an area the rows
   are named for what they are: an app by its name, a source by its address.
   A file's own bytes are one level further down.

   The view is filled here, out of the stick, and drawn by gui/files_view.c:
   the left column is a list of rows, the right column what the row under
   the cursor comes to: a sentence saying what it is, then its contents as
   lines. */
#define FILES_MAX 96
#define FILES_ROWS 6
#define FILES_TEXT 4096
/* A path on the stick in full: the device, PSP/PSPDX, an area, and a name
   as long as FAT allows. */
#define FILES_PATH 288
struct file_row { char name[40]; char detail[24]; };
enum { FILE_MEDIA_NONE, FILE_MEDIA_PICTURE, FILE_MEDIA_FILM, FILE_MEDIA_SOUND };
struct file_view {
    int level;                          /* 0 the areas, 1 inside one, 2 a file's bytes */
    int area;
    int group;                          /* the system areas are one row on top, opened as a list */
    int deeper;                         /* X opens the row under the cursor */
    int band;                           /* the raw bytes are up, in a band over the columns */
    char head[40];                      /* over the right column */
    char path[FILES_PATH];              /* where on the stick, in full, under it */
    char media[FILES_PATH];             /* a file shown for itself instead of lines, or "" */
    int media_kind;                     /* what it is, one of FILE_MEDIA_* */
    char note[256];                     /* the sentence under it: a manifest's summary at most */
    struct file_row row[FILES_MAX];
    int count, cursor, first;
    char text[FILES_TEXT];              /* the lines under the sentence */
    int text_first, text_lines;
    int raw;                            /* text is a file's bytes: fixed width, wrapped */
};

/* Where a name for an app id can be had beyond the stick's own records: the
   catalog in memory, which main.c holds. NULL or "" from it means no name. */
void files_names(const char *(*name_of)(const char *id));

void files_open(struct file_view *v);
void files_move(struct file_view *v, int by);

/* The rows the text comes to once a raw line is cut at cols: what the
   scrolling counts, since a file of one long line is many rows. */
int files_rows(const struct file_view *v, int cols);

/* The lines scrolled by; rows is what the text comes to at the width it is
   drawn at and room how many of them the last draw held, both the view's
   to say, so that the scrolling stops with the last of the text in view. */
void files_scroll(struct file_view *v, int lines, int rows, int room);

/* Into the row under the cursor. Returns 1 when there was a level to go
   into, 0 when the row is as deep as it goes. */
int files_enter(struct file_view *v);

/* 1 while the cursor is on the one action among the areas, Clear Cache:
   X there is the caller's to ask about and do, not a level to go into. */
int files_action(const struct file_view *v);
/* 1 when row i is that action: drawn under a line, apart from the areas. */
int files_row_is_action(const struct file_view *v, int i);

/* The bytes of what the row stands for, as they are, without the layout a
   record or a picture is otherwise given. Returns 0 when the row stands for
   nothing that has bytes. */
int files_raw(struct file_view *v);

/* One level up. Returns 0 when there was no level left, and the browser
   should close. */
int files_back(struct file_view *v);

#endif
