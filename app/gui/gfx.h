#ifndef PSPDX_GFX_H
#define PSPDX_GFX_H

#include <stddef.h>

/* The hardware surface. The draw buffer is 512 wide because the GE wants a
   power of two; only the left 480 are shown. */
#define SCR_W 480
#define SCR_H 272

/* PSP colours are ABGR little-endian, which is why every literal in this
   codebase looks byte-swapped. */
#define RGBA(r, g, b, a) \
    ((unsigned)(((a) << 24) | ((b) << 16) | ((g) << 8) | (r)))
#define RGB(r, g, b) RGBA(r, g, b, 0xFF)

void gfx_init(void);
void gfx_shutdown(void);

/* One frame: begin, draw, end. end() syncs, waits for vblank and swaps, so it
   paces the caller at 60 Hz. */
void gfx_frame_begin(unsigned clear);
void gfx_frame_end(void);
unsigned gfx_frames(void);

/* Something the firmware draws over every frame -- its on-screen keyboard
   -- called from end() once the list is finished and before the swap, which
   is where the utility dialogs want to be. NULL takes it off again. */
void gfx_frame_overlay(void (*overlay)(void));

/* The worst wait for the GE to finish and the worst wait for vblank since
   last asked, in microseconds: how much of a frame the drawing itself takes
   and how much is slack. */
void gfx_frame_worst(unsigned *ge_us, unsigned *vblank_us, unsigned *list_bytes);

/* Makes room for bytes more in the display list before something that
   takes them without asking, intraFont above all. Inside a frame that has
   no room left, what is drawn so far goes to the GE and the frame goes on
   in an empty list. 0 when no list holds that many bytes, or when there
   is no room outside a frame: the caller then draws nothing. */
int gfx_list_room(unsigned bytes);

void gfx_rect(int x, int y, int w, int h, unsigned color);
void gfx_vgrad(int x, int y, int w, int h, unsigned top, unsigned bottom);
void gfx_hgrad(int x, int y, int w, int h, unsigned left, unsigned right);

/* A band through n points, half pixels above and below each, one colour per
   point so a line can fade with depth along its own length. Added onto what
   is behind it. */
void gfx_ribbon(const float *x, const float *y, const unsigned *color, int n,
                float half);

/* ------------------------------------------------------------------ water */

struct gfx_texture;

/* The water is the one surface drawn in real space rather than projected by
   hand: a plane in perspective texture-maps evenly, and a plane faked in 2D
   does not -- the GE interpolates a flat quad's texture affinely, and on the
   big foreshortened quads at the front that shows as a diagonal hatch.

   Its camera is a level pinhole: GFX_FOCAL pixels across per unit of width at
   unit depth, with the eye's own height above the surface carried in the
   vertices, and the vanishing point at GFX_HORIZON rather than the middle of
   the screen. Anything drawn flat over the water -- the dry grid, the lights
   at its crossings -- projects itself with gfx_water_project and lands on the
   pixel the GE puts the mesh on. */
#define GFX_FOCAL 720.0f
#define GFX_HORIZON 116.0f

static inline void gfx_water_project(float x, float y, float z,
                                     float *sx, float *sy) {
    float f = GFX_FOCAL / z;
    *sx = SCR_W / 2.0f + x * f;
    *sy = GFX_HORIZON - y * f;
}

/* One corner: where it is in the world (x across, y up, z away from the
   viewer), where it sits in the ripple tile in tiles, the way the surface
   faces there, and the colour it carries -- the room's, as a fraction of the
   one the palette was built on, with the wet fade and the distance in its
   alpha. The light is the GE's: the normal and this colour go in as the
   material, and what comes out multiplies the texture. The GE reads the
   components in a fixed order -- texture, colour, normal, position -- so the
   members are declared in it. */
struct gfx_water_vertex {
    float u, v;
    unsigned color;
    float nx, ny, nz;
    float x, y, z;
};

/* The palette is the lighting. gfx's ripple tiles hold a quantised normal per
   texel rather than a colour, and this turns all 256 of them into colours for
   one light: deep where the water faces the eye, sky where it turns away, and
   glint where it faces the light square on. A kilobyte a frame buys per-texel
   reflection that no amount of geometry would. Directions are in the tile's
   own space: x across, y away, z up. */
void gfx_water_light(float lx, float ly, float lz,
                     unsigned deep, unsigned sky, unsigned glint);

/* Room for the whole surface and for the indices that cut it into strips,
   both kept from frame to frame: a crossing belongs to the strip above it
   and the one below, and written once and pointed at twice it is written
   half as often. ready() puts what the caller wrote where the GE can read
   it. Between begin and end nothing else may draw. */
struct gfx_water_vertex *gfx_water_mesh(int verts);
unsigned short *gfx_water_index(int count);
void gfx_water_ready(void);
/* du and dv are where the tile sits this frame: the whole surface creeps
   together, so it creeps once here rather than in every corner. */
void gfx_water_begin(float du, float dv);

/* The ripple has a handful of baked steps, and cut from one to the next
   it stutters against the swell, which moves every frame. So the surface
   is drawn twice, the step going out at (1 - f) of the light and the one
   coming in at f: call this with which = 0 and 1 before each pass. */
