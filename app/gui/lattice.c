#include <math.h>

#include "gui/lattice.h"

/* The surface runs to the horizon in every direction: rows a constant factor
   apart in depth until they are two and a half pixels under the one behind
   them, and beyond the sixty-one columns in front of the viewer eight more
   far out to either side, which only come into view where the rows have
   narrowed enough. The sixty-one are the field the sweep fills; the
   sixteen are open sea.

   The light is the GE's now, so a crossing costs a normal and a colour
   rather than a page of arithmetic, and the field is as dense as the eye
   can use: every row it has stands a good two pixels tall on screen, and
   none of it is spent on rows too thin to see. */
#define NX_INNER 61
#define NX_OUTER 16
#define NX (NX_INNER + NX_OUTER)
#define J0 (NX_OUTER / 2)       /* first inner column */
#define J1 (J0 + NX_INNER - 1)  /* last inner column */
#define NZ 60                   /* lines into the distance */
/* Where the rows lie. A row's depth is read off the pixel it lands on:
   starting a crest's height under the bottom edge of the screen, each row a
   constant fraction nearer the horizon than the last until that fraction is
   thinner than MIN_TALL pixels, after which they are MIN_TALL apart. Kept
   geometric where it can be, because that is what perspective looks like;
   held apart where it cannot, because two rows closer than a pixel and a
   half share pixel rows and additive blending paints the shared edge twice
   -- and because a row a pixel tall is a row of the field spent on nothing.
   The fraction is found at init so that the last row lands on Z_FAR. */
#define Z_NEAR 0.70f
/* Where the water stops. Not at the horizon: past this the ripple tile is
   laid down faster than its coarsest level can follow -- tens of texels to
   the pixel, which is one bright texel row drawn as a dash -- and the rows
   are close enough together that a crest presses one against the next. The
   old field ran to forty and faded out everything past about here for the
   same reason; this one does not draw it at all, and the horizon's own
   glow stands in for the few pixels between. */
#define Z_FAR 20.0f
#define MIN_TALL 2.6f
/* The swell and the fades were written against a field of thirty rows
   spaced geometrically from here to Z_FAR, so both are read off the depth
   through the row that field would have put there -- the same shape on the
   water however the rows are laid out now. */
#define Z_REF 0.6f
#define STARS 22
#define SPECKS 12

/* The water in the world the GE draws it in: how high the eye sits over the
   surface, what one unit of the height field is worth there, and how many
   ripple tiles go to a unit of width. The height a near crest may reach is
   capped in pixels, so a tall swell does not climb over the footer. */
#define EYE_Y (150.0f / GFX_FOCAL)
#define H_SCALE (52.0f / GFX_FOCAL)
#define TILES 1.6f
/* The tile goes into the distance at the density the first row of the old
   field gave it; the rows have moved since, the tile has not. */
#define V_REF 0.6f
#define RIPPLE_TEXELS 256.0f    /* one tile, as gfx builds it */
#define DY_CAP 42.0f
/* Past this much width per unit of depth a row is far off screen, and its
   corner can sit there rather than thousands of pixels out. */
#define XLIM 0.9167f
/* How much steeper the surface is to the light than it is to the eye. The
   swell is long and low -- a crest leans about five degrees -- and a light
   lying on the horizon would find almost nothing in that; the water the eye
   knows has black troughs and a white edge on every crest. So the normal is
   leant this much further than the shape it came from, which is what a bump
   map is and costs the same as not doing it. */
#define SLOPE_GAIN 2.5f
#define DZ_LEAST 0.18f

/* The wave equation on the cells. C is c^2 dt^2 / dx^2 and has to stay well
   under a half or the surface explodes; it also sets the speed, and a long
   ocean swell is slow. The damping is what stops a ring from ringing for
   ever. A ring runs at sqrt(C) cells a frame; the cells are half the width
   they were, so C is what keeps a ring crossing the same water in the same
   time -- and 0.35 is still comfortably under the half. */
#define WAVE_C 0.35f
#define WAVE_DAMP 0.995f
#define WAVE_MAX 2.5f

/* What the source covers in cells, and how many frames of standing over a
   cell it takes to fill it. Sized so a sweep of the field at stick speed
   leaves no dry cells behind between passes -- which is a share of the
   field, so the radii grew with it. */
#define POUR_RX 5.8f
#define POUR_RZ 4.5f
#define POUR_RATE 0.34f
#define POUR_DENT 0.16f

/* The colour front. It runs in the same cells the ring does and at the same
   speed -- sqrt(WAVE_C), three fifths of a cell a frame -- so the edge of
   the colour sits on the ring that carried it out, and the corners of what
   is on screen have turned in about a second and a half. Its edge is soft
   over a tenth of the field's width, which is seven of the inner
   columns. */
#define FRONT_C 0.59f
#define FRONT_W 7.0f

/* What water is where no light reaches it. */
static const struct rgb DEEP = { 3, 8, 24 };

/* Deterministic and nothing to do with the entropy pool. */
static unsigned g_lcg = 0x9E3779B9;
static float frand(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return (g_lcg >> 8) / 16777216.0f;
}

static struct { float x, y, vx, vy, size, phase; } g_stars[STARS];
static struct { float x, y, vy, size, phase; } g_specks[SPECKS];

/* A sine costs a few hundred cycles out of libm and the surface wants
   thousands of them a frame, so it comes out of a table read with a linear
   step between entries, which is a hundredth of a level of alpha off --
   nothing an eye or an 8-bit channel can hold. */
#define SIN_N 256

static float g_sin[SIN_N + 1];

static float fsin(float a) {
    float p = a * (SIN_N / 6.2831853f);
    int i = (int)p;
    if (p < 0) i--;                     /* the cast truncates toward zero */
    float f = p - i;
    i &= SIN_N - 1;
    return g_sin[i] + (g_sin[i + 1] - g_sin[i]) * f;
}

static float fcos(float a) { return fsin(a + 1.5707963f); }

/* The rows never move, so everything that depends only on the row is worked
   out once: where it is, what a unit of height is worth there in pixels and
   in the world, how much of the light it still carries, and the colour a
   crossing on it hands the GE while the whole surface is one colour. */
static struct {
    float z, y0, yh, near, near2;
    float f, zf;                /* pixels per unit of width, and its inverse */
    float v;                    /* where the row sits in the ripple tile */
    float lod;                  /* the mip level a texel per pixel wants */
    float thin;                 /* 0 where the row is under a pixel tall */
    float a1, a2;               /* where the two swells stand on this row */
    float kz;                   /* a slope away, as the normal wants it */
    float least;                /* how far it must stay below the row behind */
    float alpha;                /* what a crossing on it is drawn with */
    float dim;                  /* how much of the light it still shows */
    unsigned color;             /* that alpha, over the dimmed white */
    /* The columns this row is drawn over. A row near the viewer is a
       thousand pixels wide at the sides of the field and the screen is four
       hundred and eighty: the corners past the edge are placed, lit and sent
       to the GE for nothing. Each row keeps the columns that reach its own
       screen and a margin, widened to what the row behind it needs, since
       the two share a strip. The simulation still runs over the whole
       field -- a ring has to be able to leave. */
    short j0, j1;
    int at;                     /* where its strip's indices start */
} g_row[NZ];

