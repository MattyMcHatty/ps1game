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
void flip_buffers(RenderContext *ctx);
void draw_sky_gradient(RenderContext *ctx);
void draw_faces(RenderContext *ctx, SVECTOR *verts, int faces[][4],
                uint8_t colors[][3], int face_count, int depth_bias);

#endif
