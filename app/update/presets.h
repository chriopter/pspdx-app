#ifndef PSPDX_PRESETS_H
#define PSPDX_PRESETS_H

/* The catalogs PSPDX ships with: presets.txt beside the EBOOT, or the copy
   in util/self_presets.h when that file is gone or names nothing. Each one
   is added to PSP/PSPDX/sources.txt once, on the first start that sees it,
   and written down in PSP/PSPDX/presets.seen: a source the user removes is
   not put back, and one a newer release brings still arrives.

   Called at startup, before the first catalog fetch reads the file. boot is
   the EBOOT's own path, argv[0]. Returns how many sources were added; a
   line that is not a source, or a stick that will not write, is logged and
   passed over. */
int presets_merge(const char *boot);

#endif