/* The first rows are under the bottom edge of the screen, so the water runs
   off it rather than stopping on it; g_row0 is where the screen starts, and
   the sweep's field and the light both measure depth from there. */
static int g_row0;

static float g_u[NX];       /* across, sorted; -1..1 in front, wider outside */
static float g_ut[NX];      /* the same, in tiles of the ripple */
static float g_kx[NX];      /* a slope across, as the normal wants it */

/* The height field, its velocity, and how much of each cell is water at all.
   Everything else on screen is derived from these three a frame at a time. */
static float g_h[NZ][NX], g_v[NZ][NX], g_wet[NZ][NX];

/* The surface as the swell and the simulation left it, and the pixel every
   crossing landed on -- which the lines, the crossings, the source and the
   picture lying in the water all read. The mesh the GE is handed is written
   in the same pass, so the world position is not kept twice. */
static float g_hh[NZ][NX];              /* the swell plus the simulation */
static float g_x[NZ][NX], g_y[NZ][NX];  /* and where that lands on screen */
static struct gfx_water_vertex *g_mesh;
static unsigned short *g_index;
static int g_dry;                       /* some cell is not water yet */

/* The sweep's source: where it is over the field, and whether to draw it. */
static struct { float fx, fz; int on; } g_source;

/* ------------------------------------------------------------- the colour */

/* The room's colour used to be one number for the whole surface: the palette
   was built out of it, so every one of its 256 entries was already tinted
   and every texel on the water turned at the same moment. The colour lives
   in the crossings now. Each one carries its own, the palette is built from
   the brightest colour in play, and a vertex hands the GE its own colour as
   a fraction of that one -- texel times vertex is what the GE draws, so the
   fraction puts the colour back. While one colour is on the water every
   fraction is 1 and the frame is exactly the frame that was drawn before.
   Two colours mean a front between them, and the front is nothing more than
   which colour a crossing is holding. */
static struct rgb g_vt[NZ][NX];         /* what each crossing is holding */
static struct rgb g_ref = { 255, 255, 255 };    /* what the palette is built on */
/* While there is no front the crossings are not written at all: they all
   hold the same colour, nothing reads them, and writing five thousand
   copies of one colour a frame is a pass over sixty kilobytes to say
   nothing. They are filled in when a front starts, since what it leaves
   behind it is what they were holding. */
static struct rgb g_held = { 255, 255, 255 };
static int g_uniform = 1;

/* The front: where it started, how far it has got, and what it is bringing. */
static struct rgb g_bring;
static float g_front_r, g_front_end, g_front_j, g_front_i;
static int g_front_on;

/* Where the last drop fell, which is where the next front starts. Until one
   has, the middle of the field. */
static float g_touch_j = J0 + 0.5f * (NX_INNER - 1);
static float g_touch_i;

/* The eased colour as it came in last frame, and how far it moved getting
   there: a colour on its way to a new one moves less every frame, so a step
   bigger than the last is a new one having been picked. */
static struct rgb g_seen;
static float g_seen_step;
static int g_seen_ok;
static int g_told;                      /* lattice_tint() does the telling */

static void front_start(struct rgb target) {
    g_front_on = 1;
    g_front_r = 0.0f;
    g_front_j = g_touch_j;
    g_front_i = g_touch_i;
    g_bring = target;
    /* Done when the furthest corner of the field is inside it. */
    float dj = g_front_j > (NX - 1) * 0.5f ? g_front_j : (NX - 1) - g_front_j;
    float di = g_front_i > (NZ - 1) * 0.5f ? g_front_i : (NZ - 1) - g_front_i;
    g_front_end = sqrtf(dj * dj + di * di) + FRONT_W;
}

void lattice_tint(struct rgb target) {
    /* Told again what it is already bringing, a front would start over at the
       drop every frame and never leave it, so only a colour that is news
       here starts one. Saying it once per selection is all it wants. */
    g_told = 1;
    if (target.r == g_bring.r && target.g == g_bring.g && target.b == g_bring.b)
        return;
    front_start(target);
}

/* Smooth at both ends, so the edge of the front has no line in it. */
static float ease(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c * c * (3.0f - 2.0f * c);
}

/* Carry the front one frame outward. A crossing it has passed holds the new
   colour, one it has not holds whatever it held before -- the colour from
   before this front, or a half-crossed mix a front that never finished left
   there. In the edge itself a crossing is moved the rest of the way from
   where the edge had it last frame to where the edge has it now, which is
   how a front started over a half-turned field needs no memory of the one
   before it. */
static void spread(struct rgb tint) {
    float rp = g_front_r;
    if (g_front_on) {
        g_front_r += FRONT_C;
        if (g_front_r >= g_front_end) g_front_on = 0;
    }
    if (!g_front_on) {
        /* One colour, and it is the one the room is easing to. Nobody asks
           which colour a crossing is holding while they are all holding
           this one. */
        g_held = tint;
        g_uniform = 1;
        g_ref = tint;
        return;
    }
    /* The first frame of a front is where the field it runs over is written
       down: everything outside the front is still holding what it held. */
    if (g_uniform) {
        for (int i = 0; i < NZ; i++)
            for (int j = 0; j < NX; j++) g_vt[i][j] = g_held;
        g_uniform = 0;
    }
    /* Nobody told us the colour the room is heading for, so the front takes
       it as it arrives: the easing is all but over in half a second and the
       front takes three times that, so what it carries is the new colour. */
    if (!g_told) g_bring = tint;
    if (g_bring.r > g_ref.r) g_ref.r = g_bring.r;
    if (g_bring.g > g_ref.g) g_ref.g = g_bring.g;
    if (g_bring.b > g_ref.b) g_ref.b = g_bring.b;

    float r = g_front_r, rr = r * r;
    float in = r - FRONT_W;
    float inn = in > 0.0f ? in * in : -1.0f;
    for (int i = 0; i < NZ; i++) {
        float di = i - g_front_i, dii = di * di;
        if (dii >= rr) continue;
        float span = sqrtf(rr - dii);
        int j0 = (int)(g_front_j - span), j1 = (int)(g_front_j + span) + 1;
        if (j0 < 0) j0 = 0;
        if (j1 > NX - 1) j1 = NX - 1;
        for (int j = j0; j <= j1; j++) {
            float dj = j - g_front_j;
            float d2 = dii + dj * dj;
            if (d2 >= rr) continue;
            if (d2 <= inn) { g_vt[i][j] = g_bring; continue; }
            float d = sqrtf(d2);
            float c = ease((r - d) * (1.0f / FRONT_W));
            float cp = ease((rp - d) * (1.0f / FRONT_W));
            float a = c >= 1.0f ? 1.0f : (c - cp) / (1.0f - cp);
            if (a <= 0.0f) continue;
            g_vt[i][j] = rgb_mix(g_vt[i][j], g_bring, a);
        }
    }
}

