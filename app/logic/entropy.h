#ifndef PSPDX_ENTROPY_H
#define PSPDX_ENTROPY_H

/* The handshake is X25519 with ChaCha20-Poly1305, so it stands on a 128-bit
   security level: the ephemeral key is exactly as guessable as the entropy
   behind it, and nothing above 128 bits buys any strength. The pool below is
   a 20-byte SHA-1 state and cannot hold more than 160 bits however long the
   stick is swept, so a larger target here would only be a number on screen. */
#define ENTROPY_BITS 128

/* The sweep is scored on an invisible 250x250 field laid over the visible
   grid. Each point has one number, y * 250 + x, and each number pays out
   once: driving over ground already covered is worth nothing, which is what
   keeps a stick held against its stop from filling the bar. */
#define ENTROPY_FIELD_SIDE 250
#define ENTROPY_FIELD_COUNT (ENTROPY_FIELD_SIDE * ENTROPY_FIELD_SIDE)

/* A stroke is not entropy. Driving the source across the grid in one line
   enters a few hundred fields in a second, and all of them follow from where
   the stroke began and which way it went. What the hand chooses is where it
   turns, so a field is credited only when it is new ground reached under a
   heading different from the one at the last credit: eight headings, one per
   45 degrees, and a stroke pays once however long it runs. The full stick
   trace in testdata (sweep-full.trace) turns 123 times in 27 seconds, and
   three turns in four go 45 degrees on from the last heading: measured
   against the heading before it, a turn carries 2.0 bits, and the moment of
   it more. One bit per credit is the conservative half of that. */
#define ENTROPY_HEADINGS 8
#define ENTROPY_BITS_PER_FIELD 1

void entropy_init(void);

/* Every new field reaches the pool. Returns 1 when it was also credited: it
   was reached under a heading (0 to ENTROPY_HEADINGS - 1) other than the one
   at the previous credit. */
int entropy_absorb_field(unsigned int field, unsigned int heading);

/* Runtime noise: absorbed, never counted. The bar is a gate on the first
   handshake, not a running total, and the pool is a 20-byte SHA-1 state that
   is already full at 128 bits -- so what this buys is not strength but
   freshness. PSPDX.SEED is a readable file on the stick, and a copy of it
   otherwise predicts every later boot, because a stored seed skips the sweep.
   Stirring the session full of things the device does not control is what
   makes a stolen copy go stale.

   All of it stays in RAM. The seed is written when the run ends through HOME
   and not before: the same twenty bytes land on the same sector every time,
   and a memory stick has no wear levelling worth the name. A run cut short by
   a flat battery loses the stirring and leaves the seed the startup already
   rotated, which is no worse than before this existed.

   A timestamp is folded in on every call, so a caller may pass the bare value
   it has. */
void entropy_stir(const void *data, unsigned int len);

int entropy_bits(void);
int entropy_load(void);
void entropy_save(int replaying);
void entropy_forget(void);

/* A sweep the browser asked for, which has a pool behind it already: the
   count is set aside and the field starts dry, while the pool keeps what it
   holds and takes the new sweep on top -- the network threads run on through
   it, and a handshake in the middle must not draw from an empty pool. Either
   the new sweep finishes and the count is its own, or it is abandoned and
   entropy_restore puts the old count back. entropy_stashed is what the sweep
   screen asks to know whether there is anything to go back to -- the first
   sweep of a run has nothing, and stays until it is done. */
void entropy_stash(void);
int entropy_stashed(void);
void entropy_restore(void);

int psprandom_seed_raw(unsigned char *seed, unsigned int size);

#endif
