#ifndef PSPDX_SCREEN_H
#define PSPDX_SCREEN_H


/* What is left of the text UI: the entropy sweep draws itself on the debug
   screen, and a failed run dumps the log there. Everything else moved to the
   GE shell in gui/shell.h. */

/* pspDebugScreen is an 8x8 font on a 480x272 panel: 60 columns, 34 rows;
   the failure dump stops short of the bottom ones. */
#define SCREEN_COLS 60
#define STATUS_ROW 30

#define COL_TEXT   0xFFFFFFFF
#define COL_DIM    0xFF909090

void gui_init(void);
void gui_clear(void);
void gui_failure(void);

#endif