/* The colour arrives eased, a frame at a time, and never says where it is
   going. Until somebody says, the surface watches it: a step bigger than the
   step before it is a new colour having been picked, because easing toward
   one colour only ever slows down. */
static void watch(struct rgb tint) {
    float step = fabsf(tint.r - g_seen.r) + fabsf(tint.g - g_seen.g)
               + fabsf(tint.b - g_seen.b);
    if (!g_seen_ok) { g_seen_ok = 1; step = 0.0f; }
    else if (step > g_seen_step * 1.3f + 4.0f) front_start(tint);
    g_seen_step = step;
    g_seen = tint;
}

/* Motes in the air over the horizon, rising slowly through the sky and
   never in front of the water: something drifting across the surface
   reads as dirt on it. */
static void speck_reset(int i, int anywhere) {
    g_specks[i].x = 20 + frand() * (SCR_W - 40);
    g_specks[i].y = anywhere ? 8 + frand() * (GFX_HORIZON - 16) : GFX_HORIZON - 4;
    g_specks[i].vy = 0.05f + frand() * 0.12f;
    g_specks[i].size = 4 + frand() * 8;
    g_specks[i].phase = frand() * 6.283f;
}

void lattice_init(void) {
    for (int i = 0; i <= SIN_N; i++) g_sin[i] = sinf(i * (6.2831853f / SIN_N));

    /* The rows, laid out in pixels. d is how far a row's own level is under
       the horizon, which is what perspective makes of its depth; the step
       from one to the next is a fraction of it until that falls under
       MIN_TALL. The fraction that lands the last row on Z_FAR is found by
       halving: it is one number, once, and writing it down would only mean
       writing it down again the next time a row is added. */
    float d[NZ];
    float lo = 0.0f, hi = 0.5f;
    float d_near = EYE_Y * GFX_FOCAL / Z_NEAR, d_far = EYE_Y * GFX_FOCAL / Z_FAR;
    for (int pass = 0; pass < 40; pass++) {
        float step = 0.5f * (lo + hi);
        d[0] = d_near;
        for (int i = 1; i < NZ; i++) {
            float fall = d[i - 1] * step;
            d[i] = d[i - 1] - (fall > MIN_TALL ? fall : MIN_TALL);
        }
        if (d[NZ - 1] < d_far) hi = step; else lo = step;
    }

    for (int i = 0; i < NZ; i++) {
        float z = EYE_Y * GFX_FOCAL / d[i];
        float inv = 1.0f / z;
        g_row[i].z = z;
        g_row[i].y0 = GFX_HORIZON + d[i];
        g_row[i].yh = H_SCALE * GFX_FOCAL * inv;
        g_row[i].f = GFX_FOCAL * inv;
        g_row[i].zf = z * (1.0f / GFX_FOCAL);
        /* The two long swells stand where they stood when a row was a step
           in the log of the depth, so they keep their wavelength in the
           world whatever the rows do. */
        float turns = logf(z / Z_REF);
        g_row[i].a1 = 0.55f * 21.0f / logf(Z_FAR / Z_REF) * turns;
        g_row[i].a2 = 0.95f * 21.0f / logf(Z_FAR / Z_REF) * turns;
        /* The tile goes into the distance by the log of the depth, not the
           depth: laid down by depth alone, a far row a few pixels tall
           would cross hundreds of texel rows, and one bright texel row is
           a streak across the screen. This keeps the front row's density
           and lets the tile grow with distance, which the perspective
           takes back out. */
        g_row[i].v = TILES * V_REF * logf(z);
        /* Texels per pixel, across and away, both grow with depth: across
           it is the tile's density over the pixels a unit of width gets,
           away it is the tile's density over the pixels a unit of depth
           gets, and the coarser of the two picks the level. */
        float across = TILES * RIPPLE_TEXELS * z / GFX_FOCAL;
        float away = TILES * V_REF * RIPPLE_TEXELS / z * z * z / (EYE_Y * GFX_FOCAL);
        float dense = across > away ? across : away;
        g_row[i].lod = dense > 1.0f ? logf(dense) * (1.0f / 0.6931472f) : 0.0f;
    }
    /* Where the screen starts, and how deep a row is measured from there:
       by the log of the depth, as the rows themselves used to be spaced, so
       the light fades down to the horizon exactly as it did. */
    g_row0 = 0;
    while (g_row0 + 1 < NZ && g_row[g_row0].y0 > SCR_H) g_row0++;
    g_touch_i = g_row0 + 0.35f * (NZ - 1 - g_row0);
    float turn0 = logf(g_row[g_row0].z / Z_REF), span = logf(Z_FAR / Z_REF) - turn0;
    for (int i = 0; i < NZ; i++) {
        float depth = (logf(g_row[i].z / Z_REF) - turn0) / span;
        if (depth < 0.0f) depth = 0.0f;
        g_row[i].near = 1.0f - depth;
        g_row[i].near2 = (1.0f - depth) * (1.0f - depth);
        /* A row under a pixel tall cannot hold a crest to itself, and where
           two strips share an edge the rasteriser may paint it from both;
           added on, that is a line of bright dots. None of the rows is that
           thin now -- MIN_TALL sees to it -- but the fade stays, since what
           it is really saying is that the far water has no shape left. */
        float tall = i + 1 < NZ ? g_row[i].y0 - g_row[i + 1].y0 : 0.0f;
        float thin = (tall - 1.0f) / 2.5f;
        g_row[i].thin = thin < 0.0f ? 0.0f : thin > 1.0f ? 1.0f : thin;
        /* Not a sliver: a row pressed against the one behind it keeps a
           third of its natural height, and never less than a pixel and a
           half. */
        float least = tall * 0.35f;
        g_row[i].least = least < 1.5f ? 1.5f : least;
        g_row[i].alpha = 200.0f * (0.35f + 0.65f * g_row[i].near) * g_row[i].thin;
        /* Contrast falling into haze toward the horizon, which is most of
           what reads as distance on water -- and what keeps the far rows
           quiet. They are two pixels apart and the swell moves them two
           pixels, so a crest there presses one row against the next and the
           strip between them lands on a single row of pixels; drawn twice,
           once from each side, that pixel is painted twice. Bright, that is
           a dash on the horizon. Dim, it is haze. */
        float dim = 0.32f + 0.68f * g_row[i].near2;
        g_row[i].dim = dim;
        int grey = (int)(255.0f * dim);
        g_row[i].color = RGBA(grey, grey, grey, (int)g_row[i].alpha);
        /* A slope away, turned into the normal's own units: how much world
           height a step to the next row is worth, over how far that step
           goes. It carries the fade with distance -- the shading on a row
           two pixels tall reads as a dashed line, so the far rows are
           handed a surface that is flat. */
        int im = i ? i - 1 : 0, ip = i + 1 < NZ ? i + 1 : NZ - 1;
        /* Never over a step shorter than this. The swell keeps its place in
           the log of the depth, so close to the viewer its wavelength in the
           world is short and its slopes steep; read literally, the near rows
           would stand at twice the angle of the far ones and the light would
           find them either white or black with an edge between. */
        float dz = g_row[ip].z - g_row[im].z;
        if (dz < DZ_LEAST) dz = DZ_LEAST;
        g_row[i].kz = SLOPE_GAIN * H_SCALE * g_row[i].near2 / dz;
    }
    /* Out to fourteen: the far rows are twenty deep and the screen's edge
       is a third of the depth out, so anything less than nine leaves a
       corner of the horizon bare. The last of them is past the screen
       altogether and is the field's own edge, where a ring turns round out
       of sight. */
    static const float OUTER[NX_OUTER / 2] = { 1.25f, 1.6f, 2.1f, 2.8f, 4.0f, 6.0f, 9.0f, 14.0f };
    int j = 0;
    for (int k = NX_OUTER / 2 - 1; k >= 0; k--) g_u[j++] = -OUTER[k];
    for (int k = 0; k < NX_INNER; k++) g_u[j++] = (float)k / (NX_INNER - 1) * 2.0f - 1.0f;
    for (int k = 0; k < NX_OUTER / 2; k++) g_u[j++] = OUTER[k];
    for (j = 0; j < NX; j++) {
        int jm = j ? j - 1 : 0, jp = j + 1 < NX ? j + 1 : NX - 1;
        float dx = g_u[jp] - g_u[jm];
        g_kx[j] = SLOPE_GAIN * H_SCALE / (dx > 1e-4f ? dx : 1e-4f);
        /* Where the column sits in the tile. The sway and the tile's own
           creep are the same for every corner on the surface, so they go to
           the GE once as a texture offset rather than into five thousand
           corners. Off to the sides a corner is pinned at XLIM and its own
           place in the tile is lost with it, but a corner pinned there is
           six hundred pixels past the edge of the screen. */
        g_ut[j] = g_u[j] * TILES;
    }

    /* What of each row reaches the screen: the columns whose corner lands
       inside it, with a margin for the room's sway and for the corner just
       outside that the strip needs to cover the edge. Then widened to the
       row behind, which is further off and so holds more of the field in
       the same pixels; the two are drawn as one strip and the strip is cut
       to the wider of them. */
    for (int i = 0; i < NZ; i++) {
        float lim = (SCR_W / 2.0f + 12.0f) / g_row[i].f + 0.07f;
        int lo = 0, hi = NX - 1;
        while (lo + 1 < NX && g_u[lo + 1] <= -lim) lo++;
        while (hi > 0 && g_u[hi - 1] >= lim) hi--;
        g_row[i].j0 = (short)lo;
        g_row[i].j1 = (short)hi;
    }
    for (int i = 0; i < NZ - 1; i++) {
        if (g_row[i + 1].j0 < g_row[i].j0) g_row[i].j0 = g_row[i + 1].j0;
        if (g_row[i + 1].j1 > g_row[i].j1) g_row[i].j1 = g_row[i + 1].j1;
    }

    /* The mesh, and the strips cut out of it: two rows at a time, every
       crossing of the near row followed by the one behind it. The corners
       are written once a frame and pointed at twice. */
    g_mesh = gfx_water_mesh(NZ * NX);
    g_index = gfx_water_index((NZ - 1) * NX * 2);
    if (g_mesh)
        for (int k = 0; k < NZ * NX; k++) {
            static const struct gfx_water_vertex zero;
            g_mesh[k] = zero;
        }
    int at = 0;
    if (g_index)
        for (int i = 0; i < NZ - 1; i++) {
            g_row[i].at = at;
            for (int k = g_row[i].j0; k <= g_row[i].j1; k++) {
                g_index[at++] = (unsigned short)(i * NX + k);
                g_index[at++] = (unsigned short)((i + 1) * NX + k);
            }
        }

    for (int i = 0; i < STARS; i++) {
        g_stars[i].x = frand() * SCR_W;
        g_stars[i].y = 6 + frand() * (GFX_HORIZON - 30);
        g_stars[i].size = 3 + frand() * 6;
        g_stars[i].phase = frand() * 6.283f;
        /* Adrift, each its own way, a few pixels a second. */
        g_stars[i].vx = (frand() - 0.5f) * 0.12f;
        g_stars[i].vy = (frand() - 0.5f) * 0.05f;
    }
    for (int i = 0; i < SPECKS; i++) speck_reset(i, 1);

    /* Open water unless somebody asks for the sweep's empty field. */
    for (int i = 0; i < NZ; i++)
        for (int k = 0; k < NX; k++) {
            g_h[i][k] = g_v[i][k] = 0.0f;
            g_wet[i][k] = 1.0f;
        }
    g_source.on = 0;
}

