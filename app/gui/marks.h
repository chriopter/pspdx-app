#ifndef PSPDX_MARKS_H
#define PSPDX_MARKS_H

/* The glyph set: the four face buttons, the badges a row can carry, and one
 * icon per tab. The system font has none of these, and spelling TRIANGLE out
 * is longer than the word it would be labelling.
 *
 * Each glyph is a bitmap drawn at the size it is shown at -- eleven pixels
 * for a button, fifteen for a tab -- which is the only way a two-pixel stroke
 * stays two pixels. The bitmaps live in assets/marks as PNG and reach the
 * build through marks_data.h; see the root README, "Regenerating icons and EBOOT media".
 *
 * Sizes are not uniform and must be asked for: a face button is 11x11, a
 * tab 15x15, START and SELECT are 17x7 pills, L and R are 13x9, the Memory
 * Stick 9x13 and the play triangle 9x11.
 */

enum mark {
    MARK_CROSS, MARK_CIRCLE, MARK_TRIANGLE, MARK_SQUARE,
    MARK_TICK, MARK_UPDATE, MARK_BASKET, MARK_STICK, MARK_PLAY,
    MARK_DOWNLOAD, MARK_INFO,
    MARK_ALL, MARK_GAMES, MARK_DEMOS, MARK_APPS, MARK_EMULATORS, MARK_PLUGINS,
    MARK_START, MARK_SELECT, MARK_L, MARK_R, MARK_HOME, MARK_GEAR,
    MARK_COUNT
};

/* How lit a mark is, which is the whole of the highlight language the XMB
 * uses: the glyph never changes shape or brightness between states, only what
 * is behind it does.
 *
 * MARK_DIM    flat, no shadow and no light: a mark on a row the eye is not on.
 * MARK_PLAIN  the glyph over a dark soft copy of itself, so it holds its edge
 *             against whatever it is standing on.
 * MARK_LIT    plain, with a wide soft light behind it in the room's colour.
 *
 * The colour is always the caller's -- grey for a dim mark, white or the
 * accent for the rest. State says how the mark is lit, not what colour it is.
 */
enum mark_state { MARK_DIM, MARK_PLAIN, MARK_LIT };

int mark_width(enum mark m);
int mark_height(enum mark m);

/* cx, cy are the centre in screen pixels; colour is ABGR as everywhere else,
 * and its own alpha fades the whole mark, shadow with it. tint is the colour
 * the light behind a lit mark takes, and t is the clock in seconds, which is
 * all the pulse in that light needs. */
void mark_draw(enum mark m, float cx, float cy, unsigned color, int state,
               unsigned tint, float t);

#endif
