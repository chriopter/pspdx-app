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
enum question { ASK_NOTHING, ASK_REMOVE, ASK_ALL, ASK_INBOX, ASK_CATALOG, ASK_RESET, ASK_DISCARD, ASK_RUN, ASK_RESTART, ASK_TRUST, ASK_PLUGIN, ASK_ERROR };

/* Confirm removing a package, fetching the whole tab, or the INBOX. */
void ask_remove(int index);
void ask_all(void);
void ask_inbox(void);

/* One the caller has put into words itself: drawn by the shell and
   remembered here. */
void ask(enum question q, int of, const char *title, const char *line);

/* Something went wrong, and must not pass by as a status line that fades:
   text is what the status line said, tag a printf of what a developer needs
   -- which step failed and its code, "launch 80020149" -- to which the
   version is added. It goes to the log and stands as a band with one answer
   until X; one that comes while another band stands, from a download in the
   background, waits its turn. Notices of things that went well stay status
   lines. */
void error_show(const char *text, const char *tag, ...) __attribute__((format(printf, 2, 3)));
/* The same for the many things that are refused while downloads run. */
void error_busy(void);

/* 1 while a question stands over the browser. */
int asking(void);

/* The catalogs this console reads, as Sources last listed them: the
   view fills it, ASK_CATALOG's index points into it. */
struct sources *question_sources(void);

/* One frame's keys: the restart, whether to turn on a plugin just
   installed, and the certificate doubt are put up first if nothing else is
   asked, then the answer to whatever stands is taken -- X does the thing, O
   leaves it. Returns 1 while a question stood, when
   the keys were its; cursor, count, keep, synced and refreshing are the
   loop's, for the actions the answers lead to. */
int questions_handle(unsigned pressed, int *cursor, int *count, char *keep,
                     size_t keep_size, int *synced, int *refreshing);

#endif
