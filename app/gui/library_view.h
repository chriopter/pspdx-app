#ifndef PSPDX_LIBRARY_VIEW_H
#define PSPDX_LIBRARY_VIEW_H

/* The library on screen: the games on the stick as the UMD grid showed
   discs -- their ICON0s five to a line across the room, the chosen one
   grown a little and named under the grid -- in three sections, each under
   its word. Walked by main.c through session/library.h, drawn here where
   the shell draws the list. */
void library_view_draw(int cursor, float t);

#endif
