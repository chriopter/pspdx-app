#ifndef PSPDX_WRAP_H
#define PSPDX_WRAP_H

#include <stddef.h>

/* Text broken into lines no wider than a width, once, and drawn from the
   lines on every frame after: measuring a description of 2500 characters
   is thousands of glyphs, which a frame at 60 Hz has no room for. Nothing
   here draws or knows a font, so the host tests hold it to its rules. */

/* One line: where it starts in the text and how many bytes it takes. A
   text is at most some kilobytes, so both fit in sixteen bits. */
struct wrap_line {
    unsigned short start, len;
};

/* How wide len bytes of text, starting at text, would be drawn. Never asked
   about more than the max_bytes wrap_text was given. */
typedef float (*wrap_measure)(void *ctx, const char *text, size_t len);

/* text broken into at most max lines of at most width and max_bytes each: a
   newline always ends a line, and an empty line stays one; otherwise a line
   ends at the last space that lets it fit, and the spaces it ended at are
   dropped. A word wider than the line on its own is cut between characters,
   never inside one, and a line holds one character at least -- four bytes
   at the most, past max_bytes only when max_bytes is smaller than that. The
   text is UTF-8 and at most 65535 bytes; a byte that is not part of a whole
   character counts as one of its own. Returns the lines made. */
int wrap_text(const char *text, float width, size_t max_bytes, wrap_measure measure,
              void *ctx, struct wrap_line *lines, int max);

#endif