void lattice_dry(void) {
    for (int i = 0; i < NZ; i++)
        for (int j = 0; j < NX; j++) g_h[i][j] = g_v[i][j] = g_wet[i][j] = 0.0f;
}

void lattice_settle(void) {
    g_source.on = 0;
    for (int i = 0; i < NZ; i++)
        for (int j = 0; j < NX; j++) g_wet[i][j] = 1.0f;
}

/* A dent in the surface, which the wave equation then turns into a ring. */
static void dent(float jf, float rf, float depth, float radius) {
    float inv = 1.0f / (radius * radius);
    int i0 = (int)(rf - radius), i1 = (int)(rf + radius) + 1;
    int j0 = (int)(jf - radius), j1 = (int)(jf + radius) + 1;
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > NZ - 1) i1 = NZ - 1;
    if (j1 > NX - 1) j1 = NX - 1;
    for (int i = i0; i <= i1; i++) {
        float di = i - rf;
        for (int j = j0; j <= j1; j++) {
            float dj = j - jf;
            float d = (di * di + dj * dj) * inv;
            if (d >= 1.0f) continue;
            g_h[i][j] -= depth * (1.0f - d) * (1.0f - d) * g_wet[i][j];
        }
    }
}

void lattice_touch(float x) {
    float jf = J0 + x * (NX_INNER - 1) + (frand() - 0.5f) * 7.0f;
    float rf = g_row0 + (0.12f + frand() * 0.5f) * (NZ - 1 - g_row0);
    dent(jf, rf, 1.5f + frand() * 1.1f, 5.1f + frand() * 4.2f);
    /* The room's next colour spreads from where the drop fell, so the front
       and the ring leave together. */
    g_touch_j = jf;
    g_touch_i = rf;
}

