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
