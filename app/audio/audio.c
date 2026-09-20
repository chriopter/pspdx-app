#include <pspaudio.h>
#include <pspatrac3.h>
#include <pspkernel.h>
#include <psputility.h>
#include <psputility_avmodules.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio.h"
#include "audio/music.h"
#include "audio/synth.h"
#include "util/runtime.h"
#include "video/player.h"

#define RATE 44100
#define SYNTH_RATE (RATE / 4)

/* PSPSDK's audio library and samples use 1024 frames. That is 23.2 ms at the
   PSP hardware rate, short enough that a late block is not heard as a long
   repeat. Audio must outrank the default 0x20 main thread: rendering it below
   the interface let a busy GE submission delay PCM production past its real
   deadline. One step above main gives the feed precedence without the old
   library's very aggressive 0x12 priority. */
#define CHUNK 1024
#define AUDIO_PRIORITY 0x1f
#define AUDIO_STACK (16 * 1024)
#define SOUND_PRIORITY 0x22
#define SOUND_STACK (16 * 1024)

static int g_up;
static int g_channel = -1;
static SceUID g_thread = -1;
static SceUID g_snd_thread = -1;
static volatile int g_quit, g_paused;
void audio_pause(int on) { g_paused = !!on; }
static volatile unsigned g_worst_us;
static volatile unsigned g_snd_worst_us;
static volatile unsigned g_output_worst_us;
static volatile unsigned g_output_frames;
static volatile unsigned g_deadlines;
static volatile unsigned g_snd_underflows;
static volatile unsigned g_output_errors;
static volatile int g_output_last_error;
static short __attribute__((aligned(64))) g_buf[2][CHUNK * 2];
static short g_synth_low[(CHUNK / 4) * 2];
static short g_synth_carry[2];
static int g_synth_primed;

/* ------------------------------------------------------ the card's sound */

/* Under the notes the list plays and well under the tune's own peaks: the
   SND0 is atmosphere for the card, not a track. */
#define SOUND_LEVEL 0.30f
#define SOUND_IN_S 0.3f
#define SOUND_OUT_S 0.2f

/* One decode of ATRAC3 is 1024 samples a channel and of ATRAC3plus 2048;
   the ring holds four of the larger frames, giving the output thread about
   186 ms of reserve without making the decoder touch its buffers. */
#define SOUND_FRAME 2048
#define SOUND_RING 8192

/* The hand-over. Play and stop leave the bytes here under the lock and
   move seq on; the audio thread notices at its next chunk, takes them
   under the same lock, and does everything else on its own, so neither
   caller ever waits on a decode. The lock is held for a pointer swap. */
static SceUID g_snd_lock = -1;
static void *g_snd_next;
static size_t g_snd_next_len;
static volatile unsigned g_snd_seq, g_snd_taken;

/* Decoder thread only from here on. sceAtracDecodeData occasionally takes a
   whole output deadline on hardware, so it must never run on the thread that
   feeds the channel. The decoder stays one priority below audio and fills a
   single-producer/single-consumer ring while both higher-priority threads are
   asleep. Four decoded frames are 186 ms of reserve. */
static void *g_snd_buf;                 /* the AT3; sceAtrac reads it in place */
static size_t g_snd_len;
static int g_snd_id = -1;
static int g_snd_modules;               /* AVCODEC and ATRAC3PLUS are loaded */
static short __attribute__((aligned(64))) g_snd_frame[SOUND_FRAME * 2];
static short __attribute__((aligned(64))) g_snd_ring[SOUND_RING * 2];
static volatile unsigned g_snd_read, g_snd_write;
static volatile unsigned g_snd_want, g_snd_ready;
static volatile int g_snd_ready_on;
static float g_snd_gain, g_snd_target;
static int g_snd_loops, g_snd_decoded;  /* times round, and samples this time */
static volatile int g_snd_on;           /* audio_duck reads this from the interface */

static inline void publish(void) {
    __asm__ volatile("" ::: "memory");
}

static void sound_close(void) {
    if (g_snd_id >= 0) {
        sceAtracReleaseAtracID(g_snd_id);
        logline("sound: off after %d times round", g_snd_loops);
    }
    g_snd_id = -1;
    free(g_snd_buf);
    g_snd_buf = 0;
    g_snd_len = 0;
}

static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned le32(const unsigned char *p) { return le16(p) | (le16(p + 2) << 16); }

