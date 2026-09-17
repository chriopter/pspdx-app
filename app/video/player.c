#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspmpeg.h>
#include <psputility.h>
#include <psputility_avmodules.h>
#include <malloc.h>
#include <string.h>

#include "video/player.h"
#include "video/psmf.h"
#include "util/runtime.h"

/* Half a megabyte of ring is a quarter of a film at the catalog's bitrate;
   the decoder asks for more as it goes. */
#define RING_PACKETS 256
#define NO_DATA 0x80618001

/* Below the main thread and below the network: a picture of the film is
   the first thing that should give way. */
#define FILM_PRIORITY 0x23
#define FILM_STACK (64 * 1024)

static const unsigned char *g_psmf;     /* the whole stream, header and all */
static struct psmf_info g_info;        /* what its header says */
static unsigned g_frame_ticks;          /* one picture's stay, at 90 kHz */
static const unsigned char *g_stream;   /* the packs, after the header */
static int g_packs, g_next_pack;
static int g_avcodec, g_modules;

static SceMpeg g_mpeg;
static SceMpegRingbuffer g_ring;
static unsigned char g_header[PSMF_HEADER] __attribute__((aligned(64)));
static SceMpegStream *g_video;
static SceMpegAu g_au;
static void *g_ring_data, *g_mpeg_data, *g_es;
static int g_stride;
static int g_open, g_created, g_ring_built, g_inited;
static int g_got_au, g_poisoned, g_disabled;

/* The handover: the thread decodes into one of the two buffers, publishes
   it, and may not touch the other until the interface has taken this one,
   which is what g_free counts. So the buffer being drawn is never the
   buffer being written, and no picture is thrown away. */
static SceUID g_thread = -1;
static SceUID g_free = -1;
static void *g_buf[2];
static int g_write;
static void *volatile g_ready;
static volatile int g_quit, g_failed;

/* The decoder pulls packs through this; it is what makes the stream a
   ring rather than a file. */
static SceInt32 feed(ScePVoid data, SceInt32 packets, ScePVoid param) {
    (void)param;
    int left = g_packs - g_next_pack;
    if (packets > left) packets = left;
    if (packets <= 0) return 0;
    size_t bytes = (size_t)packets * PSMF_PACK;
    memcpy(data, g_stream + (size_t)g_next_pack * PSMF_PACK, bytes);
    /* The Media Engine is another memory consumer; unlike PPSSPP it does
       not see dirty CPU cache lines. */
    sceKernelDcacheWritebackRange(data, bytes);
    g_next_pack += packets;
    return packets;
}

int player_avcodec_up(void) {
    if (g_avcodec) return 1;
    int rc = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
    if (rc < 0) {
        logline("player: avcodec module %08x", rc);
        return 0;
    }
    g_avcodec = 1;
    return 1;
}

static int modules_up(void) {
    if (g_modules) return 1;
    if (!player_avcodec_up()) return 0;
    if (sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE) < 0) {
        logline("player: mpegbase module");
        return 0;
    }
    g_modules = 1;
    return 1;
}

static void stream_close(void);

