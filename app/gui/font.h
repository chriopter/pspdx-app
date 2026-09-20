#ifndef PSPDX_FONT_H
#define PSPDX_FONT_H

/* The PSP's own system font, read off the firmware at flash0:/font. It is the
   single thing that stops the UI looking like a terminal, and it costs
   nothing to ship: the file is already on every PSP. */

enum font_style {
    FONT_DISPLAY,   /* a word standing in the room, rendered once to a texture */
    FONT_H1,        /* the header */
    FONT_TITLE,     /* game/app titles in the list, at the XMB item size */
    FONT_BODY,      /* summaries */
    FONT_META,      /* category, state */
    FONT_CAPTION    /* compact facts below an app description */
};

/* Returns 0 if the firmware font could not be loaded; the caller should then
   stay on the debug screen rather than draw an empty UI. */
int font_init(void);
void font_shutdown(void);

/* y is the baseline, not the top of the glyph box. Returns the x the text
   ended at. */
float font_print(enum font_style style, float x, float y, unsigned color,
                 const char *text);
float font_printf(enum font_style style, float x, float y, unsigned color,
                  const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/* Draws at most width pixels and cuts the rest -- for summaries that do not
   fit their panel. */
float font_print_clipped(enum font_style style, float x, float y, float width,
                         unsigned color, const char *text);

float font_width(enum font_style style, const char *text);

/* The system's way with a line too long for its room: cut off at the room
   while nobody is looking, and once the cursor is on it, after a pause, the
   whole line walks by. age is how long the cursor has been on it; a row
   nobody is on passes 0 and is cut. Returns where the line ends. */
float font_print_scrolling(enum font_style style, float x, float y, float width,
                           unsigned color, const char *text, float age);

#endif