/* Opens the AT3 the thread has just taken. What sceAtrac cannot say is
   read off the RIFF header first: the sample rate, which has to be the
   channel's, and the channel count, which has to be two -- the stub set
   here has no way to ask what a mono file comes out as, and no SND0 has
   ever been mono. */
static int sound_open(void) {
    const unsigned char *h = g_snd_buf;
    if (g_snd_len < 36 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0 ||
        memcmp(h + 12, "fmt ", 4) != 0) {
        logline("sound: not a RIFF");
        return -1;
    }
    unsigned tag = le16(h + 20), channels = le16(h + 22), rate = le32(h + 24);
    if (rate != RATE || channels != 2) {
        logline("sound: %u Hz, %u channels; wants %d Hz stereo", rate, channels, RATE);
        return -1;
    }
    int id = sceAtracSetDataAndGetID(g_snd_buf, (SceSize)g_snd_len);
    if (id < 0) {
        logline("sound: set data %08x (tag %04x)", (unsigned)id, tag);
        return -1;
    }
    g_snd_id = id;
    int max = 0;
    if (sceAtracGetMaxSample(id, &max) < 0 || max <= 0 || max > SOUND_FRAME) {
        logline("sound: %d samples a decode, at most %d", max, SOUND_FRAME);
        return -1;
    }
    /* Looping is the decoder's when the file carries loop points, and
       ours when it does not: it refuses the request then, and the end of
       the data is where this rewinds it. */
    int rc = sceAtracSetLoopNum(id, -1);
    logline("sound: tag %04x, %u Hz, %d samples a decode, %lu KB, loop %s", tag, rate, max,
            (unsigned long)(g_snd_len / 1024), rc < 0 ? "by hand" : "by the decoder");
    g_snd_loops = g_snd_decoded = 0;
    return 0;
}

/* Back to the top, in place; when that is refused the same bytes are
   opened again, which is what a fresh ID costs. */
static int sound_rewind(void) {
    if (sceAtracResetPlayPosition(g_snd_id, 0, 0, 0) >= 0) return 0;
    sceAtracReleaseAtracID(g_snd_id);
    g_snd_id = sceAtracSetDataAndGetID(g_snd_buf, (SceSize)g_snd_len);
    if (g_snd_id < 0) { logline("sound: rewind %08x", (unsigned)g_snd_id); return -1; }
    return 0;
}

/* The end of the data, seen either way the decoder reports it. Logged the
   first time round, so the log says how long the loop is. */
static void sound_end(void) {
    if (g_snd_loops++ == 0) logline("sound: %d samples, again", g_snd_decoded);
    g_snd_decoded = 0;
    if (sound_rewind() != 0) sound_close();
}

/* One decode into the ring. Returns 1 while there is more to come, 0 when
   the sound is over or the ring has no room. */
static int sound_decode(void) {
    unsigned read = g_snd_read, write = g_snd_write;
    if (g_snd_id < 0 || write - read + SOUND_FRAME > SOUND_RING) return 0;
    int n = 0, end = 0, remain = 0;
    unsigned t0 = now_us();
    int rc = sceAtracDecodeData(g_snd_id, (u16 *)g_snd_frame, &n, &end, &remain);
    unsigned took = now_us() - t0;
    if (took > g_snd_worst_us) g_snd_worst_us = took;
    if (rc == (int)PSP_ATRAC_ERROR_ALLDATA_WAS_DECODED) {
        /* Nothing came with it; the ring is filled by the next call. */
        sound_end();
        return g_snd_id >= 0;
    }
    if (rc < 0) {
        logline("sound: decode %08x", (unsigned)rc);
        sound_close();
        return 0;
    }
    if (n > SOUND_FRAME) n = SOUND_FRAME;
    if (g_snd_loops == 0 && g_snd_decoded == 0) {
        /* Once, so a rig run can see that the samples are a signal and
           not the silence a decoder that failed quietly would give. */
        int peak = 0;
        for (int i = 0; i < n * 2; i++) {
            int v = g_snd_frame[i] < 0 ? -g_snd_frame[i] : g_snd_frame[i];
            if (v > peak) peak = v;
        }
        logline("sound: first decode %d samples, peak %d", n, peak);
    }
    int tail = (int)(write % SOUND_RING);
    int first = n < SOUND_RING - tail ? n : SOUND_RING - tail;
    memcpy(g_snd_ring + tail * 2, g_snd_frame, (size_t)first * 4);
    if (n > first) memcpy(g_snd_ring, g_snd_frame + first * 2, (size_t)(n - first) * 4);
    g_snd_decoded += n;
    if (end) sound_end();
    publish();
    g_snd_write = write + (unsigned)n;
    return g_snd_id >= 0;
}

