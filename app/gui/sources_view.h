#ifndef PSPDX_SOURCES_VIEW_H
#define PSPDX_SOURCES_VIEW_H

#include "session/manage_sources.h"

/* Manage sources on screen: the rows on the left where the gear's list
   stands, and on the right what the row under the cursor is, in the gear's
   own words-and-sentence column. The model is session/manage_sources.h's,
   walked by session/options.c and drawn from here through the pointer it
   is handed, for as long as it is up. NULL takes the view down. */
void sources_view_set(const struct manage_sources *m);

/* 1 while the view is on screen. */
int sources_view_shown(void);

/* The two columns, drawn where the shell draws the list. */
void sources_view_draw(float t);

#endif
