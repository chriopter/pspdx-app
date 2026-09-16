/*
 * The 2D render layer. Everything the shell draws goes through here: flat
 * quads, gradients, waves, soft lights, and textures out of system RAM.
 *
 * There is no depth buffer on purpose. A 2D UI never needs one, and leaving
 * it out gives back 272 KB of the 2 MB of VRAM -- enough that both display
 * buffers fit with room to spare.
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gui/gfx.h"
#include "util/runtime.h"

#define BUF_W 512                       /* draw buffer stride, must be 2^n */
#define FRAME_SIZE (BUF_W * SCR_H * 4)  /* 0x88000 */
#define GLOW_SIZE 64

/* The ripple: one tile of water surface, held as normals rather than as a
   picture, in as many steps as it takes to loop, each with a pyramid of
   smaller copies under it. Averaging two normals gives the normal of the two
   of them together, so the coarse levels come out genuinely flatter -- which
   is what water does as it goes away, and why the far rows neither shimmer
   nor show the tile. */
#define RIPPLE_SIZE 256
#define RIPPLE_FRAMES 4         /* crossfaded, so few are needed */
/* Four levels, down to 16x16: the GE reads a texture in 16-byte units,
   so a T8 level narrower than 16 texels has a stride it cannot address. */
#define RIPPLE_LEVELS 4
#define RIPPLE_BYTES (87040)    /* 256^2 + 128^2 + 64^2 + 32^2 */

static unsigned int __attribute__((aligned(16))) g_list[64 * 1024];
/* Kept clear under the list's end for what is drawn between two calls of
   gfx_list_room: rectangles, strips, a batch's worth of glows. */
#define LIST_SPARE (32 * 1024)
static int g_in_frame;
static unsigned g_list_sent;            /* bytes of this frame already sent */
static unsigned g_frames;
static void *g_draw;                    /* the draw buffer, relative to VRAM */
static int g_up;
static struct gfx_texture g_glow;
static unsigned char *g_ripple;                 /* RIPPLE_FRAMES tiles of T8 */
static float g_normal[256][3];                  /* what each index stands for */
/* The palette as lit, and two copies scaled for the two steps of the
   ripple that are on screen at once -- see gfx_water_step. */
static unsigned char g_clut_lit[256][3];
static unsigned __attribute__((aligned(16))) g_clut[4][256];

/* Vertex layouts. The GE reads the components in a fixed order -- texture,
   colour, position -- so the struct members have to be declared in that
   order too. */
struct vcol  { unsigned color; short x, y, z; };
struct vtex  { short u, v; short x, y, z; };
struct vtexc { short u, v; unsigned color; short x, y, z; };

/* A batch. The backdrop asks for a few hundred lights and three dozen
   strips a frame; sent one at a time that is a texture bind, a colour and a
   draw each. Held here they leave as one sprite array and one run of
   strips, out of two allocations instead of three hundred. Only one kind is
   ever pending, so appending the other kind sends what is waiting first and
   the order the caller drew in survives. */
#define BATCH_SPRITES 320
#define BATCH_STRIPS 40
#define BATCH_STRIP_VERTS 1200

static int g_batching;
static int g_veil = 256;

void gfx_veil(int keep) { g_veil = keep < 0 ? 0 : keep > 256 ? 256 : keep; }

unsigned gfx_veiled(unsigned color) {
    if (g_veil == 256) return color;
    unsigned a = (color >> 24) * (unsigned)g_veil >> 8;
    return (color & 0x00FFFFFF) | (a << 24);
}
static struct vtexc *g_sprite;          /* two vertices per sprite */
static int g_sprites;
static struct vcol *g_strip;
static int g_strip_verts, g_nstrips;
static struct { short first, count; } g_strip_at[BATCH_STRIPS];

/* One soft white disc, alpha falling off with the square of the distance.
   Every light on screen is this texture, scaled and tinted. */
static void make_glow(void) {
    g_glow.w = g_glow.h = g_glow.tw = g_glow.th = GLOW_SIZE;
    g_glow.pixels = memalign(16, GLOW_SIZE * GLOW_SIZE * 4);
    if (!g_glow.pixels) return;
    unsigned *px = g_glow.pixels;
    float c = (GLOW_SIZE - 1) / 2.0f;
    for (int y = 0; y < GLOW_SIZE; y++) {
        for (int x = 0; x < GLOW_SIZE; x++) {
            float dx = (x - c) / c, dy = (y - c) / c;
            float d = sqrtf(dx * dx + dy * dy);
            float a = d >= 1.0f ? 0.0f : (1.0f - d) * (1.0f - d);
            px[y * GLOW_SIZE + x] = RGBA(255, 255, 255, (unsigned)(a * 255.0f));
        }
    }
    sceKernelDcacheWritebackRange(g_glow.pixels, GLOW_SIZE * GLOW_SIZE * 4);
}

/* The tile holds a normal per texel, not a colour: the high nibble is the
   slope across, the low nibble the slope away, and the palette turns the pair
   into whatever the light is doing this frame. Sixteen steps each is coarse
   for a picture and plenty for a slope, since the eye reads the movement and
   not the value. */
static void ripple_normals(void) {
    for (int i = 0; i < 256; i++) {
        float nx = ((i >> 4) - 7.5f) / 7.5f;
        float ny = ((i & 15) - 7.5f) / 7.5f;
        float flat = nx * nx + ny * ny;
        if (flat > 1.0f) {                      /* off the hemisphere: lay it over */
            float k = 1.0f / sqrtf(flat);
            nx *= k; ny *= k; flat = 1.0f;
        }
        g_normal[i][0] = nx;
        g_normal[i][1] = ny;
        g_normal[i][2] = sqrtf(1.0f - flat);
    }
}

static unsigned char normal_index(float nx, float ny) {
    int ix = (int)((nx + 1.0f) * 7.5f + 0.5f);
    int iy = (int)((ny + 1.0f) * 7.5f + 0.5f);
    if (ix < 0) ix = 0; else if (ix > 15) ix = 15;
    if (iy < 0) iy = 0; else if (iy > 15) iy = 15;
    return (unsigned char)(ix << 4 | iy);
}

/* Eight travelling waves, no two along the same line and none of the
   directions a simple ratio of another, with the amplitude falling as the
   wave gets shorter -- a sea, not a corrugated roof. All of them run mostly
   toward the viewer: seen from this low, a sea is streaks lying across the
   view, not cells, and a streak has no corner to recognise when the tile
   comes round again. The tile still wraps in
   both directions and in time, so the frames play round and round; what it
   no longer does is look like a stamp when it is laid down ten times. A sine
   of a sum is two products of the ends, which is what keeps this to a few
   thousand sines instead of a million. */
