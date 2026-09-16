#ifndef PSPDX_SHELL_INTERNAL_H
#define PSPDX_SHELL_INTERNAL_H

#include "gui/font.h"
#include "gui/gfx.h"
#include "gui/marks.h"
#include "gui/palette.h"

/* What shell.c shares with the views drawn beside it in gui/, and nothing
   outside gui/ should touch: the frame's palette, where the columns and the
   footer stand, and the chrome every band is made of. */

#define FOOTER_Y 252
#define FOOTER_BASE (FOOTER_Y + 11)     /* the hints' baseline, clear of the edge */

#define LIST_X 16
#define LIST_W 200
#define LIST_Y 44

#define PANEL_X 232

/* The list's rows, and where the package's icon sits in one; the gear's
   rows and Manage sources' are measured the same. */
#define ITEM_H 32
/* The bundle's icon, 144x80 shown at a third: as tall as the row allows
   with a little air, and the name starts after it. */
#define ICON_W 43
#define NAME_X (LIST_X + ICON_W + 9)
#define VISIBLE ((FOOTER_Y - 6 - LIST_Y) / ITEM_H)

/* The right column: as wide as the card, from where the card starts. */
#define SHOT_W 224
#define SHOT_Y 43

#define INFO_Y 36
#define INFO_H (FOOTER_Y - 6 - INFO_Y)

/* The palette of the current frame, derived from the eased tint once per
   frame so every element agrees. */
extern struct rgb g_tint;
extern unsigned g_accent, g_text, g_dim;

/* The same colour, quieter: a mark on a row the eye is not on should be
   read only when it is looked for. */
unsigned faded(unsigned color, int alpha);

/* Under a block of text the floor is dimmed, but with a soft spot and not
   a plate: the lattice runs everywhere, it only gets quieter here. */
void draw_shade(int cx, int cy, int w, int h);

/* A band the full width of the screen, held by a line of light along each
   edge; and a line of the same kind but shorter, for dividing its inside. */
void draw_band(int y, int h);
void band_rule(int y, int half_w, int alpha);

/* One button and the word for what it does, the mark sitting on the text's
   own line. Returns where the next one may start; a row that has to be
   centred is measured with the same arithmetic first. */
float hint_width(enum mark m, const char *text);
float draw_hint(float x, float base, enum mark m, const char *text, unsigned color);

/* How long the cursor has sat on what it sits on: the scrolling of a line
   too long for its room starts over each time it moves. One clock, keyed
   by the list and the row. */
float hover_age(int list, int key);

/* The list's floor and light: the soft shade under `rows` rows and the glow
   of the selection, whose eased top is sel_y; and the scroll bar, for a list
   of count rows scrolled to first, when they are more than fit. */
void draw_rows_light(int rows, float sel_y, float t);
void draw_rows_bar(int count, float first, float t);

/* The gear's right column: a row's name, and under it what taking the row
   comes to, over at most three lines, the name walking from age on when it
   is wider than the column (0 keeps it still). Returns the lines the
   sentence took. */
int draw_setting_note(const char *title, const char *note, float age);

/* 1 while the foot is free for a view's keys: no status line, and no
   question, options or install standing over the screen. */
int shell_footer_free(void);

/* Prints text over at most `lines` lines of `width`, breaking at spaces,
   the last line clipped. Returns the lines used. */
int draw_wrapped(enum font_style style, float x, float y, float width,
                 float step, int lines, unsigned color, const char *text);

/* The card is told to fetch its entry afresh, as if the cursor had just
   arrived on it: what a browser that took the card's place says on
   closing. */
void shell_reshow_card(void);

#endif
