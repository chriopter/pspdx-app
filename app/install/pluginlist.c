#include "install/pluginlist.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* One line of the list: where it starts and ends and where the next begins
   -- past its line end, CR LF counted as one -- and, for a line that names
   a plugin, where its path and its on or off stand. */
struct line {
    size_t start, end, next;
    int plugin;
    size_t path, path_len, word, word_len;
};

/* [*a, b) without the spaces around it. A tab or a CR would be whitespace
   to ARK too, but either has ended the line before it gets here. */
static size_t trim(const char *t, size_t *a, size_t b) {
    while (*a < b && t[*a] == ' ')
        (*a)++;
    while (b > *a && t[b - 1] == ' ')
        b--;
    return b - *a;
}

static int comment(const char *t, size_t at, size_t end) {
    return t[at] == ';' || t[at] == '#' || (t[at] == '/' && at + 1 < end && t[at + 1] == '/');
}

/* The line at at, 0 past the last. ARK stops reading at a NUL, so nothing
   after one is a line. */
static int line_at(const char *t, size_t len, size_t at, struct line *l) {
    if (at >= len || !t[at])
        return 0;
    size_t end = at;
    while (end < len && (unsigned char)t[end] >= ' ')
        end++;
    memset(l, 0, sizeof(*l));
    l->start = at;
    l->end = end;
    l->next = end == len ? end : !t[end] ? len
            : end + 1 + (t[end] == '\r' && end + 1 < len && t[end + 1] == '\n');
    size_t a = at, b = a + trim(t, &a, end);
    if (a == b || comment(t, a, b))
        return 1;
    const char *first = memchr(t + a, ',', b - a);
    const char *second = first ? memchr(first + 1, ',', (size_t)(t + b - first - 1)) : NULL;
    if (!second)
        return 1;
    /* A comment ends the line only once all three fields have begun: a
       path may hold a #. */
    size_t stop = (size_t)(second + 1 - t);
    while (stop < b && t[stop] != ',' && !comment(t, stop, b))
        stop++;
    l->path = (size_t)(first + 1 - t);
    l->path_len = trim(t, &l->path, (size_t)(second - t));
    l->word = (size_t)(second + 1 - t);
    l->word_len = trim(t, &l->word, stop);
    l->plugin = 1;
    return 1;
}

static int names(const char *t, const struct line *l, const char *path) {
    const char *slash = strrchr(path, '/');
    /* The folder the list is in, for a path that names no device. */
    size_t beside = memchr(t + l->path, ':', l->path_len) || !slash ? 0
                                                                   : (size_t)(slash + 1 - path);
    return l->plugin && strlen(path) == beside + l->path_len &&
           !strncasecmp(path + beside, t + l->path, l->path_len);
}

static int says_on(const char *t, const struct line *l) {
    static const char *const on[] = {"on", "1", "true", "enabled"};
    for (unsigned i = 0; i < sizeof(on) / sizeof(*on); i++)
        if (strlen(on[i]) == l->word_len && !strncasecmp(t + l->word, on[i], l->word_len))
            return 1;
    return 0;
}

int pluginlist_state(const char *text, size_t len, const char *path) {
    struct line l;
    int state = -1;
    for (size_t at = 0; line_at(text, len, at, &l); at = l.next)
        if (names(text, &l, path))
            state = says_on(text, &l);
    return state;
}

/* The longest last field of a line that is PSPDX's own: " on " and a few
   spaces more, should a hand have added them. */
#define OWN_FIELD 8

/* Whether [a, b) is the last field of an own line, 1 for on and 0 for off:
   the one word with nothing but spaces around it. -1 for anything else. */
static int own_field(const char *t, size_t a, size_t b) {
    size_t n = b - a, k = trim(t, &a, b);
    if (n < 3 || n > OWN_FIELD)
        return -1;
    return k == 2 && !memcmp(t + a, "on", 2) ? 1 : k == 3 && !memcmp(t + a, "off", 3) ? 0 : -1;
}

/* PSPDX's own line for path, by its bytes: "always, <path>," from the
   line's first byte and then its last field to the line's end. field is
   where that begins. 1 with the one there is, 0 with none, -1 with more
   than one, of which none can be told to be PSPDX's. */
static int own_line(const char *t, size_t len, const char *path, struct line *own, size_t *field) {
    char head[PLUGINLIST_BYTES];
    int k = snprintf(head, sizeof(head), "always, %s,", path), found = 0;
    struct line l;
    /* A list with a NUL in it is not one to write to. */
    if (k < 0 || k >= (int)sizeof(head) || memchr(t, 0, len))
        return 0;
    for (size_t at = 0; line_at(t, len, at, &l); at = l.next) {
        if (l.end - l.start < (size_t)k || memcmp(t + l.start, head, (size_t)k) ||
            own_field(t, l.start + (size_t)k, l.end) < 0)
            continue;
        if (found++)
            return -1;
        *own = l;
        *field = l.start + (size_t)k;
    }
    return found;
}

int pluginlist_add(const char *text, size_t len, const char *path, struct pluginlist_write *w) {
    /* The line end the list's last finished line has. */
    const char *eol = "\n";
    for (size_t i = len; i > 0; i--)
        if (text[i - 1] == '\n') {
            if (i > 1 && text[i - 2] == '\r')
                eol = "\r\n";
            break;
        }
    int open = len && text[len - 1] != '\n' && text[len - 1] != '\r';
    int n = snprintf(w->bytes, sizeof(w->bytes), "%salways, %s, on %s", open ? eol : "", path, eol);
    if (n < 0 || n >= (int)sizeof(w->bytes) || strpbrk(path, ", ") || memchr(text, 0, len))
        return -1;
    w->at = len;
    w->n = (size_t)n;
    return 0;
}

int pluginlist_switch(const char *text, size_t len, const char *path, int on,
                      struct pluginlist_write *w) {
    struct line l;
    size_t field;
    if (own_line(text, len, path, &l, &field) != 1)
        return -1;
    /* The word in the room there is, a space before it where it fits: over
       " on" goes "off", over " off" goes " on ". */
    const char *word = on ? "on" : "off";
    w->at = field;
    w->n = l.end - field;
    memset(w->bytes, ' ', w->n);
    memcpy(w->bytes + (w->n > strlen(word)), word, strlen(word));
    return memcmp(text + field, w->bytes, w->n) != 0;
}

int pluginlist_blank(const char *text, size_t len, const char *path, struct pluginlist_write *w) {
    struct line l;
    size_t field;
    if (own_line(text, len, path, &l, &field) != 1)
        return -1;
    w->at = l.start;
    w->n = l.end - l.start;
    memset(w->bytes, ' ', w->n);
    return 1;
}