static void ripple_height(float *h, int frame) {
    /* Fourteen on a tile twice the size it was: the two longest span the
       whole tile and vary it slowly, so its return is twice as far off and
       no two returns look alike; the two shortest are the chop the bigger
       tile would otherwise have lost. Every wave is a pass over the tile
       for every step, and that is what the boot pays for. */
    static const struct { int px, py, turns; } WAVE[] = {
        {  0,  1,  1 }, {  1,  1, -1 }, { -1,  2,  1 }, {  1,  3, -1 },
        {  2,  3,  2 }, { -2,  5, -2 }, {  3,  5,  1 }, {  1,  7,  3 },
        { -3,  8, -1 }, {  2, 11,  2 }, { -1, 13,  1 }, {  4, 13, -3 },
        { -2, 17,  2 }, {  3, 23, -1 },
    };
    for (int i = 0; i < RIPPLE_SIZE * RIPPLE_SIZE; i++) h[i] = 0.0f;
    for (unsigned w = 0; w < sizeof(WAVE) / sizeof(*WAVE); w++) {
        int px = WAVE[w].px, py = WAVE[w].py;
        float amp = 1.0f / powf((float)(px * px + py * py), 0.4f);
        float phase = 6.2831853f * WAVE[w].turns * frame / RIPPLE_FRAMES;
        float sx[RIPPLE_SIZE], cx[RIPPLE_SIZE], sy[RIPPLE_SIZE], cy[RIPPLE_SIZE];
        for (int x = 0; x < RIPPLE_SIZE; x++) {
            float a = 6.2831853f * px * x / RIPPLE_SIZE + phase;
            sx[x] = sinf(a); cx[x] = cosf(a);
        }
        for (int y = 0; y < RIPPLE_SIZE; y++) {
            float b = 6.2831853f * py * y / RIPPLE_SIZE;
            sy[y] = sinf(b); cy[y] = cosf(b);
        }
        for (int y = 0; y < RIPPLE_SIZE; y++)
            for (int x = 0; x < RIPPLE_SIZE; x++)
                h[y * RIPPLE_SIZE + x] += amp * (sx[x] * cy[y] + cx[x] * sy[y]);
    }
}

/* How far the steepest of those slopes is allowed to lean. Less than it
   was: on a sea thirty metres across the ripple is texture, not weather. */
#define RIPPLE_BUMP 0.8f

static void ripple_mip(const unsigned char *src, int n, unsigned char *dst) {
    for (int y = 0; y < n / 2; y++) {
        for (int x = 0; x < n / 2; x++) {
            float ax = 0, ay = 0, az = 0;
            for (int k = 0; k < 4; k++) {
                const float *nn = g_normal[src[(y * 2 + (k >> 1)) * n + x * 2 + (k & 1)]];
                ax += nn[0]; ay += nn[1]; az += nn[2];
            }
            float k = 1.0f / sqrtf(ax * ax + ay * ay + az * az + 1e-9f);
            dst[y * (n / 2) + x] = normal_index(ax * k, ay * k);
        }
    }
}

static void make_ripple(void) {
    unsigned t0 = now_us();
    g_ripple = memalign(16, RIPPLE_FRAMES * RIPPLE_BYTES);
    if (!g_ripple) return;
    ripple_normals();
    static float h[RIPPLE_SIZE * RIPPLE_SIZE];
    for (int f = 0; f < RIPPLE_FRAMES; f++) {
        ripple_height(h, f);
        unsigned char *tile = g_ripple + f * RIPPLE_BYTES;
        for (int y = 0; y < RIPPLE_SIZE; y++) {
            int ym = (y + RIPPLE_SIZE - 1) % RIPPLE_SIZE;
            int yp = (y + 1) % RIPPLE_SIZE;
            for (int x = 0; x < RIPPLE_SIZE; x++) {
                int xm = (x + RIPPLE_SIZE - 1) % RIPPLE_SIZE;
                int xp = (x + 1) % RIPPLE_SIZE;
                float dx = h[y * RIPPLE_SIZE + xp] - h[y * RIPPLE_SIZE + xm];
                float dy = h[yp * RIPPLE_SIZE + x] - h[ym * RIPPLE_SIZE + x];
                tile[y * RIPPLE_SIZE + x] =
                    normal_index(-dx * RIPPLE_BUMP, -dy * RIPPLE_BUMP);
            }
        }
        unsigned char *level = tile;
        for (int n = RIPPLE_SIZE; n > RIPPLE_SIZE >> (RIPPLE_LEVELS - 1); n /= 2) {
            ripple_mip(level, n, level + n * n);
            level += n * n;
        }
    }
    sceKernelDcacheWritebackRange(g_ripple, RIPPLE_FRAMES * RIPPLE_BYTES);
    logline("ripple: %d steps of %dx%d in %u ms", RIPPLE_FRAMES, RIPPLE_SIZE, RIPPLE_SIZE,
            (now_us() - t0) / 1000);
}

void gfx_init(void) {
    if (!g_glow.pixels) make_glow();
    if (!g_ripple) make_ripple();
    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)FRAME_SIZE, BUF_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    g_up = 1;
    logline("gu: up, %d KB of vram left after both buffers",
            2048 - 2 * (FRAME_SIZE / 1024));
}

void gfx_shutdown(void) {
    if (!g_up) return;
    sceGuTerm();
    g_up = 0;
}

void gfx_frame_begin(unsigned clear) {
    /* Batch memory comes out of the list that is about to be reset. */
    g_batching = g_sprites = g_nstrips = g_strip_verts = 0;
    g_sprite = 0;
    g_strip = 0;
    g_in_frame = 1;
    g_list_sent = 0;
    sceGuStart(GU_DIRECT, g_list);
    sceGuClearColor(clear);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT);
}

static unsigned g_worst_ge, g_worst_vblank, g_worst_list;
static void (*g_overlay)(void);

void gfx_frame_overlay(void (*overlay)(void)) { g_overlay = overlay; }

void gfx_frame_end(void) {
    gfx_batch_end();
    g_in_frame = 0;
    /* How much of the list the frame took: a frame that fills it writes
       past it, and the first sign of that would be a hang. What went to
       the GE early to make room counts too. */
    unsigned used = g_list_sent + (unsigned)sceGuFinish();
    if (used > g_worst_list) g_worst_list = used;
    unsigned t0 = now_us();
    sceGuSync(0, 0);
    unsigned t1 = now_us();
    /* The firmware's dialogs draw themselves over a finished frame, so
       this is the one place they can be given the buffer: the list is
       done and nothing has been shown yet. */
    if (g_overlay) g_overlay();
    sceDisplayWaitVblankStart();
    unsigned t2 = now_us();
    if (t1 - t0 > g_worst_ge) g_worst_ge = t1 - t0;
    if (t2 - t1 > g_worst_vblank) g_worst_vblank = t2 - t1;
    g_draw = sceGuSwapBuffers();
    g_frames++;
}

