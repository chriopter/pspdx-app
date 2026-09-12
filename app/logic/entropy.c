#include "util/storage.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdlib.h>

#include "logic/entropy.h"

#define POOL_BYTES 20
#define SEED_FILE storage_path("PSP/PSPDX/CRYPTO/seed.bin")

static unsigned char pool[POOL_BYTES];
static unsigned int pool_counter;
static int pool_bits;
static unsigned char field_seen[(ENTROPY_FIELD_COUNT + 7) / 8];
static unsigned int last_heading = ~0u;

/* sync runs the network on its own thread while the shell keeps the main one,
   and SELECT starts a fresh sweep from there: two threads reach the pool, and
   since entropy_stir writes the seed as it goes, two could reach the file. The
   lock is taken per operation, never across a sweep, so the handshake is never
   left waiting on a hand at the stick. */
static SceUID pool_sema = -1;

static void pool_lock(void) {
    if (pool_sema >= 0) sceKernelWaitSema(pool_sema, 1, NULL);
}

static void pool_unlock(void) {
    if (pool_sema >= 0) sceKernelSignalSema(pool_sema, 1);
}

static void pool_absorb(const void *data, unsigned int len) {
    unsigned char buf[POOL_BYTES + 64];
    unsigned int n = len > 64 ? 64 : len;
    memcpy(buf, pool, POOL_BYTES);
    memcpy(buf + POOL_BYTES, data, n);
    sceKernelUtilsSha1Digest(buf, POOL_BYTES + n, pool);
}

static void pool_absorb_jitter(int rounds) {
    for (int i = 0; i < rounds; i++) {
        unsigned t0 = sceKernelGetSystemTimeLow();
        unsigned spins = 0;
        while (sceKernelGetSystemTimeLow() - t0 < 500) spins++;
        pool_absorb(&spins, sizeof(spins));
    }
}

void entropy_init(void) {
    if (pool_sema < 0) pool_sema = sceKernelCreateSema("entropy", 0, 1, 1, NULL);
    unsigned t = sceKernelGetSystemTimeLow();
    void *sp = &t;
    pool_lock();
    pool_absorb(&t, sizeof(t));
    pool_absorb(&sp, sizeof(sp));
    pool_absorb_jitter(8);
    pool_bits = 0;
    last_heading = ~0u;
    memset(field_seen, 0, sizeof(field_seen));
    pool_unlock();
}

int entropy_absorb_field(unsigned int field, unsigned int heading) {
    if (field >= ENTROPY_FIELD_COUNT) return 0;
    unsigned int byte = field >> 3;
    unsigned char bit = (unsigned char)(1u << (field & 7));
    pool_lock();
    if (field_seen[byte] & bit) {
        pool_unlock();
        return 0;
    }
    field_seen[byte] |= bit;

    /* Every new field goes in: the position, the heading, and the moment,
       since when the point is reached is unpredictable too and the pool is
       happy to take it. Only a turn is counted, so the tally stays honest. */
    struct {
        unsigned int field;
        unsigned int heading;
        unsigned int sys;
    } sample;
    sample.field = field;
    sample.heading = heading;
    sample.sys = sceKernelGetSystemTimeLow();
    pool_absorb(&sample, sizeof(sample));
    int credited = heading != last_heading;
    if (credited) {
        last_heading = heading;
        pool_bits += ENTROPY_BITS_PER_FIELD;
    }
    pool_unlock();
    return credited;
}

void entropy_stir(const void *data, unsigned int len) {
    unsigned int sys = sceKernelGetSystemTimeLow();
    pool_lock();
    pool_absorb(data, len);
    pool_absorb(&sys, sizeof(sys));
    pool_unlock();
}

int entropy_bits(void) { return pool_bits; }

int entropy_load(void) {
    char *stored=NULL;
    int n=storage_read(SEED_FILE,&stored,POOL_BYTES);
    if(n!=POOL_BYTES){free(stored);return 0;}
    pool_lock();
    pool_absorb(stored, POOL_BYTES);
    free(stored);
    pool_absorb_jitter(4);
    pool_bits = ENTROPY_BITS;
    pool_unlock();
    return 1;
}

/* Called three times in a run at most: once the pool is ready, again if SELECT
   throws it away, and once on the way out through HOME. A replayed sweep is
   public input and never reaches the stick. */
void entropy_save(int replaying) {
    if (replaying) return;
    unsigned char next[POOL_BYTES];
    unsigned int tag = 0x50535058;
    unsigned char buf[POOL_BYTES + sizeof(tag)];
    pool_lock();
    memcpy(buf, pool, POOL_BYTES);
    memcpy(buf + POOL_BYTES, &tag, sizeof(tag));
    sceKernelUtilsSha1Digest(buf, sizeof(buf), next);
    /* The write stays inside the lock: released here, two threads could be in
       this file at once, and a half-written seed still reads back as twenty
       bytes and is taken for a full pool on the next run. A short wait on the
       stick is the cheaper end of that trade. */
    storage_write(SEED_FILE,next,sizeof(next));
    pool_unlock();
}

/* The browser's own re-sweep, which the first sweep of a run is not: there is
   a full pool already, and the screen it opens cannot be finished in under
   half a minute of stick work. Somebody who opens it by accident -- it is one
   row of the info band -- has to be able to leave, and leaving may not hand
   the session a pool of nothing to make its keys from. So the old field is
   set aside rather than destroyed, and put back if the sweep is abandoned.
   The seed on the stick still goes at once, as it always did: somebody asking
   for this wants that file gone, and whichever pool the session ends with
   writes a new one on the way out. */
static unsigned char stash[POOL_BYTES];
static int stash_bits = -1;

void entropy_stash(void) {
    pool_lock();
    memcpy(stash, pool, POOL_BYTES);
    stash_bits = pool_bits;
    pool_unlock();
    entropy_forget();
}

int entropy_stashed(void) { return stash_bits >= 0; }

void entropy_restore(void) {
    pool_lock();
    memcpy(pool, stash, POOL_BYTES);
    pool_bits = stash_bits;
    pool_unlock();
    stash_bits = -1;
}

void entropy_forget(void) {
    pool_lock();
    /* Inside the lock, or an automatic save from the network thread lands
       between the removal and the clearing and puts the file straight back. */
    storage_remove(SEED_FILE);
    pool_bits = 0;
    last_heading = ~0u;
    memset(pool, 0, sizeof(pool));
    memset(field_seen, 0, sizeof(field_seen));
    pool_unlock();
}

int psprandom_seed_raw(unsigned char *seed, unsigned int size) {
    pool_lock();
    while (size > 0) {
        unsigned char buf[POOL_BYTES + sizeof(unsigned int)];
        unsigned char out[POOL_BYTES];
        memcpy(buf, pool, POOL_BYTES);
        memcpy(buf + POOL_BYTES, &pool_counter, sizeof(pool_counter));
        sceKernelUtilsSha1Digest(buf, sizeof(buf), out);
        pool_counter++;
        unsigned n = size < sizeof(out) ? size : (unsigned)sizeof(out);
        memcpy(seed, out, n);
        seed += n;
        size -= n;
        pool_absorb(out, sizeof(out));
    }
    pool_unlock();
    return 0;
}