static int stream_open(void) {
    if (g_open) stream_close();
    if (g_disabled || g_info.stream_size < PSMF_PACK) return -1;
    if (psmf_decoder_header(g_psmf, g_info.stream_offset + g_info.stream_size,
                            g_header) != 0) {
        logline("player: stream lacks PSP decoder index");
        return -1;
    }
    if (!modules_up()) return -1;

    /* Where the header says the packs are, not where psmf_build puts
       them: an ICON1.PMF out of an EBOOT says so itself. */
    g_stream = g_psmf + g_info.stream_offset;
    g_packs = (int)(g_info.stream_size / PSMF_PACK);
    g_next_pack = 0;

    int rc = sceMpegInit();
    if (rc < 0) { logline("player: init %08x", rc); return -1; }
    g_inited = 1;

    int ring_size = sceMpegRingbufferQueryMemSize(RING_PACKETS);
    if (ring_size <= 0) {
        logline("player: ring size %08x", (unsigned)ring_size);
        stream_close();
        return -1;
    }
    g_ring_data = memalign(64, ring_size);
    int mpeg_size = sceMpegQueryMemSize(0);
    if (mpeg_size <= 0) {
        logline("player: context size %08x", (unsigned)mpeg_size);
        stream_close();
        return -1;
    }
    g_mpeg_data = memalign(64, mpeg_size);
    if (!g_ring_data || !g_mpeg_data) {
        logline("player: no room (%d + %d)", ring_size, mpeg_size);
        stream_close();
        return -1;
    }
    rc = sceMpegRingbufferConstruct(&g_ring, RING_PACKETS, g_ring_data, ring_size, feed, 0);
    if (rc < 0) { logline("player: ring %08x", rc); stream_close(); return -1; }
    g_ring_built = 1;
    rc = sceMpegCreate(&g_mpeg, g_mpeg_data, mpeg_size, &g_ring, g_stride, 0, 0);
    if (rc < 0) { logline("player: create %08x", rc); stream_close(); return -1; }
    g_created = 1;

    SceInt32 offset = 0, size = 0;
    rc = sceMpegQueryStreamOffset(&g_mpeg, g_header, &offset);
    if (rc < 0 || offset != (SceInt32)g_info.stream_offset) {
        logline("player: stream offset %08x (%d)", (unsigned)rc, offset);
        stream_close();
        return -1;
    }
    rc = sceMpegQueryStreamSize(g_header, &size);
    if (rc < 0 || size != (SceInt32)g_info.stream_size) {
        logline("player: stream size %08x (%d)", (unsigned)rc, size);
        stream_close();
        return -1;
    }
    g_video = sceMpegRegistStream(&g_mpeg, 0, 0);
    if (!g_video) { logline("player: no video stream"); stream_close(); return -1; }
    g_es = sceMpegMallocAvcEsBuf(&g_mpeg);
    if (!g_es) { logline("player: no es buffer"); stream_close(); return -1; }
    rc = sceMpegInitAu(&g_mpeg, g_es, &g_au);
    if (rc < 0) { logline("player: au init %08x", rc); stream_close(); return -1; }

    SceMpegAvcMode mode = { -1, PSP_DISPLAY_PIXEL_FORMAT_8888 };
    rc = sceMpegAvcDecodeMode(&g_mpeg, &mode);
    if (rc < 0) { logline("player: decode mode %08x", rc); stream_close(); return -1; }
    g_got_au = 0;
    g_poisoned = 0;
    g_open = 1;
    logline("player: %d packs, ring %d KB, ctx %d KB", g_packs, ring_size / 1024, mpeg_size / 1024);
    return 0;
}

/* Decodes the next picture into frame if one is due. Returns 1 when frame
   was written, 0 when nothing happened, -1 at the end of the stream. */
static int stream_step(void *frame) {
    if (!g_open) return -2;

    int avail = sceMpegRingbufferAvailableSize(&g_ring);
    if (avail < 0) {
        logline("player: ring available %08x", (unsigned)avail);
        g_poisoned = 1;
        return -2;
    }
    if (avail > 0 && g_next_pack < g_packs) {
        /* Retail traces submit bounded batches and pass the complete free
           count separately. Never ask the callback for more than remains. */
        int request = avail > 32 ? 32 : avail;
        int left = g_packs - g_next_pack;
        if (request > left) request = left;
        int rc = sceMpegRingbufferPut(&g_ring, request, avail);
        if (rc < 0) {
            logline("player: ring put %08x", (unsigned)rc);
            g_poisoned = 1;
            return -2;
        }
    }

    SceInt32 unk = 0;
    int rc = sceMpegGetAvcAu(&g_mpeg, g_video, &g_au, &unk);
    if (rc == (int)NO_DATA) {
        /* NO_DATA is also the normal warm-up answer. In particular, InitAu
           leaves the timestamps at their end sentinel, so neither that nor
           having submitted the final pack proves EOF. Only an empty ring
           after at least one AU was produced does. */
        avail = sceMpegRingbufferAvailableSize(&g_ring);
        if (avail < 0) {
            logline("player: ring drain %08x", (unsigned)avail);
            g_poisoned = 1;
            return -2;
        }
        if (g_next_pack >= g_packs &&
            (avail == RING_PACKETS || (g_got_au && g_au.iDts == 0xFFFFFFFFu)))
            return g_got_au ? -1 : -2;
        return 0;
    }
    if (rc < 0) {
        logline("player: au %08x", rc);
        g_poisoned = 1;
        return -2;
    }
    g_got_au = 1;

    void *dst = frame;
    SceInt32 status = 0;
    rc = sceMpegAvcDecode(&g_mpeg, &g_au, g_stride, &dst, &status);
    if (rc < 0) {
        logline("player: decode %08x", rc);
        g_poisoned = 1;
        return -2;
    }
    return 1;
}

static void stream_close(void) {
    /* FlushAllStream is for reusing a live context. Calling it while a short
       stream is still warming up crashes sceMpeg_library on real hardware.
       A context being destroyed needs only its owned objects released. */
    if (g_poisoned) {
        /* A real PSP can fault inside sceMpegFreeAvcEsBuf/Delete after a
           runtime stream error.  The context is only about 600 KiB and no
           firmware call can safely recover it, so quarantine it for the rest
           of this process rather than re-entering corrupted library state. */
        logline("player: abandoned rejected decoder context");
        g_disabled = 1;
        g_open = g_created = g_ring_built = g_inited = 0;
        g_ring_data = g_mpeg_data = g_es = 0;
        g_video = 0;
        return;
    }
    if (g_created) {
        if (g_es) sceMpegFreeAvcEsBuf(&g_mpeg, g_es);
        if (g_video) sceMpegUnRegistStream(g_mpeg, g_video);
        sceMpegDelete(&g_mpeg);
    }
    if (g_ring_built)
        sceMpegRingbufferDestruct(&g_ring);
    if (g_inited) sceMpegFinish();
    g_open = g_created = g_ring_built = g_inited = 0;
    free(g_ring_data);
    free(g_mpeg_data);
    g_ring_data = g_mpeg_data = g_es = 0;
    g_video = 0;
    g_open = 0;
}

