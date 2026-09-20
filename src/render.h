#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>

#define OT_LENGTH     2048
#define BUFFER_LENGTH 65536
/* OT indices 0-15 are reserved for menu UI — always rendered on top.
   All scene/entity geometry must use indices >= SCENE_OT_MIN. */
#define SCENE_OT_MIN  16
#define SCREEN_XRES   320
#define SCREEN_YRES   240

#define SKY_FOG_R 25
#define SKY_FOG_G  0
#define SKY_FOG_B 29

typedef struct {
    DISPENV  disp_env;
    DRAWENV  draw_env;
    uint32_t ot[OT_LENGTH];
    uint8_t  buffer[BUFFER_LENGTH];
} RenderBuffer;

typedef struct {
    RenderBuffer buffers[2];
    uint8_t     *next_packet;
    int          active_buffer;
} RenderContext;

/* Per-frame performance counters (refreshed by flip_buffers, shown in debug). */
/* ---- Section timing, in HBLANKS -------------------------------------------
   >>> VB ALONE CANNOT SIZE A PROBLEM, ONLY DETECT ONE. <<< perf_frame_vblanks
   is a whole-frame quantum: VB2 covers everything from 16.7ms to 33.3ms, so a
   change that took a frame from 30ms to 18ms reads exactly the same as one that
   did nothing. Maze One was optimised for several rounds against that number and
   it could not show whether any of it was working — the primitive count fell
   from 269 to 93 while VB never moved.

   Root counter 1 counts hblanks: 15.734 kHz, so 262 ticks to a 60Hz frame and
   one tick is 0.4% of it. It wraps every 4.2 seconds, which per-frame deltas in
   16-bit arithmetic do not care about. Nothing else in the SDK uses it.

   The three sections answer the question the "GPU-bound" flag never could:
     perf_ticks_update  the game's own logic
     perf_ticks_draw    walking the scene and QUEUEING primitives — CPU, and
                        this is where culling work lands, including the cost of
                        the culls themselves
     perf_ticks_gpu     time spent WAITING at DrawSync for the GPU to finish the
                        previous frame. The GPU draws in parallel with the CPU,
                        so this is the overhang: near zero means the CPU is the
                        limit however busy the GPU looked, and large means fill
                        really is the wall.
   ------------------------------------------------------------------------- */
extern int perf_ticks_update;
extern int perf_ticks_draw;
extern int perf_ticks_gpu;

/* Start root counter 1. Call once at startup, before the render loop. */
void perf_timer_init(void);
/* Current hblank count, 16-bit. Differences are (a - b) & 0xFFFF. */
int  perf_ticks_now(void);

extern int perf_frame_vblanks;  /* vblanks per frame: 1 = full rate, 2 = half, ... */
extern int perf_packet_bytes;   /* packet-buffer bytes used (scene load proxy)      */
extern int perf_gpu_busy;       /* 1 = GPU still drawing at flip (fill/GPU-bound)   */

/* ---- Contact-seam depth sorting -------------------------------------------
   The PS1 sorts per-poly by AVERAGE depth (gte_avsz3/4), so where a vertical
   surface stands on a floor/ceiling the two averages nearly tie along the
   contact line and the wrong poly can win the sort — each texture bleeds
   across the 90-degree seam in a "cross", worst at oblique view angles.

   Fix: sort HORIZONTAL (flat-in-Y) polys by their FARTHEST corner instead of
   their average, so a floor that continues under a prop/wall always sorts
   behind whatever stands on it. InitGeom's ZSF3=341/ZSF4=256 make the AVSZ
   OTZ equal avg(SZ)/4, so max(SZ)>>2 is the same unit. Model-space vy is flat
   iff world vy is flat (props only rotate about Y), and flat surfaces survive
   the int16 export exactly, so plain equality detects them. */
static inline int poly_is_flat_y(const SVECTOR *v0, const SVECTOR *v1,
                                 const SVECTOR *v2, const SVECTOR *v3) {
    if (v1->vy != v0->vy || v2->vy != v0->vy) return 0;
    return v3 == 0 || v3->vy == v0->vy;
}
static inline int32_t otz_far3(int32_t a, int32_t b, int32_t c) {
    int32_t m = a > b ? a : b;
    if (c > m) m = c;
    return m >> 2;
}
static inline int32_t otz_far4(int32_t a, int32_t b, int32_t c, int32_t d) {
    int32_t m = a > b ? a : b;
    if (c > m) m = c;
    if (d > m) m = d;
    return m >> 2;
}

/* Distance fog shared by room geometry and the entities drawn in it. Each area
   draw sets g_fog_near/g_fog_far to its own room fog; sprites cull at/after
   g_fog_far and modulate their colour by render_fog_scale (256 = near/full
   colour, 0 = far/fully fogged). */
extern int32_t g_fog_near, g_fog_far;
int render_fog_scale(int32_t dist);

