/* The catalogs PSPDX offers once, in this order: each is added to
   PSP/PSPDX/sources.txt on the first start that sees it, and a source the
   user removes stays removed (update/presets.c). The first line is also
   what the catalog is called before any source has answered. */
#define PSPDX_PRESET_FIRST "https://chriopter.github.io/pspdx-catalog/"
#define PSPDX_PRESETS PSPDX_PRESET_FIRST "\n" \
    "https://pspdev.github.io/homebrew/\n"
