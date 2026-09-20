/* Mirrors app/presets.txt, the list the release puts next to the EBOOT;
   verified by the host tests. The copy on the stick is read first, this one
   stands in when it is missing or names nothing. The first line is also
   what the catalog is called before any source has answered. */
#define PSPDX_PRESET_FIRST "https://chriopter.github.io/pspdx-catalog/"
#define PSPDX_PRESETS PSPDX_PRESET_FIRST "\n" \
    "https://pspdev.github.io/homebrew/\n"