void lattice_stir(float x, float y) {
    float push = x * x + y * y;
    if (push < 0.04f) return;                   /* the dead zone */
    if (push > 1.0f) push = 1.0f;
    /* Across the field with the stick, and stick forward is out toward
       the horizon. Kept off the very front rows, which are under the
       screen's bottom edge. */
    float fx = 0.5f + x * 0.42f;
    float fz = 0.42f - y * 0.34f;
    float jf = J0 + fx * (NX_INNER - 1) + (frand() - 0.5f) * 1.9f;
    float rf = g_row0 + fz * (NZ - 1 - g_row0) + (frand() - 0.5f) * 1.2f;
    /* A light press each frame rather than a drop: the wave equation adds
       them up into a wake, and the cap on the height keeps a stick held
       against its stop from digging a hole. */
    dent(jf, rf, 0.18f + 0.45f * push, 4.6f + 2.8f * push);
}

float lattice_pour(float fx, float fz, int pouring) {
    g_source.fx = fx;
    g_source.fz = fz;
    g_source.on = 1;

    float jf = J0 + fx * (NX_INNER - 1);
    float rf = g_row0 + fz * (NZ - 1 - g_row0);
    if (pouring) {
        for (int i = g_row0; i < NZ; i++) {
            float di = (i - rf) / POUR_RZ;
            if (di * di >= 1.0f) continue;
            for (int j = J0; j <= J1; j++) {
                float dj = (j - jf) / POUR_RX;
                float d = di * di + dj * dj;
                if (d >= 1.0f) continue;
                float w = g_wet[i][j] + POUR_RATE * (1.0f - d * 0.5f);
                g_wet[i][j] = w > 1.0f ? 1.0f : w;
            }
        }
        dent(jf, rf, POUR_DENT, 2.8f);
    }
    /* Past the sides of the field, and under the bottom edge of the screen,
       the sea simply carries on. */
    for (int i = g_row0; i < NZ; i++) {
        for (int j = 0; j < J0; j++) g_wet[i][j] = g_wet[i][J0];
        for (int j = J1 + 1; j < NX; j++) g_wet[i][j] = g_wet[i][J1];
    }
    for (int i = 0; i < g_row0; i++)
        for (int j = 0; j < NX; j++) g_wet[i][j] = g_wet[g_row0][j];

    int wet = 0;
    for (int i = g_row0; i < NZ; i++)
        for (int j = J0; j <= J1; j++) if (g_wet[i][j] >= 0.5f) wet++;
    return (float)wet / ((NZ - g_row0) * NX_INNER);
}

static unsigned tinted(unsigned rgb, int alpha) {
    if (alpha < 0) alpha = 0;
    else if (alpha > 255) alpha = 255;
    return rgb | (unsigned)alpha << 24;
}

/* The simulation and the swell, in one pass over the cells.

   The simulation is the plain damped wave equation -- velocity from the
   curvature, height from the velocity -- with the edges reflecting because
   their neighbour is themselves. Dry ground is a shore: what runs into it
   stops there. The curvature wants the heights the frame started with, so
   the cell behind is carried in a register and the row behind in a line of
   its own; the row ahead has not been written yet.

   The swell is two long waves crossing at an angle, each one a sine of the
   row and the column. A sine of a sum is two products of the ends, so the
   whole surface costs four sines a row and four a column rather than two a
   cell. It lands on the height the simulation just wrote, while both are
   still in registers: a second pass over five thousand cells is a second
   walk through eighty kilobytes, and the pass is the cost, not the sines. */
static void step_water(float t) {
    float sa1[NZ], ca1[NZ], sa2[NZ], ca2[NZ];
    float sb1[NX], cb1[NX], sb2[NX], cb2[NX];
    for (int i = 0; i < NZ; i++) {
        float a1 = g_row[i].a1 - t * 0.80f, a2 = g_row[i].a2 + t * 0.55f;
        sa1[i] = fsin(a1); ca1[i] = fcos(a1);
        sa2[i] = fsin(a2); ca2[i] = fcos(a2);
    }
    for (int j = 0; j < NX; j++) {
        float b1 = g_u[j] * 1.2f, b2 = g_u[j] * -2.4f;
        sb1[j] = fsin(b1); cb1[j] = fcos(b1);
        sb2[j] = fsin(b2); cb2[j] = fcos(b2);
    }

    static float behind[NX];            /* the row above, as it came in */
    for (int i = 0; i < NZ; i++) {
        float *h = g_h[i], *v = g_v[i], *hh = g_hh[i];
        const float *wet = g_wet[i];
        const float *down = g_h[i + 1 < NZ ? i + 1 : NZ - 1];
        float a1 = sa1[i], b1 = ca1[i], a2 = sa2[i], b2 = ca2[i];
        float left = h[0];
        for (int j = 0; j < NX; j++) {
            float old = h[j];
            float right = j + 1 < NX ? h[j + 1] : old;
            float up = i ? behind[j] : old;
            float lap = up + down[j] + left + right - 4.0f * old;
            float w = wet[j];
            float vv = (v[j] + lap * WAVE_C) * WAVE_DAMP;
            float nh = old + vv;
            if (w < 1.0f) { nh *= w; vv *= w; }
            if (nh > WAVE_MAX) nh = WAVE_MAX;
            else if (nh < -WAVE_MAX) nh = -WAVE_MAX;
            v[j] = vv;
            h[j] = nh;
            behind[j] = old;
            left = old;

            float s1 = a1 * cb1[j] + b1 * sb1[j];
            float s2 = a2 * cb2[j] + b2 * sb2[j];
            float s = 0.58f * s1 + 0.30f * s2 + nh;
            /* Water is not a sine: crests stand up and troughs lie flat. */
            hh[j] = (s + 0.20f * s * (s < 0 ? -s : s)) * w;
        }
    }
}

/* One over the square root, by the old trick: the exponent halved in the
   bits and one step of Newton over it. A part in six hundred out, which is
   nothing to a normal, for a tenth of what the divide and the root cost --
   and there are a few thousand of them a frame. */
static float rsqrt(float x) {
    union { float f; unsigned i; } u;
    u.f = x;
    u.i = 0x5F3759DFu - (u.i >> 1);
    float y = u.f;
    return y * (1.5f - 0.5f * x * y * y);
}

/* Place the whole surface in the world, and hand the GE what it needs to
   light it: for every crossing its world position, the way the surface faces
   there, the colour it holds, and the pixel it landed on -- so the flat
   things drawn over the water agree with the mesh.

   The light itself is the GE's. The normal comes off the height field's own
   slopes, flattened with distance: a row two pixels tall lit crossing by
   crossing reads as a dashed line along the horizon, and a surface that is
   flat there cannot dash. What the GE makes of it is a face turned toward
   the sun standing white and one turned away falling to the ambient, and a
   glint path down the middle for nothing -- the eye is where the eye is, so
   the facets that send the sun down the lens are the ones that lie between
   the viewer and the light. */
