#ifndef PSPDX_SYSTEM_VIEW_H
#define PSPDX_SYSTEM_VIEW_H

/* Options on screen: the gear's two columns, the rows on the left -- the
   frame rate with its value at the row's end, the two switches with their
   ticks, the seed's renewal, the way back to the defaults -- and on the
   right what the row under the cursor comes to. Walked by session/options.c,
   drawn here for as long as it is up. */

enum system_row { SYS_FRAME_RATE, SYS_SHOW_FPS, SYS_FAKE_UPDATES, SYS_SWEEP, SYS_RESET, SYS_COUNT };

void system_view_open(void);
void system_view_close(void);
int system_view_shown(void);

/* Where the cursor is, and its move. */
int system_view_cursor(void);
void system_view_move(int by);

/* The two columns, drawn where the shell draws the list. */
void system_view_draw(float t);

#endif
