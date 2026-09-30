#ifndef PSPDX_CLOUD_H
#define PSPDX_CLOUD_H

/* The tags laid out as a cloud: chips of their own widths, as many to a
   line as fit, each line centred, and the way the pad walks them. Nothing
   here draws or knows a font, so the host tests hold it to its rules. */

struct cloud_chip {
    float x, w;             /* from the cloud's left edge */
    short line;             /* counted from 0; its top is line * the line's height */
};

/* n chips of widths w, in their order, into lines no wider than room with
   gap between chips. A chip wider than room stands alone and is cut by
   whoever draws it. Returns the lines made. */
int cloud_layout(struct cloud_chip *chip, int n, const float *w, float room, float gap);

/* Where the pad goes from chip from: dx along the order, a line's end
   onto the next one's start; dy a line up or down, onto the chip there
   whose middle is nearest this one's. Nowhere past the first or the last:
   from itself. */
int cloud_step(const struct cloud_chip *chip, int n, int from, int dx, int dy);

#endif