static void place_all(float swayx) {
    struct gfx_water_vertex *mesh = g_mesh;
    if (!mesh) return;
    /* A crossing's colour as a fraction of the one the palette was built
       from. Where the whole surface is the one colour the fraction is 1,
       every crossing on a row hands the GE the same white, and the row's own
       colour stands for all of them -- a thousandth over 255, so that a
       crossing holding exactly that colour comes out at the full level and
       not a rounding under it. */
    int plain = g_uniform;
    float sr = 255.001f / (g_ref.r > 1.0f ? g_ref.r : 1.0f);
    float sg = 255.001f / (g_ref.g > 1.0f ? g_ref.g : 1.0f);
    float sb = 255.001f / (g_ref.b > 1.0f ? g_ref.b : 1.0f);
    int dry = 0;

    /* From the horizon forward, because of the last thing each row does: no
       row may climb over the one behind it. Where a crest would, the strip
       between the two folds and lands on itself, and added on twice it is a
       bright line straight across the screen. So each row is held a pixel
       and a half below the row behind it -- a whole pixel put the two edges
       on the same pixel rows, and the rasteriser painted a dot from each --
       and the world position is taken back from the pixel, so the mesh and
       the flat things drawn over it still agree. The row behind is finished
       by the time the row in front asks where it is. */
    for (int i = NZ - 1; i >= 0; i--) {
        float xlim = XLIM * g_row[i].z, wz = -g_row[i].z;
        float y0 = g_row[i].y0, yh = g_row[i].yh, f = g_row[i].f, zf = g_row[i].zf;
        float kz = g_row[i].kz, n2 = g_row[i].near2, alpha = g_row[i].alpha;
        float dr = sr * g_row[i].dim, dg = sg * g_row[i].dim, db = sb * g_row[i].dim;
        float least = g_row[i].least, vrow = g_row[i].v;
        unsigned flat = g_row[i].color;
        const float *hh = g_hh[i];
        const float *up = g_hh[i ? i - 1 : 0], *down = g_hh[i + 1 < NZ ? i + 1 : NZ - 1];
        const float *wet = g_wet[i];
        const float *back = i + 1 < NZ ? g_y[i + 1] : 0;
        float *y = g_y[i], *x = g_x[i];
        int j0 = g_row[i].j0, j1 = g_row[i].j1;
        struct gfx_water_vertex *p = mesh + i * NX + j0;
        for (int j = j0; j <= j1; j++, p++) {
            float w = wet[j];
            float ax = (hh[j + 1 < NX ? j + 1 : NX - 1] - hh[j ? j - 1 : 0])
                     * g_kx[j] * n2;
            float az = (down[j] - up[j]) * kz;
            float k = rsqrt(ax * ax + az * az + 1.0f);

            float wx = g_u[j] + swayx;
            if (wx < -xlim) wx = -xlim;
            else if (wx > xlim) wx = xlim;
            /* Perspective would give the front row a crest half the screen
               tall; it is allowed this much and no more. Not cut off at it,
               though: a cut leaves every crest that reaches it the same
               height, and with the rows this close together a dozen of them
               come out as one flat terrace with an edge. Past half the cap a
               crest is eased into it instead, the two halves meeting with
               the same slope, so a tall crest is rounded rather than
               planed. */
            float dy = hh[j] * yh;
            float rise = dy < 0.0f ? -dy : dy;
            if (rise > DY_CAP * 0.5f) {
                float eased = DY_CAP - (0.25f * DY_CAP * DY_CAP) / rise;
                dy = dy < 0.0f ? -eased : eased;
            }
            float sy = y0 - dy;
            if (back) {
                float floor = back[j] + least;
                if (sy < floor) { sy = floor; dy = y0 - floor; }
            }
            x[j] = SCR_W / 2.0f + wx * f;
            y[j] = sy;

            unsigned color = flat;
            if (!plain) {
                struct rgb vt = g_vt[i][j];
                int lr = (int)(vt.r * dr), lg = (int)(vt.g * dg), lb = (int)(vt.b * db);
                color = RGBA(lr > 255 ? 255 : lr, lg > 255 ? 255 : lg,
                             lb > 255 ? 255 : lb, (int)alpha);
            }
            if (w < 1.0f) {
                dry = 1;
                color = (color & 0x00FFFFFFu) | (unsigned)(int)(alpha * w) << 24;
            }

            p->u = g_ut[j];
            p->v = vrow;
            p->color = color;
            p->nx = -ax * k;
            p->ny = k;
            p->nz = az * k;
            p->x = wx;
            p->y = dy * zf - EYE_Y;
            p->z = wz;
        }
    }
    g_dry = dry;
}

/* The surface itself: one strip of quads per pair of rows, in real space, so
   the GE lays the tile down in perspective and the front row comes out even
   instead of hatched. The same corners are drawn once per ripple step;
   writing them is the CPU's part and happens once. */
static void draw_surface(void) {
    if (!g_mesh || !g_index) return;
    int shine = -1;
    for (int i = 0; i < NZ - 1; i++) {
        /* The glint goes out with the square of the distance, as it always
           did: a dash is what a glint on a far row comes out as. Eight
           steps of it, so the light is set a handful of times and not once
           a strip. */
        int want = (int)(g_row[i].near2 * 7.99f);
        if (want != shine) {
            shine = want;
            gfx_water_shine(want * (1.0f / 7.0f));
        }
        gfx_water_strip(g_mesh, g_index + g_row[i].at,
                        (g_row[i].j1 - g_row[i].j0 + 1) * 2,
                        0.5f * (g_row[i].lod + g_row[i + 1].lod));
    }
}

/* How lit a crossing of the dry grid is: a crest stands in the light and a
   trough hides from it, a face leaning back toward the horizon catches more
   than a flat one, and ground keeps the little it is drawn with. The water's
   own light has not come through here since the GE took it over -- this is
   the grid's, and the grid is only ever on ground. The slope away is read
   over three rows' worth of the spacing the field had when the number below
   was chosen. */
static float lit_at(int i, int j) {
    int im = i ? i - 1 : 0, ip = i + 1 < NZ ? i + 1 : NZ - 1;
    float w = g_wet[i][j];
    float dhz = (g_hh[ip][j] - g_hh[im][j]) * 3.0f;
    float raw = 0.05f + w * (0.16f + 0.52f * g_hh[i][j] - 0.60f * dhz)
              + (1.0f - w) * 0.55f;
    if (raw < 0.0f) raw = 0.0f;
    return g_row[i].near2 * raw;
}

/* ------------------------------------------------------- what the water holds */

/* The picture on the card, lying in the water under it: the same mesh a
   second time, with the card's own texture on it instead of the ripple. A
   corner takes what the card holds at its own pixel, mirrored about the
   card's bottom edge, so the reflection stands on that edge and runs away
   from it -- and since the corner is where the surface put it, the picture
   is broken by every wave it crosses. The slope moves what a corner is
   holding on top of that, which is what turns a straight edge in the
   picture into a wavering one rather than a staircase.

   Added on at a quarter or so, and gone within a card and a bit: a
   reflection that reached the bottom of the screen would be a second
   picture, and this is meant to be light on water. */
#define MIRROR_ALPHA 0.28f
#define MIRROR_REACH 1.2f       /* card heights it carries down */
#define MIRROR_BREAK 0.05f      /* what a slope does to what a corner holds */
#define MIRROR_SIDE 0.10f       /* of the width, faded out at each side */
#define MIRROR_EDGE 16.0f       /* rows this far over the edge can still dip under it */