/* Takes the exact generation audio asked for. A newer post makes this one
   obsolete without ever making the real-time thread wait for the lock. */
static int sound_take(unsigned seq) {
    sceKernelWaitSema(g_snd_lock, 1, 0);
    if (seq != g_snd_seq) {
        sceKernelSignalSema(g_snd_lock, 1);
        return 0;
    }
    void *buf = g_snd_next;
    size_t len = g_snd_next_len;
    g_snd_next = 0;
    g_snd_next_len = 0;
    sceKernelSignalSema(g_snd_lock, 1);
    sound_close();
    g_snd_read = g_snd_write = 0;
    if (!buf) return 1;
    g_snd_buf = buf;
    g_snd_len = len;
    if (!g_snd_modules || sound_open() != 0) sound_close();
    return 1;
}

/* sceAtrac lives entirely here. Fill before publishing a sound, then stay
   ahead of the consumer. An interrupted selection abandons its prebuffer and
   moves directly to the newest generation. */
static int sound_run(SceSize args, void *argp) {
    (void)args; (void)argp;
    /* audio_start publishes the already-settled generation in ready. Starting
       there also makes a stop/start cycle safe when seq has moved past zero. */
    unsigned current = g_snd_ready;
    while (!g_quit) {
        if (g_paused) { sceKernelDelayThread(10000); continue; }
        unsigned want = g_snd_want;
        if (want != current) {
            if (!sound_take(want)) { sceKernelDelayThread(1000); continue; }
            current = want;
            while (!g_quit && g_snd_id >= 0 && g_snd_want == current &&
                   g_snd_write - g_snd_read < SOUND_FRAME * 2)
                if (!sound_decode()) break;
            if (g_snd_want != current) continue;
            g_snd_ready_on = g_snd_id >= 0 && g_snd_write != g_snd_read;
            publish();
            g_snd_ready = current;
            continue;
        }
        if (g_snd_id >= 0 && g_snd_write - g_snd_read + SOUND_FRAME <= SOUND_RING) {
            sound_decode();
            continue;
        }
        sceKernelDelayThread(1000);
    }
    sound_close();
    return 0;
}

/* Mixes already-decoded sound under a chunk the tune has rendered. It never
   waits: during a hand-over it fades the old ring, asks the worker to switch,
   and returns silence until two ATRAC frames have been prefetched. */
