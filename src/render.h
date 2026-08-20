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