/* The first and last column of a row that the card stands over, one wide on
   each side so the strip covers the edge it is cut off at. Inside the
   columns the row was drawn over, since those are the ones it was placed
   for -- and the card is on the screen, so they are the ones it stands
   over. */
static void card_span(int i, float x0, float x1, int *lo, int *hi) {
    int j0 = g_row[i].j0, j1 = g_row[i].j1;
    while (j0 + 1 < j1 && g_x[i][j0 + 1] <= x0) j0++;
    while (j1 > j0 && g_x[i][j1 - 1] >= x1) j1--;
    *lo = j0;
    *hi = j1;
}

void lattice_mirror(const struct gfx_texture *t, int alpha,
                    float px, float bottom, float pw, float ph) {
    if (!t || !t->pixels || alpha <= 0 || !g_mesh) return;
    float u1 = (float)t->w / t->tw, v1 = (float)t->h / t->th;
    float head = MIRROR_ALPHA * alpha;          /* at the card's own edge */
    float reach = MIRROR_REACH * ph;
    float inv_w = 1.0f / pw, inv_h = 1.0f / ph, inv_reach = 1.0f / reach;
    float x1 = px + pw;
    int begun = 0;

    for (int i = 0; i < NZ - 1; i++) {
        if (g_row[i].y0 <= bottom - MIRROR_EDGE) break;
        int a0, a1, b0, b1;
        card_span(i, px, x1, &a0, &a1);
        card_span(i + 1, px, x1, &b0, &b1);
        int j0 = a0 < b0 ? a0 : b0, j1 = a1 > b1 ? a1 : b1;
        int n = (j1 - j0 + 1) * 2;
        if (n < 4) continue;
        if (!begun) { gfx_mirror_begin(t); begun = 1; }
        struct gfx_mirror_vertex *v = gfx_mirror_room(n);
        if (!v) break;
        struct gfx_mirror_vertex *p = v;
        for (int j = j0; j <= j1; j++) {
            int jm = j ? j - 1 : 0, jp = j + 1 < NX ? j + 1 : NX - 1;
            for (int k = 0; k < 2; k++, p++) {
                int r = i + k;
                int rm = r ? r - 1 : 0, rp = r + 1 < NZ ? r + 1 : NZ - 1;
                const struct gfx_water_vertex *m = &g_mesh[r * NX + j];
                float below = g_y[r][j] - bottom;
                float u = (g_x[r][j] - px) * inv_w
                        + (g_hh[r][jp] - g_hh[r][jm]) * MIRROR_BREAK;
                float v_ = 1.0f - below * inv_h
                         + (g_hh[rp][j] - g_hh[rm][j]) * MIRROR_BREAK;
                int fade = 0;
                if (below > 0.0f && below < reach && u > 0.0f && u < 1.0f) {
                    /* Falling as the square rather than straight: under the
                       card, where a reflection is read as one, it keeps its
                       full weight, and it is gone by the time the lines
                       under the card are written -- which are meant to be
                       read, not to sit in a picture. */
                    float left = 1.0f - below * inv_reach;
                    float lit = head * left * left;
                    /* And out at the sides. The picture is clamped there, so
                       a corner past its edge holds the edge texel, and the
                       span between the last corner inside and the first
                       outside would draw that one column of it stretched
                       across. Faded out before it can. */
                    float edge = u < 0.5f ? u : 1.0f - u;
                    if (edge < MIRROR_SIDE) lit *= edge * (1.0f / MIRROR_SIDE);
                    fade = (int)lit;
                }
                p->u = u * u1;
                p->v = v_ * v1;
                p->color = RGBA(255, 255, 255, fade);
                p->x = m->x;
                p->y = m->y;
                p->z = m->z;
            }
        }
        gfx_mirror_strip(v, n);
    }
    if (begun) gfx_mirror_end();
}

/* The source hangs over the cell it is filling, so it has to be placed
   between four crossings that have already been projected. */
static void source_at(float *sx, float *sy) {
    float rf = g_row0 + g_source.fz * (NZ - 1 - g_row0);
    float jf = J0 + g_source.fx * (NX_INNER - 1);
    int i = (int)rf, j = (int)jf;
    if (i > NZ - 2) i = NZ - 2;
    /* Only over columns the row was placed over: off the side of the screen
       the source is out of sight anyway, and the edge of what was placed is
       out of sight with it. */
    if (j < g_row[i].j0) j = g_row[i].j0;
    if (j > g_row[i].j1 - 1) j = g_row[i].j1 - 1;
    if (j > NX - 2) j = NX - 2;
    float fi = rf - i, fj = jf - j;
    float x0 = g_x[i][j] + (g_x[i][j + 1] - g_x[i][j]) * fj;
    float x1 = g_x[i + 1][j] + (g_x[i + 1][j + 1] - g_x[i + 1][j]) * fj;
    float y0 = g_y[i][j] + (g_y[i][j + 1] - g_y[i][j]) * fj;
    float y1 = g_y[i + 1][j] + (g_y[i + 1][j + 1] - g_y[i + 1][j]) * fj;
    *sx = x0 + (x1 - x0) * fi;
    *sy = y0 + (y1 - y0) * fi;
}