static int decode_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    if (stream_open() != 0) {
        stream_close();
        g_failed = 1;
        return 0;
    }
    unsigned started = now_ms();
    int paced = 0;

    while (!g_quit) {
        /* The film's own rate by the clock, not by the frame: when the
           thread was held off for a while the next pictures come back to
           back until the film is in step again. */
        if ((unsigned long long)paced * g_frame_ticks >=
            (unsigned long long)(now_ms() - started) * 90) {
            sceKernelDelayThread(2000);
            continue;
        }
        SceUInt wait = 50000;
        if (sceKernelWaitSema(g_free, 1, &wait) < 0) continue;
        if (g_quit) break;

        int rc = stream_step(g_buf[g_write]);
        if (rc > 0) {
            g_ready = g_buf[g_write];
            g_write ^= 1;
            paced++;
            continue;
        }
        sceKernelSignalSema(g_free, 1);     /* nothing written, keep the buffer */
        if (rc == 0) {
            sceKernelDelayThread(1000);     /* the ring is still filling */
            continue;
        }
        if (rc == -2) {
            logline("film: decoder stopped on a bad stream");
            g_failed = 1;
            break;
        }
        if (paced == 0) { logline("film: ends where it starts"); g_failed = 1; break; }
        logline("film: %d pictures in %u ms, again", paced, now_ms() - started);
        started = now_ms();
        paced = 0;
        /* The library keeps its own read position inside the ring; the clean
           way back to the start is a fresh context over the same stream. */
        if (stream_open() != 0) { logline("film: rewind failed"); g_failed = 1; break; }
    }
    stream_close();
    return 0;
}

int player_start(const unsigned char *psmf, size_t len, void *frame_a,
                 void *frame_b, int stride) {
    player_stop();
    if (g_disabled) {
        logline("player: decoder disabled after firmware error");
        g_failed = 1;
        return -1;
    }
    /* Read here, on the caller's thread, so a film that is not a PSMF at
       all is refused before a thread is spent on it. */
    if (psmf_parse(psmf, len, &g_info) != 0) {
        logline("player: not a psmf");
        g_failed = 1;
        return -1;
    }
    if (!frame_a || !frame_b || stride < g_info.width) {
        logline("player: bad frame buffers");
        g_failed = 1;
        return -1;
    }
    g_frame_ticks = psmf_frame_ticks(&g_info);
    g_psmf = psmf;
    g_stride = stride;
    g_buf[0] = frame_a;
    g_buf[1] = frame_b;
    g_write = 0;
    g_ready = 0;
    g_quit = g_failed = 0;

    /* One to start with: the thread fills a buffer, then waits for the
       interface to take it before filling the other. */
    g_free = sceKernelCreateSema("film", 0, 1, 2, 0);
    if (g_free < 0) { logline("player: no sema %08x", g_free); g_failed = 1; return -1; }
    g_thread = sceKernelCreateThread("film", decode_thread, FILM_PRIORITY, FILM_STACK,
                                     PSP_THREAD_ATTR_USER, 0);
    if (g_thread < 0) {
        logline("player: no thread %08x", g_thread);
        sceKernelDeleteSema(g_free);
        g_free = -1;
        g_failed = 1;
        return -1;
    }
    int rc = sceKernelStartThread(g_thread, 0, 0);
    if (rc < 0) {
        logline("player: start thread %08x", rc);
        sceKernelDeleteThread(g_thread);
        sceKernelDeleteSema(g_free);
        g_thread = g_free = -1;
        g_failed = 1;
        return -1;
    }
    return 0;
}

void *player_take(void) {
    void *picture = g_ready;
    if (!picture) return 0;
    g_ready = 0;
    /* Taking this one lets go of the one before it, which the GE finished
       reading at the last frame's sync. */
    sceKernelSignalSema(g_free, 1);
    return picture;
}

int player_failed(void) { return g_failed; }

void player_stop(void) {
    if (g_thread >= 0) {
        g_quit = 1;
        sceKernelSignalSema(g_free, 1);     /* out of the handover, not into a decode */
        sceKernelWaitThreadEnd(g_thread, 0);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (g_free >= 0) {
        sceKernelDeleteSema(g_free);
        g_free = -1;
    }
    g_ready = 0;
    g_psmf = 0;
}