/* ---- Point lights, as a discount on the fog ---------------------------------
   A LIGHT DOES NOT ADD BRIGHTNESS HERE. It subtracts DISTANCE. Everything in
   this game that fades - the room mesh, the props, the sprites - fades on one
   number: how far the surface is from the camera. A light is registered at a
   world XZ, and any surface inside its radius is fogged and culled as if it
   were NEARER THE CAMERA than it really is. That is why it needs no second
   colour ramp, no per-vertex normals and no extra pass: it reuses the fog the
   player already carries around with them, and a wall a brazier has "lit" is
   lit in exactly the shade that wall has when the player stands next to it.

   The mapping is linear in the light's own Manhattan radius: a surface ON the
   light reads as g_fog_near (full colour), one at `radius` reads as g_fog_far
   (fully fogged, i.e. no contribution at all), and the two ends meet the
   camera's own fade smoothly because both are points on the SAME ramp.

   `strength` is a 0..256 fade for the light as a whole, and it exists because
   a light switching on at full reach would pop a disc of geometry into view in
   a single frame - the same reason src/catacombs_entry.c eases its view
   distance rather than stepping it. It lerps the discounted distance back
   toward the true one, so 0 is indistinguishable from no light at all.

   ORDER, and it is the whole of the contract. draw_current_area() in
   src/main.c calls render_lights_clear() once before it dispatches to ANY
   room, so the list starts every frame empty and no room can inherit another
   room's lights. A room that has lights then calls render_light_add() from
   inside its own draw, AFTER it has set g_fog_near/g_fog_far for the frame
   (render_light_add resolves its ramp against them) and BEFORE it queues the
   geometry those lights are supposed to reach. A room that has none does
   nothing at all, and pays one rejected box test per call site. */
#define RENDER_MAX_LIGHTS 4

typedef struct {
    int32_t x, z;       /* world XZ                                          */
    int32_t radius;     /* Manhattan reach; past it the light contributes 0  */
    int32_t k;          /* 12.12 apparent-distance units per world unit      */
    int32_t strength;   /* 0..256 fade-in of the whole light                 */
} RenderLight;

extern RenderLight g_lights[RENDER_MAX_LIGHTS];
extern int32_t g_light_count;
/* Union of every registered light's reach, so the common case - a surface no
   light touches - costs four compares and no loop. Inverted while the list is
   empty, which makes the test below reject unconditionally. */
extern int32_t g_light_min_x, g_light_max_x, g_light_min_z, g_light_max_z;

void render_lights_clear(void);
void render_light_add(int32_t x, int32_t z, int32_t radius, int32_t strength);

/* The apparent distance of a surface at world (x,z) whose true distance from
   the camera is cam_dist: cam_dist itself, or less where a light reaches it.
   Feed it to the CULL TEST and to the FOG MATHS BOTH - passing it to only one
   of them is how you get geometry that is lit but still culled, or drawn but
   still black. Inline, and box-rejecting first, because the room mesh calls it
   once per primitive. */
static inline int32_t render_light_dist(int32_t x, int32_t z, int32_t cam_dist) {
    if (x < g_light_min_x || x > g_light_max_x ||
        z < g_light_min_z || z > g_light_max_z) return cam_dist;
    int32_t best = cam_dist;
    int i;
    for (i = 0; i < g_light_count; i++) {
        const RenderLight *L = &g_lights[i];
        int32_t dx = x - L->x, dz = z - L->z;
        int32_t d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (d >= L->radius) continue;
        d = g_fog_near + ((d * L->k) >> 12);
        if (d >= best) continue;
        /* Fade the DISCOUNT in, never the geometry: at strength 0 this leaves
           best exactly as it was, so a light coming up disturbs nothing. */
        best -= ((best - d) * L->strength) >> 8;
    }
    return best;
}

void setup_context(RenderContext *ctx, int w, int h, int r, int g, int b);

/* Repaint the HARDWARE background clear in this colour, for both buffers.

   >>> USE THIS INSTEAD OF DRAWING A FULL-SCREEN BACKGROUND TILE. <<< The draw
   environments are set up with isbg=1 (setup_context), so DrawOTagEnv already
   fills the whole 320x240 framebuffer before a single primitive is drawn. A room
   that then queues its own full-screen TILE to get a different colour is paying
   for the SECOND full-screen fill of the same frame — about 77k pixels, call it
   a millisecond of a 16.6ms budget, every frame, for a result the hardware clear
   would have given free.

   That millisecond is not academic on a fill-bound room: Maze One's entry view
   measured VB2 (30fps) with the debug overlay up and VB1 (60fps) without it, so
   the frame there sits inside a millisecond of the vblank boundary and this is
   the cheapest millisecond available.

   Both buffers are set because the room alternates between them; the two RGB
   writes are free, so calling this every frame from a room's draw is fine and
   needs no room-entry hook. Rooms that still paint their own TILE are unharmed
   either way — their tile simply covers whatever the clear left. */
void render_set_clear_colour(RenderContext *ctx, int r, int g, int b);
void flip_buffers(RenderContext *ctx);
void draw_sky_gradient(RenderContext *ctx);
void draw_faces(RenderContext *ctx, SVECTOR *verts, int faces[][4],
                uint8_t colors[][3], int face_count, int depth_bias);

#endif
