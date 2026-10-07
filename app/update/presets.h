#ifndef PSPDX_PRESETS_H
#define PSPDX_PRESETS_H

/* The catalogs PSPDX ships with: the list in util/self_presets.h, which
   the program carries. Each one is added to PSP/PSPDX/sources.txt once, on
   the first start that sees it, and written down in PSP/PSPDX/presets.seen:
   a source the user removes is not put back, and one a newer release brings
   still arrives. Releases up to 1.1.4 also put the list beside the EBOOT as
   presets.txt; that file is no longer read, or the one an old release left
   there would hide what a newer program brings.

   Called at startup, before the first catalog fetch reads the file. Returns
   how many sources were added; a line that is not a source, or a stick that
   will not write, is logged and passed over. */
int presets_merge(void);

/* The same for a list given as text, a source a line: what presets_merge
   does with its own, for the tests to say what a newer release would
   bring. A list that names nothing is the built-in one. */
int presets_offer(const char *text);

#endif