void lattice_draw(float t, struct rgb tint) {
    float sway = fsin(t * 0.23f) * 0.06f;
    step_water(t);
    /* The colour before the light: the palette below and every crossing's
       own share of it are both built out of what the front has done. */
    if (!g_told) watch(tint);
    spread(tint);

    gfx_batch_begin();

    /* Sky: a few points of light, and the horizon burning under them. */
    unsigned white = rgb_pack(RGB_WHITE, 0);
    for (int i = 0; i < STARS; i++) {
        g_stars[i].x += g_stars[i].vx + fsin(t * 0.3f + g_stars[i].phase) * 0.03f;
        g_stars[i].y += g_stars[i].vy;
        if (g_stars[i].x < -8) g_stars[i].x += SCR_W + 16;
        else if (g_stars[i].x > SCR_W + 8) g_stars[i].x -= SCR_W + 16;
        if (g_stars[i].y < 4) { g_stars[i].y = 4; g_stars[i].vy = -g_stars[i].vy; }
        else if (g_stars[i].y > GFX_HORIZON - 24) { g_stars[i].y = GFX_HORIZON - 24; g_stars[i].vy = -g_stars[i].vy; }
        float tw = 0.5f + 0.5f * fsin(t * 1.3f + g_stars[i].phase);
        gfx_glow(g_stars[i].x, g_stars[i].y, g_stars[i].size, g_stars[i].size,
                 tinted(white, (int)(30 + 70 * tw)));
    }
    /* The light is out past the far row, so the room sliding under it barely
       moves it. */
    float lightx = SCR_W / 2 + sway * (GFX_FOCAL / Z_FAR);
    gfx_glow(lightx, GFX_HORIZON + 6, 760, 110, rgb_pack(tint, 110));
    gfx_glow(lightx, GFX_HORIZON + 2, 420, 30,
             rgb_pack(rgb_mix(tint, RGB_WHITE, 0.6f), 120));
    /* The light's path on the water: the sun is a point on the horizon and
       the water is rough, so what comes back down the lens is a path that
       runs from under the light to the viewer, narrow at the far end and
       broad at the near one. The GE's own glint cannot draw it -- a light
       that far off makes the same angle with every facet at a given depth,
       so it lights a band across rather than a path down -- and the path is
       what a sea looks like. Three lights under the water, widening as they
       come forward; the surface is drawn over them and adds its own. */
    struct rgb pathlit = rgb_mix(tint, RGB_WHITE, 0.45f);
    gfx_glow(lightx, GFX_HORIZON + 34, 150, 90, rgb_pack(pathlit, 40));
    gfx_glow(lightx, GFX_HORIZON + 86, 330, 150, rgb_pack(pathlit, 38));
    gfx_glow(lightx, GFX_HORIZON + 160, 620, 190, rgb_pack(pathlit, 34));

    /* What the water is made of, for the palette: its own dark, the sky it
       mirrors, and what a facet turned square into the light sends back.
       Built from the brightest colour on the surface rather than from the
       room's: a crossing holding a dimmer one gets there by handing the GE
       its own share of it, and while there is only one colour on the water
       the two are the same colour and this is what it always was. */
    gfx_water_light(0.42f * fsin(t * 0.13f), 0.86f, 0.30f,
                    rgb_pack(rgb_mix(g_ref, DEEP, 0.86f), 0),
                    rgb_pack(rgb_mix(g_ref, DEEP, 0.38f), 0),
                    rgb_pack(rgb_mix(rgb_mix(g_ref, RGB_WHITE, 0.9f), DEEP, 0.62f), 0));
    place_all(sway);
    gfx_water_ready();
    /* Seven steps a second through the ripple's baked frames, each one
       crossfaded into the next so nothing jumps. */
    float phase = t * 7.0f;
    int step = (int)phase;
    float f = phase - step;
    /* The tile is anchored to the world and creeps toward the viewer, which
       is the movement between the crossings that the swell is too coarse to
       carry; the sway of the room goes with it, since the surface is drawn
       where the sway put it. */
    gfx_water_begin(sway * TILES + t * 0.05f, -t * 0.33f);
    gfx_water_step(0, step, 1.0f - f);
    draw_surface();
    gfx_water_step(1, step + 1, f);
    draw_surface();
    gfx_water_end();

    /* The ground under the water, which is only ever drawn while there is
       ground: the sweep starts dry and ends wet, and the browser stands on a
       field that has been water since before it came up. Nothing below here
       has anything to say about open sea. It is walked two crossings at a
       time, so the grid keeps the spacing it had before the field was
       doubled under it. */
    if (g_dry) {
        float x[NX > NZ ? NX : NZ], y[NX > NZ ? NX : NZ];
        unsigned c[NX > NZ ? NX : NZ];
        /* The lines and the crossings each keep one colour all frame and
           vary only in alpha, so the channels are packed once and the alpha
           byte is laid in over them. */
        unsigned line = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.15f), 0);
        int i0 = g_row0 - 1;

        /* The grid belongs to the ground, not to the water: it goes out
           under a cell as the cell fills, and what is left where the field
           is full is the surface and nothing else. */
        for (int i = i0; i < NZ; i += 2) {
            int n = 0;
            for (int j = g_row[i].j0; j <= g_row[i].j1; j += 2, n++) {
                x[n] = g_x[i][j];
                y[n] = g_y[i][j];
                c[n] = tinted(line, (int)(190 * lit_at(i, j) * (1.0f - g_wet[i][j])));
            }
            gfx_ribbon(x, y, c, n, 0.9f);
        }
        /* A line away runs only as far forward as the rows it crosses were
           drawn out to: nearer than that its column is off the side of the
           screen, and the crossing it would join was never placed. */
        for (int j = 0; j < NX; j += 2) {
            int n = 0;
            for (int i = i0; i < NZ; i += 2) {
                if (j < g_row[i].j0 || j > g_row[i].j1) {
                    if (n >= 2) gfx_ribbon(x, y, c, n, 0.9f);
                    n = 0;
                    continue;
                }
                x[n] = g_x[i][j];
                y[n] = g_y[i][j];
                c[n] = tinted(line, (int)(140 * lit_at(i, j) * (1.0f - g_wet[i][j])));
                n++;
            }
            if (n >= 2) gfx_ribbon(x, y, c, n, 0.9f);
        }
        /* Dry ground keeps a light at every crossing. Water keeps only the
           foam along the shore where a cell is filling but not yet full; its
           glints are in the palette, texel by texel, and a glow laid on a
           crossing over a row a few pixels tall came out as a dash. */
        unsigned cell = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.45f), 0);
        unsigned foam = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.85f), 0);
        for (int i = i0; i < NZ; i += 2) {
            for (int j = g_row[i].j0; j <= g_row[i].j1; j += 2) {
                float sx = g_x[i][j];
                if (sx < -10 || sx > SCR_W + 10) continue;
                float w = g_wet[i][j];
                if (w >= 0.98f) continue;
                float sy = g_y[i][j], lit = lit_at(i, j);
                float size = 3.0f + 16.0f * lit;
                gfx_glow(sx, sy, size, size,
                         tinted(cell, (int)((50 + 200 * lit) * (1.0f - w))));
                if (w > 0.05f)
                    gfx_glow(sx, sy, 14, 7,
                             tinted(foam, (int)(200 * g_row[i].near2)));
            }
        }
    }

    /* The source: a light standing over the water it is making, the column
       under it, and what it throws up where the two meet. */
    if (g_source.on) {
        float sx, sy;
        source_at(&sx, &sy);
        unsigned core = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.8f), 0);
        gfx_glow(sx, sy - 15, 7, 34, tinted(core, 150));
        gfx_glow(sx, sy, 54, 22, tinted(core, 120));
        gfx_glow(sx, sy, 26, 12, tinted(white, 200));
        float bob = 2.0f * fsin(t * 5.0f);
        gfx_glow(sx, sy - 30 + bob, 26, 26, tinted(core, 210));
        gfx_glow(sx, sy - 30 + bob, 11, 11, tinted(white, 255));
    }

    /* Spray lifting off the water. */
    unsigned spark = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.7f), 0);
    for (int i = 0; i < SPECKS; i++) {
        g_specks[i].y -= g_specks[i].vy;
        g_specks[i].x += fsin(t * 1.7f + g_specks[i].phase) * 0.2f;
        if (g_specks[i].y < 6) speck_reset(i, 0);
        /* Brightest just over the horizon, gone by the top. */
        float life = (g_specks[i].y - 6) / (GFX_HORIZON - 10);
        gfx_glow(g_specks[i].x, g_specks[i].y, g_specks[i].size, g_specks[i].size,
                 tinted(spark, (int)(140 * life)));
    }

    gfx_batch_end();
}