static void sound_render(short *out, int frames) {
    unsigned seq = g_snd_seq;
    if (seq != g_snd_taken) {
        if (g_snd_gain > 0.001f) {
            g_snd_target = 0.0f;
        } else {
            g_snd_gain = g_snd_target = 0.0f;
            g_snd_want = seq;
            unsigned ready = g_snd_ready;
            publish();
            if (ready != seq) return;
            g_snd_taken = seq;
            if (!g_snd_ready_on) { g_snd_on = 0; return; }
            g_snd_target = 1.0f;
            g_snd_on = 1;
        }
    }
    if (!g_snd_on) return;

    float g = g_snd_gain, target = g_snd_target;
    float step = target > g ? 1.0f / (RATE * SOUND_IN_S) : -1.0f / (RATE * SOUND_OUT_S);
    unsigned read = g_snd_read, write = g_snd_write;
    /* Acquire the samples the decoder published before advancing write. On
       the PSP's single core this compiler barrier is sufficient; no cache
       maintenance or SMP fence belongs in a cached SPSC ring. */
    publish();
    unsigned available = write - read;
    int have = available < (unsigned)frames ? (int)available : frames;
    if (have < frames) g_snd_underflows += (unsigned)(frames - have);
    for (int f = 0; f < have; f++) {
        if (step > 0.0f ? g < target : g > target) g += step;
        else g = target;
        float k = g * SOUND_LEVEL;
        const short *s = g_snd_ring + ((read + (unsigned)f) % SOUND_RING) * 2;
        int l = out[f * 2] + (int)(s[0] * k), r = out[f * 2 + 1] + (int)(s[1] * k);
        out[f * 2] = (short)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[f * 2 + 1] = (short)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    publish();
    g_snd_read = read + (unsigned)have;
    g_snd_gain = g;
}

/* Whichever thread calls, the bytes are its own again on return; the
   copy is the audio side's until it lets go. */
static void sound_post(void *copy, size_t len) {
    if (g_snd_lock < 0) { free(copy); return; }
    sceKernelWaitSema(g_snd_lock, 1, 0);
    free(g_snd_next);
    g_snd_next = copy;
    g_snd_next_len = len;
    g_snd_seq++;
    sceKernelSignalSema(g_snd_lock, 1);
}

void audio_sound_play(const void *at3, size_t len) {
    if (!at3 || !len) { audio_sound_stop(); return; }
    /* sceAtrac wants its buffer four-byte aligned and reads it for as long
       as the ID lives, which is why it is copied rather than borrowed. */
    void *copy = memalign(64, len);
    if (!copy) { logline("sound: no room for %lu", (unsigned long)len); return; }
    memcpy(copy, at3, len);
    sound_post(copy, len);
}

void audio_sound_stop(void) {
    sound_post(0, 0);
}

/* The AV modules, once: the film's decoder loads AVCODEC through the
   player and this asks it to, then adds ATRAC3PLUS, which the sceAtrac
   calls resolve against. Done before the thread starts, so the first
   sound does not spend its chunk on a module load. */
static void sound_modules(void) {
    if (g_snd_modules) return;
    if (!player_avcodec_up()) return;
    int rc = sceUtilityLoadAvModule(PSP_AV_MODULE_ATRAC3PLUS);
    if (rc < 0) { logline("sound: atrac3plus module %08x", (unsigned)rc); return; }
    g_snd_modules = 1;
}

/* Boot the Media Engine (the AVCODEC/ATRAC3PLUS module_start does it) before
   anything else touches the GE. On real hardware, loading these after the
   entropy sweep has run wedges ATRAC3PLUS's module_start on its ME RPC; done
   first, the ME comes up clean and the later audio_start() reuses it. Safe to
   call more than once. */
void audio_load_modules(void) {
    sound_modules();
}

/* ------------------------------------------------------------ the thread */

/* The score is dark ambience low-passed at 1.6 kHz, so its useful band fits
   comfortably below the 5.5-kHz Nyquist limit here. Synthesize it at quarter
   rate, then linearly interpolate onto the PSP's fixed 44.1-kHz channel. This
   preserves tempo and continuous phase while removing three quarters of the
   voice, filter, delay, and reverb work. */
static void music_render_output(short *out) {
    if (!g_synth_primed) {
        music_render(g_synth_carry, 1);
        g_synth_primed = 1;
    }
    music_render(g_synth_low, CHUNK / 4);
    int l = g_synth_carry[0], r = g_synth_carry[1];
    for (int i = 0; i < CHUNK / 4; i++) {
        int nl = g_synth_low[i * 2], nr = g_synth_low[i * 2 + 1];
        short *d = out + i * 8;
        d[0] = (short)l;                    d[1] = (short)r;
        d[2] = (short)((l * 3 + nl) / 4);   d[3] = (short)((r * 3 + nr) / 4);
        d[4] = (short)((l + nl) / 2);       d[5] = (short)((r + nr) / 2);
        d[6] = (short)((l + nl * 3) / 4);   d[7] = (short)((r + nr * 3) / 4);
        l = nl;
        r = nr;
    }
    g_synth_carry[0] = (short)l;
    g_synth_carry[1] = (short)r;
}

static int run(SceSize args, void *argp) {
    (void)args; (void)argp;
    int b = 0;
    while (!g_quit) {
        if (g_paused) { sceKernelDelayThread(10000); continue; }
        unsigned t0 = now_us();
        music_render_output(g_buf[b]);
        sound_render(g_buf[b], CHUNK);
        unsigned took = now_us() - t0;
        if (took > g_worst_us) g_worst_us = took;
        if (took > (unsigned)((1000000ULL * CHUNK) / RATE)) g_deadlines++;
        /* Blocks until the chunk before this one has played out, which is
           what paces the loop. */
        unsigned output_at = now_us();
        int rc = sceAudioOutputPannedBlocking(g_channel, PSP_AUDIO_VOLUME_MAX,
                                              PSP_AUDIO_VOLUME_MAX, g_buf[b]);
        unsigned output_took = now_us() - output_at;
        if (output_took > g_output_worst_us) g_output_worst_us = output_took;
        if (rc < 0) {
            g_output_errors++;
            g_output_last_error = rc;
        } else {
            g_output_frames += CHUNK;
        }
        b ^= 1;
    }
    return 0;
}

void audio_duck(int film_on) {
    synth_set_music_level(film_on || g_snd_on ? 0.0f : 1.0f);
}

unsigned audio_worst_us(void) {
    unsigned w = g_worst_us;
    g_worst_us = 0;
    return w;
}

unsigned audio_decode_worst_us(void) {
    unsigned w = g_snd_worst_us;
    g_snd_worst_us = 0;
    return w;
}

void audio_stats(unsigned *output_us, unsigned *frames, unsigned *deadlines,
                 unsigned *underflows, unsigned *errors, int *last_error) {
    *output_us = g_output_worst_us;
    *frames = g_output_frames;
    *deadlines = g_deadlines;
    *underflows = g_snd_underflows;
    *errors = g_output_errors;
    *last_error = g_output_last_error;
    g_output_worst_us = g_output_frames = g_deadlines = 0;
    g_snd_underflows = g_output_errors = 0;
}

int audio_start(void) {
    if (g_up) return 1;
    synth_init(SYNTH_RATE);
    music_init(SYNTH_RATE);
    g_synth_primed = 0;
    g_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, CHUNK, PSP_AUDIO_FORMAT_STEREO);
    if (g_channel < 0) {
        logline("audio: no channel %08x", (unsigned)g_channel);
        return 0;
    }
    g_snd_lock = sceKernelCreateSema("sound", 0, 1, 1, 0);
    if (g_snd_lock < 0) logline("audio: no sound lock %08x", (unsigned)g_snd_lock);
    sound_modules();
    g_snd_taken = g_snd_want = g_snd_ready = g_snd_seq;
    g_snd_read = g_snd_write = 0;
    g_snd_ready_on = g_snd_on = 0;
    g_snd_gain = g_snd_target = 0.0f;
    g_worst_us = g_snd_worst_us = g_output_worst_us = 0;
    g_output_frames = g_deadlines = g_snd_underflows = g_output_errors = 0;
    g_output_last_error = 0;
    g_quit = 0;
    g_snd_thread = sceKernelCreateThread("sound_decode", sound_run, SOUND_PRIORITY,
                                         SOUND_STACK, PSP_THREAD_ATTR_USER, 0);
    int decoder_start = g_snd_thread < 0 ? (int)g_snd_thread
                                         : sceKernelStartThread(g_snd_thread, 0, 0);
    if (decoder_start < 0) {
        logline("audio: no decoder thread %08x", (unsigned)decoder_start);
        if (g_snd_thread >= 0) sceKernelDeleteThread(g_snd_thread);
        g_snd_thread = -1;
    }
    g_thread = sceKernelCreateThread("audio", run, AUDIO_PRIORITY, AUDIO_STACK,
                                     PSP_THREAD_ATTR_USER, 0);
    if (g_thread < 0) {
        logline("audio: no thread %08x", (unsigned)g_thread);
        g_quit = 1;
        if (g_snd_thread >= 0) {
            sceKernelWaitThreadEnd(g_snd_thread, 0);
            sceKernelDeleteThread(g_snd_thread);
            g_snd_thread = -1;
        }
        sceAudioChRelease(g_channel);
        g_channel = -1;
        if (g_snd_lock >= 0) {
            sceKernelDeleteSema(g_snd_lock);
            g_snd_lock = -1;
        }
        return 0;
    }
    int start = sceKernelStartThread(g_thread, 0, 0);
    if (start < 0) {
        logline("audio: thread start %08x", (unsigned)start);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
        g_quit = 1;
        if (g_snd_thread >= 0) {
            sceKernelWaitThreadEnd(g_snd_thread, 0);
            sceKernelDeleteThread(g_snd_thread);
            g_snd_thread = -1;
        }
        sceAudioChRelease(g_channel);
        g_channel = -1;
        if (g_snd_lock >= 0) {
            sceKernelDeleteSema(g_snd_lock);
            g_snd_lock = -1;
        }
        return 0;
    }
    g_up = 1;
    logline("audio: up");
    return 1;
}

void audio_stop(void) {
    if (!g_up) return;
    g_quit = 1;
    sceKernelWaitThreadEnd(g_thread, 0);
    sceKernelDeleteThread(g_thread);
    g_thread = -1;
    if (g_snd_thread >= 0) {
        sceKernelWaitThreadEnd(g_snd_thread, 0);
        sceKernelDeleteThread(g_snd_thread);
        g_snd_thread = -1;
    }
    sceAudioChRelease(g_channel);
    g_channel = -1;
    if (g_snd_lock >= 0) {
        sceKernelDeleteSema(g_snd_lock);
        g_snd_lock = -1;
    }
    free(g_snd_next);
    g_snd_next = 0;
    g_snd_next_len = 0;
    g_snd_on = 0;
    g_up = 0;
}