void gfx_water_step(int which, int frame, float weight);
/* level is the mip level the strip is sampled at, 0 = the full tile: the
   GE would pick one per triangle from the triangle's shape, and a strip a
   pixel tall and the screen wide is the wrong shape to ask. */
void gfx_water_strip(const struct gfx_water_vertex *v, const unsigned short *idx,
                     int n, float level);
/* How much of the glint the strips drawn after this one may keep, 0 to 1.
   A crossing far enough away stands over a cell tens of pixels wide and two
   tall, and a glint found at one corner of it is drawn as a dash across it.
   The surface itself is flattened with distance for the same reason; this
   is the same retreat for the light that flattening cannot reach, since a
   glint is between the eye and the light and not in the surface alone. */
void gfx_water_shine(float keep);
void gfx_water_end(void);

/* A picture laid on the water: the same mesh a second time, textured with
   whatever is on the card and added on, so the surface breaks it. The
   caller places the corners itself -- it has the mesh -- and only says
   where they are and what of the picture is there. Clamped, so a corner
   past the picture's edge holds still rather than repeating it. */
struct gfx_mirror_vertex { float u, v; unsigned color; float x, y, z; };

void gfx_mirror_begin(const struct gfx_texture *t);
struct gfx_mirror_vertex *gfx_mirror_room(int verts);
void gfx_mirror_strip(const struct gfx_mirror_vertex *v, int n);
void gfx_mirror_end(void);

/* A soft radial light, added onto what is behind it. The alpha in color is
   how strong; the rgb is what it tints toward. Cheap enough to draw dozens
   of per frame. */
void gfx_glow(float cx, float cy, float w, float h, unsigned color);

/* Between these two, glows and ribbons pile up and go out as a handful of
   draws instead of one each -- one texture bind for every light on screen
   rather than hundreds. Every other gfx_ primitive empties what is pending
   first, so the order things were asked for is the order they land in.
   Nothing that draws behind gfx_'s back -- intraFont above all -- may run
   inside a batch, or it would end up under what was asked for before it. */
void gfx_batch_begin(void);
void gfx_batch_end(void);

/* Pixels live in system RAM and are read by the GE directly, so they must be
   16-byte aligned and written back out of the cache before use. w/h are the
   used area inside the power-of-two tw/th. */
struct gfx_texture {
    int w, h, tw, th;
    void *pixels;
    /* The alpha channel means nothing: take colour only. sceMpeg writes
       every pixel with alpha zero, as the PSP does. */
    int opaque;
};

void gfx_texture_draw(const struct gfx_texture *t, int x, int y, int w, int h,
                     unsigned tint);

/* One cell out of a sheet, at its own size, on whole pixels: sx, sy, w, h
   are texels and x, y are where the top left of them lands. tint modulates
   colour and alpha together, so a white cell comes out in the caller's
   colour and a black one is a shadow the caller can fade. This is how the
   marks are drawn -- two sprites out of one bound texture. */
void gfx_texture_draw_part(const struct gfx_texture *t, int sx, int sy,
                           int w, int h, float x, float y, unsigned tint);

void gfx_texture_free(struct gfx_texture *t);

/* A card in space: a picture on a plane that can turn a little toward or
   away from the viewer, drawn in real perspective. Everything else on screen
   is flat; this is the one thing that is not, on purpose. */
struct gfx_card {
    float cx, cy;           /* screen centre when facing the viewer */
    float w, h;             /* screen size when facing the viewer */
    float yaw, pitch;       /* radians; small */
    int alpha;              /* 0..255 for the picture */
    float gloss;            /* 0..1 where the light sweep is, outside = none */
    int reflect_h;          /* pixels of reflection below, 0 = none */
    /* Lettering rather than a picture: no frame, no shadow, and the sweep
       lights the texture's own shape instead of a band across the card. */
    int bare;
};

/* t may be NULL: then only the frame, its shadow and its gloss are drawn,
   which is what the card looks like while its picture is on its way. */
void gfx_card_draw(const struct gfx_texture *t, const struct gfx_card *c);

/* Baking a texture: between frames, a corner of the draw buffer is cleared
   to transparent and handed to the caller to draw into with the ordinary 2D
   calls; end copies that corner through the GE into `into`, a texture in
   system RAM the caller allocated (16-byte aligned, tw >= w, th >= h), and
   the frame that follows paints over the corner. The draw buffer is the one
   surface the emulator keeps honest for a copy -- an off-screen target
   comes back empty. Call outside a frame only. */
/* Everything drawn between the two stays inside the rectangle: what a
   scrolling line is cut off at. Nests once. */
void gfx_clip(int x, int y, int w, int h);
void gfx_unclip(void);

int gfx_bake_begin(int w, int h);
void gfx_bake_end(struct gfx_texture *into);

/* A soft dark spot, composited rather than added: a shadow. */
void gfx_shade(float cx, float cy, float w, float h, int alpha);

/* A veil over everything drawn after it: every alpha is scaled by keep/256,
   so a whole layer can be faded by the one call rather than by touching each
   colour. 256 lifts it. */
void gfx_veil(int keep);
unsigned gfx_veiled(unsigned color);

/* Reads back whatever is on screen right now, GU or debug screen: it asks the
   display which buffer is front rather than assuming the start of VRAM. */
void gfx_screenshot(const char *path);

#endif
