/*
 * The tag cloud's geometry: gui/cloud.h.
 */

#include "gui/cloud.h"

int cloud_layout(struct cloud_chip *chip, int n, const float *w, float room, float gap) {
    int line = 0, first = 0;
    float x = 0;
    for (int i = 0; i <= n; i++) {
        /* A line is closed by the chip that does not fit on it, or by the
           end, and centred in the room then. */
        int closes = i == n || (i > first && x + gap + w[i] > room);
        if (closes && i > first) {
            float shift = (room - x) / 2;
            if (shift < 0) shift = 0;
            for (int k = first; k < i; k++) chip[k].x += shift;
            line++;
            first = i;
            x = 0;
        }
        if (i == n) break;
        chip[i].x = i > first ? x + gap : 0;
        chip[i].w = w[i];
        chip[i].line = (short)line;
        x = chip[i].x + w[i];
    }
    return line;
}

int cloud_step(const struct cloud_chip *chip, int n, int from, int dx, int dy) {
    if (n <= 0 || from < 0 || from >= n) return from < 0 ? 0 : from;
    if (dx) {
        int to = from + (dx > 0 ? 1 : -1);
        return to < 0 || to >= n ? from : to;
    }
    if (!dy) return from;
    int want = chip[from].line + (dy > 0 ? 1 : -1);
    float mid = chip[from].x + chip[from].w / 2;
    int best = from;
    float best_d = 0;
    for (int i = 0; i < n; i++) {
        if (chip[i].line != want) continue;
        float d = chip[i].x + chip[i].w / 2 - mid;
        if (d < 0) d = -d;
        if (best == from || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}
