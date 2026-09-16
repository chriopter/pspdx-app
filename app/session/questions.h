#ifndef PSPDX_QUESTIONS_H
#define PSPDX_QUESTIONS_H

#include <stddef.h>

#include "update/sources.h"

/* Nothing that writes to the stick starts on one press. The shell draws
   the question and the footer that answers it; the answer arrives through
   the pad, which the loop reads and hands to questions_handle(), and only
   then the thing that was asked about -- an action, session/actions.h's. */

/* Which question stands, and what it is about: a catalog index for the
   ones about a package, a row of the sources list for ASK_CATALOG, -1 for
   the rest. */
enum question { ASK_NOTHING, ASK_INSTALL, ASK_ASIDE, ASK_REMOVE, ASK_ALL, ASK_INBOX, ASK_CATALOG, ASK_RESET, ASK_DISCARD, ASK_RUN, ASK_RESTART, ASK_TRUST };

/* The questions that put themselves into words: fetching one package (or
   parking what is in its way first), removing it, fetching the whole tab,
   and the INBOX. */
void ask_install(int index);
void ask_remove(int index);
void ask_all(void);
void ask_inbox(void);

/* One the caller has put into words itself: drawn by the shell and
   remembered here. */
void ask(enum question q, int of, const char *title, const char *line);

/* 1 while a question stands over the browser. */
int asking(void);

/* The catalogs this console reads, as Manage sources last listed them: the
   view fills it, ASK_CATALOG's index points into it. */
struct sources *question_sources(void);

/* One frame's keys: the restart and the certificate doubt are put up first
   if nothing else is asked, then the answer to whatever stands is taken --
   X does the thing, O leaves it. Returns 1 while a question stood, when
   the keys were its; cursor, count, keep, synced and refreshing are the
   loop's, for the actions the answers lead to. */
int questions_handle(unsigned pressed, int *cursor, int *count, char *keep,
                     size_t keep_size, int *synced, int *refreshing);

#endif
