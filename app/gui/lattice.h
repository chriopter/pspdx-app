#ifndef PSPDX_LATTICE_H
#define PSPDX_LATTICE_H

#include "gui/palette.h"

/* The backdrop: a water surface in perspective, rows running to a horizon.
   A height field lives on the crossings -- crests catch the light, troughs
   go dark, and a glint wanders across whatever slope happens to face the
   horizon. Nothing in here knows what a catalog is; it takes a colour, a
   time, and where something landed.

   The same surface is the entropy sweep's field. There it starts dry and a
   source fills it, so the browser stands on the water the sweep built. */

void lattice_init(void);

/* A drop falls under screen position x (0..1 across the floor), and the ring
   it leaves runs outward on its own. */
void lattice_touch(float x);

/* A hand in the water: the stick's position, both -1..1, pressed into
   the surface every frame it is held off centre. The point wanders with
   the stick and every frame leaves a dent, so a stick swung about makes a
   wake of rings crossing rings. Harder the further the stick is pushed. */
void lattice_stir(float x, float y);

void lattice_draw(float t, struct rgb tint);
/* The light burning on the horizon: where it is, across, and how much of it
   the horizon keeps this frame, 0 to 1. The shell takes the rest, to draw
   the same light somewhere nearer, and gives it back. */
float lattice_light_x(void);
void lattice_horizon(float keep);
/* How far the room is swaying this frame, the slow lean the water is drawn
   with: what a thing standing in the room turns with. */
float lattice_sway(void);

/* A picture lying in the water: what is on the card, drawn again on the
   surface below it. px and pw are the card's left edge and its width in
   pixels, bottom the pixel its picture ends on and ph its height; alpha is
   how far the picture itself has faded in, so a still crossing into a film
   crosses in the water too. Called after lattice_draw, from where the card
   is drawn -- the water is already down by then, and this goes on top of
   it. Anything the card does not stand over takes nothing. */
void lattice_mirror(const struct gfx_texture *t, int alpha,
                    float px, float bottom, float pw, float ph);

/* The colour the room is heading for. Call it from shell_draw where draw_lot
   picks the target for a new selection, with the colour it drew: the water
   then turns from the last touch point outward, a front running with the
   ring, old colour outside it and new in, the far corners about a second and
   a half later. It is only ever told because lattice_draw is handed the
   eased colour, which says nothing about where it is going; left uncalled,
   the surface watches that colour instead and starts a front of its own the
   frame it jumps, and carries the colour in as it arrives: the same front in
   the same place at the same speed, only its first few frames are faint,
   since what it is carrying then is still most of the way the old colour. */
void lattice_tint(struct rgb target);

/* The sweep. dry() empties the field. pour() puts the source at (fx, fz) --
   across, and into the distance, both 0..1 -- and while it is pouring wets
   what is under it and dents the surface; it returns how much of the field
   is water. settle() ends the sweep: the source goes and the last of the
   field fills, so the browser never stands on dry ground. */
void lattice_dry(void);
float lattice_pour(float fx, float fz, int pouring);
void lattice_settle(void);
/* A burst of falling stars over the sweep, count at a time. */
void lattice_shower(int count);

#endif
