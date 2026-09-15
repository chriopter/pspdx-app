#include "gui/wrap.h"

/* The next character of UTF-8 after the one at s: its lead byte and as many
   continuation bytes as that lead announces, and only those that are there.
   A byte that leads nothing -- a stray continuation, a lead UTF-8 does not
   have -- is a character of its own, so text that is not UTF-8 still makes
   characters of four bytes at the most, and a line within its byte cap. */
static const char *next_char(const char *s) {
    unsigned char c = (unsigned char)*s++;
    int more = c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : 0;
    while (more-- > 0 && (*s & 0xC0) == 0x80)
        s++;
    return s;
}

/* Whether bytes from line to end still make a line: within the byte cap,
   which is asked first so the measure never sees more, and within width. */
static int fits(const char *line, const char *end, float width, size_t max_bytes,
                wrap_measure measure, void *ctx) {
    size_t len = (size_t)(end - line);
    return len <= max_bytes && measure(ctx, line, len) <= width;
}

int wrap_text(const char *text, float width, size_t max_bytes, wrap_measure measure,
              void *ctx, struct wrap_line *lines, int max) {
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        const char *line = p, *end = p, *at = p;
        /* Word by word, each measured with everything before it on the line:
           the width of a line is the font's to say, not a sum of pieces. */
        for (;;) {
            const char *word = at;
            while (*word == ' ')
                word++;
            if (!*word || *word == '\n')
                break;
            const char *after = word;
            while (*after && *after != ' ' && *after != '\n')
                after++;
            if (fits(line, after, width, max_bytes, measure, ctx)) {
                end = at = after;
                continue;
            }
            /* Nothing fitted before it: the word is wider than the line, and
               as much of it goes on as fits, one character at the least, so
               that every line takes something and the loop ends. */
            if (end == line) {
                end = next_char(line);
                while (end < after && fits(line, next_char(end), width, max_bytes, measure, ctx))
                    end = next_char(end);
            }
            break;
        }
        lines[n].start = (unsigned short)(line - text);
        lines[n].len = (unsigned short)(end - line);
        n++;
        /* The spaces a line broke at go with it; a newline ends it, and the
           spaces after one are the next line's own. */
        p = end;
        while (*p == ' ')
            p++;
        if (*p == '\n')
            p++;
    }
    return n;
}