void gfx_frame_worst(unsigned *ge_us, unsigned *vblank_us, unsigned *list_bytes) {
    *ge_us = g_worst_ge;
    *vblank_us = g_worst_vblank;
    *list_bytes = g_worst_list;
    g_worst_ge = g_worst_vblank = g_worst_list = 0;
}

unsigned gfx_frames(void) { return g_frames; }

/* Every primitive sets the state it depends on instead of trusting what the
   last caller left. intraFont in particular re-enables the depth test after
   each print, and with no depth buffer that silently drops everything drawn
   afterwards. */
static void flat_state(void) {
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    /* The water leaves a light burning, and a vertex with no normal in its
       format would be lit off whatever the last one had. */
    sceGuDisable(GU_LIGHTING);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

static void additive(void) {
    /* Added rather than composited: where two lights cross, the overlap
       gets brighter instead of just more opaque. */
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
}

static void bind(const struct gfx_texture *t) {
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(0, t->tw, t->th, t->tw, t->pixels);
    sceGuTexFunc(GU_TFX_MODULATE, t->opaque ? GU_TCC_RGB : GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    /* The water leaves the tile's own scale and creep behind it. */
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
}

static void flush_sprites(void) {
    if (!g_sprites) return;
    bind(&g_glow);
    additive();
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                   GU_TRANSFORM_2D, g_sprites * 2, 0, g_sprite);
    g_sprites = 0;
    g_sprite = 0;
    flat_state();
}

static void flush_strips(void) {
    if (!g_nstrips) return;
    flat_state();
    additive();
    for (int i = 0; i < g_nstrips; i++)
        sceGuDrawArray(GU_TRIANGLE_STRIP,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       g_strip_at[i].count, 0, g_strip + g_strip_at[i].first);
    g_nstrips = 0;
    g_strip_verts = 0;
    g_strip = 0;
}

static void flush_batch(void) {
    flush_sprites();
    flush_strips();
}

void gfx_batch_begin(void) { g_batching = 1; }

void gfx_batch_end(void) {
    flush_batch();
    g_batching = 0;
}

int gfx_list_room(unsigned bytes) {
    if (bytes > sizeof(g_list) - LIST_SPARE)
        return 0;
    if ((unsigned)sceGuCheckList() + bytes + LIST_SPARE <= sizeof(g_list))
        return 1;
    /* A bake or a readback has the list to itself and never comes near. */
    if (!g_in_frame)
        return 0;
    /* The GE keeps its state from one list to the next -- every frame
       counts on that already -- so the frame goes on where it stopped,
       into the same draw buffer. A batch lives in the list and goes first. */
    flush_batch();
    g_list_sent += (unsigned)sceGuFinish();
    sceGuSync(0, 0);
    sceGuStart(GU_DIRECT, g_list);
    return 1;
}

/* Four corners as a strip: TL, BL, TR, BR. Culling is off, so winding does
   not matter. */
static void quad(int x, int y, int w, int h,
                 unsigned tl, unsigned bl, unsigned tr, unsigned br) {
    flush_batch();
    struct vcol *v = sceGuGetMemory(4 * sizeof(struct vcol));
    if (!v) return;
    v[0].color = gfx_veiled(tl); v[0].x = x;     v[0].y = y;     v[0].z = 0;
    v[1].color = gfx_veiled(bl); v[1].x = x;     v[1].y = y + h; v[1].z = 0;
    v[2].color = gfx_veiled(tr); v[2].x = x + w; v[2].y = y;     v[2].z = 0;
    v[3].color = gfx_veiled(br); v[3].x = x + w; v[3].y = y + h; v[3].z = 0;
    flat_state();
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   4, 0, v);
}

void gfx_rect(int x, int y, int w, int h, unsigned color) {
    quad(x, y, w, h, color, color, color, color);
}

void gfx_vgrad(int x, int y, int w, int h, unsigned top, unsigned bottom) {
    quad(x, y, w, h, top, bottom, top, bottom);
}

void gfx_hgrad(int x, int y, int w, int h, unsigned left, unsigned right) {
    quad(x, y, w, h, left, left, right, right);
}

/* Room for one triangle strip of n vertices. Inside a batch it goes into the
   pending run and *batched says so, so the caller writes and leaves; outside
   one it is scratch the caller draws with itself. */
static struct vcol *strip_room(int n, int *batched) {
    *batched = g_batching && n <= BATCH_STRIP_VERTS;
    if (!*batched) {
        flush_batch();
        return sceGuGetMemory(n * sizeof(struct vcol));
    }
    flush_sprites();
    if (g_nstrips == BATCH_STRIPS || g_strip_verts + n > BATCH_STRIP_VERTS)
        flush_strips();
    if (!g_strip) {
        g_strip = sceGuGetMemory(BATCH_STRIP_VERTS * sizeof(struct vcol));
        if (!g_strip) return 0;
    }
    struct vcol *v = g_strip + g_strip_verts;
    g_strip_at[g_nstrips].first = (short)g_strip_verts;
    g_strip_at[g_nstrips].count = (short)n;
    g_nstrips++;
    g_strip_verts += n;
    return v;
}

void gfx_ribbon(const float *x, const float *y, const unsigned *color, int n,
                float half) {
    if (n < 2) return;
    int batched;
    struct vcol *v = strip_room(n * 2, &batched);
    if (!v) return;
    for (int i = 0; i < n; i++) {
        v[i * 2 + 0].color = color[i]; v[i * 2 + 0].x = (short)x[i];
        v[i * 2 + 0].y = (short)(y[i] - half); v[i * 2 + 0].z = 0;
        v[i * 2 + 1].color = color[i]; v[i * 2 + 1].x = (short)x[i];
        v[i * 2 + 1].y = (short)(y[i] + half); v[i * 2 + 1].z = 0;
    }
    if (batched) return;
    flat_state();
    additive();
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   n * 2, 0, v);
}

/* ------------------------------------------------------------------ water */

/* The eye lies low over the water and looks away from itself, which is why
   the surface reflects so much of the sky: at this angle almost nothing gets
   into the water and back out. Fixed, because the camera is. */
static const float EYE[3] = { 0.0f, -0.966f, 0.259f };

/* The same sun the palette is built from, as the GE wants it: a direction in
   the world the mesh lives in, where x runs across, y up and z toward the
   viewer. The tile's axes are x across, y away and z up, so the two swap.
   Its height over the horizon is held at the palette's and only its bearing
   follows the light around, since what a long low swell does with a sun
   depends on how flat that sun lies: this flat, a face turned away from it
   has no light on it at all, which is what makes the troughs black. */
#define SUN_UP 0.30f
static ScePspFVector3 g_sun = { 0.0f, SUN_UP, -0.954f };

static float channel(float v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void gfx_water_light(float lx, float ly, float lz,
                     unsigned deep, unsigned sky, unsigned high, unsigned glint) {
    float k = 1.0f / sqrtf(lx * lx + ly * ly + lz * lz + 1e-6f);
    float sx = lx * k, sz = -ly * k;
    float sk = 1.0f / sqrtf(sx * sx + sz * sz + 1e-6f);
    g_sun.x = sx * sk;
    g_sun.y = SUN_UP;
    g_sun.z = sz * sk;
    /* Halfway between the light and the eye: a texel whose normal points
       there is the one that sends the light straight down the lens. */
    float hx = lx * k + EYE[0], hy = ly * k + EYE[1], hz = lz * k + EYE[2];
    float hk = 1.0f / sqrtf(hx * hx + hy * hy + hz * hz + 1e-6f);
    hx *= hk; hy *= hk; hz *= hk;

    float dr = deep & 0xFF, dg = deep >> 8 & 0xFF, db = deep >> 16 & 0xFF;
    float sr = sky & 0xFF, sg = sky >> 8 & 0xFF, sb = sky >> 16 & 0xFF;
    float tr = high & 0xFF, tg = high >> 8 & 0xFF, tb = high >> 16 & 0xFF;
    float gr = glint & 0xFF, gg = glint >> 8 & 0xFF, gb = glint >> 16 & 0xFF;

    for (int i = 0; i < 256; i++) {
        const float *n = g_normal[i];
        float face = n[0] * EYE[0] + n[1] * EYE[1] + n[2] * EYE[2];
        if (face < 0.0f) face = 0.0f;
        float turn = 1.0f - face;
        float mirror = 0.08f + 0.70f * turn * turn * turn;
        /* What the facet mirrors is not one sky but the piece of it the
           eye's ray, bounced off the facet, goes up into: a facet leaning
           away sends the ray out low, to the burn on the horizon, one
           leaning toward the eye sends it high, into the dark of the
           zenith. So the horizon's light lies on the water broken into
           the ripples, and the near water is dark with the sky over it. */
        float up = 2.0f * face * n[2] - EYE[2];
        float t = (up - 0.05f) * (1.0f / 0.55f);
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        float kr = sr + (tr - sr) * t, kg = sg + (tg - sg) * t, kb = sb + (tb - sb) * t;
        float s = n[0] * hx + n[1] * hy + n[2] * hz;
        if (s < 0.0f) s = 0.0f;
        /* Broad rather than sharp: a sharp glint on a texel the perspective
           has stretched into a dash is a dash. */
        s *= s; s *= s;                                 /* the fourth power */
        float r = dr + (kr - dr) * mirror + gr * s;
        float g = dg + (kg - dg) * mirror + gg * s;
        float b = db + (kb - db) * mirror + gb * s;
        g_clut_lit[i][0] = (unsigned char)channel(r);
        g_clut_lit[i][1] = (unsigned char)channel(g);
        g_clut_lit[i][2] = (unsigned char)channel(b);
    }
}

/* The surface and the strips cut out of it live here from frame to frame:
   the GE has finished with them by the time the next frame is drawn, since
   frame_end syncs before it swaps. */
static struct gfx_water_vertex *g_mesh;
static int g_mesh_verts;
static unsigned short *g_index;
static int g_index_count;

struct gfx_water_vertex *gfx_water_mesh(int verts) {
    if (!g_ripple) return 0;
    if (!g_mesh || verts > g_mesh_verts) {
        free(g_mesh);
        g_mesh = memalign(16, verts * sizeof(struct gfx_water_vertex));
        g_mesh_verts = g_mesh ? verts : 0;
    }
    return g_mesh;
}

unsigned short *gfx_water_index(int count) {
    if (!g_index || count > g_index_count) {
        free(g_index);
        g_index = memalign(16, (size_t)count * sizeof(unsigned short));
        g_index_count = g_index ? count : 0;
    }
    return g_index;
}

void gfx_water_ready(void) {
    if (g_mesh)
        sceKernelDcacheWritebackRange(g_mesh,
            (unsigned)g_mesh_verts * sizeof(struct gfx_water_vertex));
    if (g_index)
        sceKernelDcacheWritebackRange(g_index,
            (unsigned)g_index_count * sizeof(unsigned short));
}

/* The camera the water is drawn through, which the picture in it borrows.
   The vanishing point belongs at the horizon and not at the middle of the
   screen, and the cheapest way to put it there is to tell the GE the screen
   is 20 rows higher than it is. Undone in end(). */
static void water_camera(void) {
    sceGuOffset(2048 - SCR_W / 2, 2048 - (unsigned)GFX_HORIZON);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    /* The field of view that makes one unit at one unit of depth come out
       GFX_FOCAL pixels wide. */
    sceGumPerspective(2.0f * 57.29578f * atanf(SCR_H / (2.0f * GFX_FOCAL)),
                      (float)SCR_W / SCR_H, 0.25f, 300.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
}

/* What the mesh's own light does, as against the ripple's: the ambient is
   the water's dark, which the crossing's colour is read through; the diffuse
   is the swell standing up into a sun that lies all but flat on the horizon,
   so a face turned toward it whitens and one turned away goes to the
   ambient alone; the specular is the glint path, which the GE draws for
   itself because it puts the eye where the eye is -- the half vector turns
   with the pixel, and the facets that send the sun down the lens lie in a
   path from under it to the viewer. */
#define WATER_AMBIENT RGBA(36, 36, 36, 255)
#define WATER_DIFFUSE RGBA(255, 255, 255, 255)
#define WATER_SPECULAR RGBA(255, 255, 255, 255)
#define WATER_SHINE 10.0f

void gfx_water_begin(float du, float dv) {
    if (!g_ripple) return;
    flush_batch();
    water_camera();

    /* The light the swell is lit by. Every colour here is a grey: the room's
       colour is in the crossings and the ripple's palette already, and a
       light with a colour of its own would lay it on twice. */
    sceGuEnable(GU_LIGHTING);
    sceGuEnable(GU_LIGHT0);
    sceGuLightMode(GU_SINGLE_COLOR);
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE_AND_SPECULAR, &g_sun);
    sceGuLightColor(0, GU_DIFFUSE, WATER_DIFFUSE);
    sceGuLightColor(0, GU_SPECULAR, WATER_SPECULAR);
    /* A directional light does not fall off, but the GE is told so rather
       than left with whatever the last caller wanted. */
    sceGuLightAtt(0, 1.0f, 0.0f, 0.0f);
    sceGuAmbient(WATER_AMBIENT);
    sceGuSpecular(WATER_SHINE);
    /* The crossing's colour is the material: its own for the ambient and the
       diffuse, white for the glint, and its alpha is what comes out. */
    sceGuModelColor(0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
    sceGuColorMaterial(GU_AMBIENT | GU_DIFFUSE);

    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuTexMode(GU_PSM_T8, RIPPLE_LEVELS - 1, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR_MIPMAP_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(du, dv);
    additive();
}

void gfx_water_scale(float s) {
    sceGuTexScale(s, s);
}
void gfx_water_step(int which, int frame, float weight) {
    if (!g_ripple) return;
    /* The step's share of the light goes into its own copy of the palette:
       the surface is drawn once per step and the two add up, so a step
       fading in and the one fading out cross without a seam. The GE reads
       the palette when the list runs, after both copies are written. */
    unsigned *clut = g_clut[which & 3];
    int w = (int)(weight * 256.0f + 0.5f);
    if (w < 0) w = 0; else if (w > 256) w = 256;
    for (int i = 0; i < 256; i++)
        clut[i] = RGBA((unsigned)(g_clut_lit[i][0] * w >> 8),
                       (unsigned)(g_clut_lit[i][1] * w >> 8),
                       (unsigned)(g_clut_lit[i][2] * w >> 8), 255);
    sceKernelDcacheWritebackRange(clut, 256 * sizeof(*clut));
    sceGuClutLoad(32, clut);
    unsigned char *tile = g_ripple + (frame % RIPPLE_FRAMES) * RIPPLE_BYTES;
    for (int level = 0, n = RIPPLE_SIZE; level < RIPPLE_LEVELS; level++, n /= 2) {
        sceGuTexImage(level, n, n, n, tile);
        tile += n * n;
    }
}

void gfx_water_shine(float keep) {
    if (!g_ripple) return;
    int q = (int)(keep * 255.0f);
    if (q < 0) q = 0; else if (q > 255) q = 255;
    sceGuLightColor(0, GU_SPECULAR, RGBA(q, q, q, 255));
}

void gfx_water_strip(const struct gfx_water_vertex *v, const unsigned short *idx,
                     int n, float level) {
    if (!g_ripple || n < 4) return;
    if (level < 0.0f) level = 0.0f;
    else if (level > RIPPLE_LEVELS - 1) level = RIPPLE_LEVELS - 1;
    sceGuTexLevelMode(GU_TEXTURE_CONST, level);
    sceGumDrawArray(GU_TRIANGLE_STRIP,
                    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF |
                    GU_VERTEX_32BITF | GU_TRANSFORM_3D | GU_INDEX_16BIT,
                    n, idx, v);
}

void gfx_water_end(void) {
    if (!g_ripple) return;
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    flat_state();
}

/* ----------------------------------------------------------- the mirror */

void gfx_mirror_begin(const struct gfx_texture *t) {
    if (!t || !t->pixels) return;
    flush_batch();
    water_camera();
    /* Clamped rather than repeated: past the picture's edge the corner holds
       the edge it left, and the caller has faded it out by then anyway. */
    bind(t);
    additive();
    sceGuDisable(GU_LIGHTING);
    sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
}

struct gfx_mirror_vertex *gfx_mirror_room(int verts) {
    return sceGuGetMemory(verts * sizeof(struct gfx_mirror_vertex));
}

void gfx_mirror_strip(const struct gfx_mirror_vertex *v, int n) {
    if (n < 4) return;
    sceGumDrawArray(GU_TRIANGLE_STRIP,
                    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF |
                    GU_TRANSFORM_3D, n, 0, v);
}

void gfx_mirror_end(void) {
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    flat_state();
}

void gfx_glow(float cx, float cy, float w, float h, unsigned color) {
    if (!g_glow.pixels) return;
    color = gfx_veiled(color);
    if (g_batching) {
        flush_strips();
        if (g_sprites == BATCH_SPRITES) flush_sprites();
        if (!g_sprite) {
            g_sprite = sceGuGetMemory(BATCH_SPRITES * 2 * sizeof(struct vtexc));
            if (!g_sprite) return;
        }
        /* The tint rides along per vertex so the batch needs no sceGuColor
           between sprites. The GE takes a sprite's colour from its second
           vertex; both carry it. */
        struct vtexc *b = g_sprite + g_sprites * 2;
        b[0].u = 0;         b[0].v = 0;         b[0].color = color;
        b[0].x = (short)(cx - w / 2); b[0].y = (short)(cy - h / 2); b[0].z = 0;
        b[1].u = GLOW_SIZE; b[1].v = GLOW_SIZE; b[1].color = color;
        b[1].x = (short)(cx + w / 2); b[1].y = (short)(cy + h / 2); b[1].z = 0;
        g_sprites++;
        return;
    }
    struct vtex *v = sceGuGetMemory(2 * sizeof(struct vtex));
    if (!v) return;
    bind(&g_glow);
    additive();
    sceGuColor(color);
    v[0].u = 0;         v[0].v = 0;
    v[0].x = (short)(cx - w / 2); v[0].y = (short)(cy - h / 2); v[0].z = 0;
    v[1].u = GLOW_SIZE; v[1].v = GLOW_SIZE;
    v[1].x = (short)(cx + w / 2); v[1].y = (short)(cy + h / 2); v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
    sceGuColor(0xFFFFFFFF);
    flat_state();
}

void gfx_texture_draw(const struct gfx_texture *t, int x, int y, int w, int h,
                     unsigned tint) {
    if (!t || !t->pixels) return;
    flush_batch();
    struct vtex *v = sceGuGetMemory(2 * sizeof(struct vtex));
    if (!v) return;
    bind(t);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuColor(gfx_veiled(tint));
    v[0].u = 0;              v[0].v = 0;
    v[0].x = x;              v[0].y = y;              v[0].z = 0;
    v[1].u = (short)t->w;    v[1].v = (short)t->h;
    v[1].x = x + w;          v[1].y = y + h;          v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
    sceGuColor(0xFFFFFFFF);
    flat_state();
}

void gfx_texture_draw_part(const struct gfx_texture *t, int sx, int sy,
                           int w, int h, float x, float y, unsigned tint) {
    if (!t || !t->pixels || w <= 0 || h <= 0) return;
    flush_batch();
    struct vtex *v = sceGuGetMemory(2 * sizeof(struct vtex));
    if (!v) return;
    bind(t);
    /* Nearest, against the linear the rest of the texture work wants: a
       sprite drawn at one to one has nothing to interpolate, and a filter
       that samples half a texel off is how a two-pixel stroke goes soft. */
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuColor(gfx_veiled(tint));
    short px = (short)(x + 0.5f), py = (short)(y + 0.5f);
    v[0].u = (short)sx;         v[0].v = (short)sy;
    v[0].x = px;                v[0].y = py;                v[0].z = 0;
    v[1].u = (short)(sx + w);   v[1].v = (short)(sy + h);
    v[1].x = (short)(px + w);   v[1].y = (short)(py + h);   v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
    sceGuColor(0xFFFFFFFF);
    flat_state();
}

void gfx_shade(float cx, float cy, float w, float h, int alpha) {
    if (!g_glow.pixels) return;
    flush_batch();
    struct vtex *v = sceGuGetMemory(2 * sizeof(struct vtex));
    if (!v) return;
    bind(&g_glow);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuColor(gfx_veiled(RGBA(0, 0, 0, alpha)));
    v[0].u = 0;         v[0].v = 0;
    v[0].x = (short)(cx - w / 2); v[0].y = (short)(cy - h / 2); v[0].z = 0;
    v[1].u = GLOW_SIZE; v[1].v = GLOW_SIZE;
    v[1].x = (short)(cx + w / 2); v[1].y = (short)(cy + h / 2); v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
    sceGuColor(0xFFFFFFFF);
    flat_state();
}

/* ------------------------------------------------------------------ card */

/* The camera sits CARD_Z in front of the origin with a 45 degree view, so
   this many world units span one screen pixel. Screen coordinates convert to
   the card's plane through it, and the card ends up exactly the size asked
   for when it faces the viewer. */
#define CARD_Z 3.0f
#define CARD_FOV 45.0f
#define PX (2.0f * CARD_Z * 0.41421356f / SCR_H)   /* tan(22.5 deg) */

struct v3t { float u, v; unsigned color; float x, y, z; };
struct v3c { unsigned color; float x, y, z; };

#define FMT3T (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define FMT3C (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

static void card_matrices(const struct gfx_card *c) {
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(CARD_FOV, (float)SCR_W / SCR_H, 0.5f, 50.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 pos = { (c->cx - SCR_W / 2) * PX, (SCR_H / 2 - c->cy) * PX, -CARD_Z };
    ScePspFVector3 rot = { c->pitch, c->yaw, 0.0f };
    sceGumTranslate(&pos);
    sceGumRotateXYZ(&rot);
}

/* A flat-coloured quad on the card's plane, corners given in card space. */
static void card_quad(float x0, float y0, float x1, float y1, float z,
                      unsigned tl, unsigned bl, unsigned tr, unsigned br) {
    struct v3c *v = sceGuGetMemory(4 * sizeof(struct v3c));
    if (!v) return;
    v[0].color = tl; v[0].x = x0; v[0].y = y0; v[0].z = z;
    v[1].color = bl; v[1].x = x0; v[1].y = y1; v[1].z = z;
    v[2].color = tr; v[2].x = x1; v[2].y = y0; v[2].z = z;
    v[3].color = br; v[3].x = x1; v[3].y = y1; v[3].z = z;
    sceGumDrawArray(GU_TRIANGLE_STRIP, FMT3C, 4, 0, v);
}

void gfx_clip(int x, int y, int w, int h) {
    flush_batch();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    /* This SDK's sceGuScissor takes a width and a height, whatever older
       code (intraFont among it) passes it as a far corner. */
    sceGuScissor(x, y, w, h);
}

void gfx_unclip(void) {
    flush_batch();
    sceGuScissor(0, 0, SCR_W, SCR_H);
}

/* ------------------------------------------------------------------ bloom */

/* Bloom, the way the era did it without a pixel shader. The frame, drawn,
   is read back as a texture -- the GE reads its own draw buffer as readily
   as anything else in VRAM -- and drawn small, which is the first blur; a
   subtraction takes the dark out of it, so only what is brighter than the
   threshold is left; a few additive passes a texel apart spread what is
   left; and the small picture is added back over the frame at full size,
   the stretch being the last blur. So every light on the screen bleeds
   into the room around it, the lit sign, the glints, a bright film, the
   letters, without any of them asking to. Two 128x64 targets in the VRAM
   the two frames leave free.

   Always on. What it costs: one read of the 480x272 frame at a quarter
   size, one flat quad, sixteen quads of 128x64 for the blur, and one
   quad back over the screen -- about a millisecond of the GE's sixteen
   a frame, measured on the emulator at a steady 60 with the water and
   the film playing under it. Should the hardware ever come up short, the
   spread passes are the first thing to halve: two instead of four costs
   a little softness and nothing else. */
#define BLOOM_W 128
#define BLOOM_H 64
#define BLOOM_A ((unsigned)(2 * FRAME_SIZE))            /* after the two frames */
#define BLOOM_B (BLOOM_A + BLOOM_W * BLOOM_H * 4)
#define BLOOM_FLOOR 0x9C                                /* what is darker than this does not glow: lights, not lit water */

static void *vram_abs(unsigned rel) {
    return (void *)((unsigned)sceGeEdramGetAddr() + (rel & 0x001FFFFF));
}

/* Where the GE draws next: a small target, or the frame again. */
static void bloom_target(unsigned rel, int w, int h) {
    sceGuDrawBufferList(GU_PSM_8888, (void *)rel, w);
    sceGuOffset(2048 - w / 2, 2048 - h / 2);
    sceGuViewport(2048, 2048, w, h);
    sceGuScissor(0, 0, w, h);
}

/* A picture in VRAM as the texture: the frame, or one of the small ones. */
static void bloom_source(unsigned rel, int tw, int th, int stride) {
    sceGuTexFlush();
    sceGuTexSync();
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(0, tw, th, stride, vram_abs(rel));
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
}

/* The source's (u0,v0)-(u1,v1), in texels, over the target's (x0,y0)-(x1,y1),
   modulated by one colour. */
static void bloom_quad(float u0, float v0, float u1, float v1,
                       float x0, float y0, float x1, float y1, unsigned color) {
    struct v3t *v = sceGuGetMemory(2 * sizeof(struct v3t));
    if (!v) return;
    v[0].u = u0; v[0].v = v0; v[0].color = color; v[0].x = x0; v[0].y = y0; v[0].z = 0;
    v[1].u = u1; v[1].v = v1; v[1].color = color; v[1].x = x1; v[1].y = y1; v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   2, 0, v);
}

/* One blur pass along one axis: from into to, four taps a quarter each at
   -1.5, -0.5, 0.5 and 1.5 texels times step, the first written and the
   rest added. Half-texel offsets make the bilinear filter average two
   texels a tap, so the four taps are a smooth tent over six texels with
   no gap in it -- taps a whole texel apart leave a comb, and a comb
   stretched to the screen is a row of dashes beside every light. */
static void bloom_spread(unsigned from, unsigned to, float dx, float dy, float step) {
    bloom_target(to, BLOOM_W, BLOOM_H);
    bloom_source(from, BLOOM_W, BLOOM_H, BLOOM_W);
    const float at[4] = { -1.5f, -0.5f, 0.5f, 1.5f };
    for (int i = 0; i < 4; i++) {
        float ox = dx * at[i] * step, oy = dy * at[i] * step;
        if (i == 0) sceGuDisable(GU_BLEND);
        else {
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
        }
        bloom_quad(ox, oy, BLOOM_W + ox, BLOOM_H + oy,
                   0, 0, BLOOM_W, BLOOM_H, 0xFF404040);
    }
}

void gfx_bloom(int strength) {
    if (strength <= 0 || !g_in_frame) return;
    flush_batch();
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_LIGHTING);

    /* Down: the frame into A, a quarter the size each way. */
    bloom_target(BLOOM_A, BLOOM_W, BLOOM_H);
    bloom_source((unsigned)g_draw, BUF_W, BUF_W, BUF_W);
    sceGuDisable(GU_BLEND);
    bloom_quad(0, 0, SCR_W, SCR_H, 0, 0, BLOOM_W, BLOOM_H, 0xFFFFFFFF);

    /* The floor: what is darker than it is taken down to nothing. */
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
    {
        struct vcol *v = sceGuGetMemory(2 * sizeof(struct vcol));
        if (v) {
            unsigned floor = 0xFF000000u | (BLOOM_FLOOR << 16) | (BLOOM_FLOOR << 8) | BLOOM_FLOOR;
            v[0].color = floor; v[0].x = 0;       v[0].y = 0;       v[0].z = 0;
            v[1].color = floor; v[1].x = BLOOM_W; v[1].y = BLOOM_H; v[1].z = 0;
            sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
        }
    }

    /* Spread: across, then down, then both again twice as wide. */
    bloom_spread(BLOOM_A, BLOOM_B, 1, 0, 1.0f);
    bloom_spread(BLOOM_B, BLOOM_A, 0, 1, 1.0f);
    bloom_spread(BLOOM_A, BLOOM_B, 1, 0, 2.0f);
    bloom_spread(BLOOM_B, BLOOM_A, 0, 1, 2.0f);

    /* Back over the frame, added, stretched to the screen. */
    bloom_target((unsigned)g_draw, SCR_W, SCR_H);
    sceGuDrawBufferList(GU_PSM_8888, g_draw, BUF_W);
    bloom_source(BLOOM_A, BLOOM_W, BLOOM_H, BLOOM_W);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
    unsigned k = strength > 255 ? 255 : (unsigned)strength;
    bloom_quad(0, 0, BLOOM_W, BLOOM_H, 0, 0, SCR_W, SCR_H, 0xFF000000u | (k << 16) | (k << 8) | k);

    sceGuColor(0xFFFFFFFF);
    flat_state();
}

/* ------------------------------------------------------------------- bake */

static int g_bake_w, g_bake_h, g_baking;

int gfx_bake_begin(int w, int h) {
    if (g_baking || w > SCR_W || h > SCR_H) return -1;
    g_bake_w = w;
    g_bake_h = h;
    g_baking = 1;
    sceGuStart(GU_DIRECT, g_list);
    sceGuScissor(0, 0, w, h);
    sceGuClearColor(0);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    return 0;
}

void gfx_bake_end(struct gfx_texture *into) {
    if (!g_baking) return;
    sceGuScissor(0, 0, SCR_W, SCR_H);
    if (into && into->pixels && into->tw >= g_bake_w && into->th >= g_bake_h) {
        void *src = (void *)((unsigned)sceGeEdramGetAddr() + ((unsigned)g_draw & 0x001FFFFF));
        sceGuCopyImage(GU_PSM_8888, 0, 0, g_bake_w, g_bake_h, BUF_W, src,
                       0, 0, into->tw, into->pixels);
        sceGuTexSync();
        into->w = g_bake_w;
        into->h = g_bake_h;
    }
    sceGuFinish();
    sceGuSync(0, 0);
    g_baking = 0;
}

void gfx_card_draw(const struct gfx_texture *t, const struct gfx_card *cc) {
    /* The veil takes the picture's own alpha down with everything else's. */
    struct gfx_card veiled = *cc;
    const struct gfx_card *c = &veiled;
    veiled.alpha = (int)((unsigned)cc->alpha * (unsigned)g_veil >> 8);
    float hw = c->w * PX / 2, hh = c->h * PX / 2;

    /* The shadow is flat, under the card, offset the way the card leans. */
    if (!c->bare)
        gfx_shade(c->cx + c->yaw * 40.0f, c->cy + 10.0f - c->pitch * 40.0f,
                  c->w + 60.0f, c->h + 60.0f, 150);

    card_matrices(c);
    flat_state();

    /* Frame: a hair wider than the picture, black. Around it a feather,
       a strip a pixel and a half wide going from the frame's black to
       nothing: the GE draws no edge smoothly, and a card turned a little
       has a staircase down every side, so the edge is not left to the
       polygon -- the frame's own edge is hidden under the feather's inner
       side, and the feather's outer side, being transparent, has no
       staircase to show. The corners are covered by running the top and
       bottom strips the feather's width past the sides. */
    float f = c->bare ? 0.0f : 1.5f * PX;
    if (!c->bare) {
        unsigned ink = gfx_veiled(RGBA(0, 0, 0, 200)), none = RGBA(0, 0, 0, 0);
        float r = 1.5f * PX;
        card_quad(-hw - f, hh + f, hw + f, -hh - f, -0.002f, ink, ink, ink, ink);
        card_quad(-hw - f - r, hh + f + r, hw + f + r, hh + f, -0.002f, none, ink, none, ink);          /* top */
        card_quad(-hw - f - r, -hh - f, hw + f + r, -hh - f - r, -0.002f, ink, none, ink, none);        /* bottom */
        card_quad(-hw - f - r, hh + f, -hw - f, -hh - f, -0.002f, none, none, ink, ink);                /* left */
        card_quad(hw + f, hh + f, hw + f + r, -hh - f, -0.002f, ink, ink, none, none);                  /* right */
    }

    if (t && t->pixels) {
        float u1 = (float)t->w / t->tw, v1 = (float)t->h / t->th;
        unsigned white = RGBA(255, 255, 255, c->alpha);
        bind(t);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

        /* Reflection first, so the card lies over its top edge. Mirrored in
           v, fading to nothing over reflect_h pixels. */
        if (c->reflect_h > 0) {
            float rh = c->reflect_h * PX;
            float rv = v1 * (float)c->reflect_h / c->h;
            unsigned top = RGBA(255, 255, 255, c->alpha * 30 / 100);
            unsigned bottom = RGBA(255, 255, 255, 0);
            struct v3t *r = sceGuGetMemory(4 * sizeof(struct v3t));
            if (r) {
                r[0].u = 0;  r[0].v = v1;      r[0].color = top;    r[0].x = -hw; r[0].y = -hh - f;      r[0].z = 0;
                r[1].u = 0;  r[1].v = v1 - rv; r[1].color = bottom; r[1].x = -hw; r[1].y = -hh - f - rh; r[1].z = 0;
                r[2].u = u1; r[2].v = v1;      r[2].color = top;    r[2].x = hw;  r[2].y = -hh - f;      r[2].z = 0;
                r[3].u = u1; r[3].v = v1 - rv; r[3].color = bottom; r[3].x = hw;  r[3].y = -hh - f - rh; r[3].z = 0;
                sceGumDrawArray(GU_TRIANGLE_STRIP, FMT3T, 4, 0, r);
            }
        }

        struct v3t *v = sceGuGetMemory(4 * sizeof(struct v3t));
        if (v) {
            v[0].u = 0;  v[0].v = 0;  v[0].color = white; v[0].x = -hw; v[0].y = hh;  v[0].z = 0;
            v[1].u = 0;  v[1].v = v1; v[1].color = white; v[1].x = -hw; v[1].y = -hh; v[1].z = 0;
            v[2].u = u1; v[2].v = 0;  v[2].color = white; v[2].x = hw;  v[2].y = hh;  v[2].z = 0;
            v[3].u = u1; v[3].v = v1; v[3].color = white; v[3].x = hw;  v[3].y = -hh; v[3].z = 0;
            sceGumDrawArray(GU_TRIANGLE_STRIP, FMT3T, 4, 0, v);
        }
        flat_state();
    } else {
        card_quad(-hw, hh, hw, -hh, 0.0f, RGBA(255, 255, 255, 10), RGBA(255, 255, 255, 3),
                  RGBA(255, 255, 255, 10), RGBA(255, 255, 255, 3));
    }

    /* Glass: a hairline of light along the top edge, and the sweep -- a
       soft diagonal band of light crossing the picture, added on. */
    if (!c->bare)
        card_quad(-hw, hh + f, hw, hh - 1.0f * PX, 0.001f,
                  RGBA(255, 255, 255, 40), RGBA(255, 255, 255, 40),
                  RGBA(255, 255, 255, 130), RGBA(255, 255, 255, 130));
    if (c->gloss >= 0.0f && c->gloss <= 1.0f && c->bare && t && t->pixels) {
        /* On lettering the band is drawn through the texture, so the light
           crosses the letters and nothing else. */
        float u1 = (float)t->w / t->tw, v1 = (float)t->h / t->th;
        float band = hw * 0.45f;
        float x = -hw - band + c->gloss * (2 * hw + 2 * band);
        float lean = hh * 0.8f;
        unsigned clear = RGBA(255, 255, 255, 0), lit = RGBA(255, 255, 255, 150);
        struct v3t *g = sceGuGetMemory(6 * sizeof(struct v3t));
        if (g) {
            float xs[6] = { x - band + lean, x - band - lean, x + lean, x - lean, x + band + lean, x + band - lean };
            for (int i = 0; i < 6; i++) {
                int top = !(i & 1);
                g[i].x = xs[i]; g[i].y = top ? hh : -hh; g[i].z = 0.002f;
                g[i].u = (xs[i] + hw) / (2 * hw) * u1; g[i].v = top ? 0 : v1;
                g[i].color = (i == 2 || i == 3) ? lit : clear;
            }
            bind(t);
            additive();
            sceGumDrawArray(GU_TRIANGLE_STRIP, FMT3T, 6, 0, g);
        }
        flat_state();
    } else if (c->gloss >= 0.0f && c->gloss <= 1.0f) {
        additive();
        float band = hw * 0.55f;
        float x = -hw - band + c->gloss * (2 * hw + 2 * band);
        float lean = hh * 0.6f;
        unsigned clear = RGBA(255, 255, 255, 0), lit = RGBA(255, 255, 255, 70);
        struct v3c *g = sceGuGetMemory(6 * sizeof(struct v3c));
        if (g) {
            g[0].color = clear; g[0].x = x - band + lean; g[0].y = hh;  g[0].z = 0.002f;
            g[1].color = clear; g[1].x = x - band - lean; g[1].y = -hh; g[1].z = 0.002f;
            g[2].color = lit;   g[2].x = x + lean;        g[2].y = hh;  g[2].z = 0.002f;
            g[3].color = lit;   g[3].x = x - lean;        g[3].y = -hh; g[3].z = 0.002f;
            g[4].color = clear; g[4].x = x + band + lean; g[4].y = hh;  g[4].z = 0.002f;
            g[5].color = clear; g[5].x = x + band - lean; g[5].y = -hh; g[5].z = 0.002f;
            sceGumDrawArray(GU_TRIANGLE_STRIP, FMT3C, 6, 0, g);
        }
        flat_state();
    }
}

void gfx_texture_free(struct gfx_texture *t) {
    if (!t) return;
    free(t->pixels);
    memset(t, 0, sizeof(*t));
}

/* Where the front buffer's pixels come from depends on who drew them. The
   debug screen writes VRAM with the CPU and the CPU can read it straight
   back. The GE's output is different: on PPSSPP the emulated VRAM behind a
   GE-rendered frame is not kept current, and a CPU read hands back whatever
   was last written there -- frames old. Copying through the GE itself is
   what games do for their save icons, and it is the path the emulator keeps
   honest. */
static unsigned __attribute__((aligned(16))) g_readback[SCR_W * SCR_H];

static const unsigned *front_pixels(int *stride) {
    void *top = 0;
    int format = 0;
    *stride = BUF_W;
    sceDisplayWaitVblankStart();
    if (sceDisplayGetFrameBuf(&top, stride, &format,
                              PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 || !top) {
        logline("screenshot: no framebuffer");
        return 0;
    }
    if (format != PSP_DISPLAY_PIXEL_FORMAT_8888) {
        logline("screenshot: pixel format %d not 8888", format);
        return 0;
    }
    if (!g_up)
        return (const unsigned *)((unsigned)top | 0x40000000);

    void *src = (void *)((unsigned)top & 0x1FFFFFFF);
    sceGuStart(GU_DIRECT, g_list);
    sceGuCopyImage(GU_PSM_8888, 0, 0, SCR_W, SCR_H, *stride, src,
                   0, 0, SCR_W, g_readback);
    sceGuTexSync();
    sceGuFinish();
    sceGuSync(0, 0);
    *stride = SCR_W;
    return (const unsigned *)((unsigned)g_readback | 0x40000000);
}

void gfx_screenshot(const char *path) {
    enum { W = SCR_W, H = SCR_H };
    int stride;
    const unsigned *pixels = front_pixels(&stride);
    if (!pixels) return;

    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    unsigned rowbytes = W * 3;
    unsigned datasize = rowbytes * H;
    unsigned char hdr[54] = { 'B', 'M' };
    unsigned value;
    value = 54 + datasize; memcpy(hdr + 2, &value, 4);
    value = 54;            memcpy(hdr + 10, &value, 4);
    value = 40;            memcpy(hdr + 14, &value, 4);
    value = W;             memcpy(hdr + 18, &value, 4);
    value = H;             memcpy(hdr + 22, &value, 4);
    hdr[26] = 1; hdr[28] = 24;
    value = datasize;      memcpy(hdr + 34, &value, 4);
    sceIoWrite(fd, hdr, sizeof(hdr));
    static unsigned char row[W * 3];
    for (int y = H - 1; y >= 0; y--) {
        const unsigned *src = pixels + y * stride;
        for (int x = 0; x < W; x++) {
            unsigned px = src[x];
            row[x * 3 + 0] = (px >> 16) & 0xff;
            row[x * 3 + 1] = (px >> 8) & 0xff;
            row[x * 3 + 2] = px & 0xff;
        }
        sceIoWrite(fd, row, sizeof(row));
    }
    sceIoClose(fd);
}
