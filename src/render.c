#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"

void setup_context(RenderContext *ctx, int w, int h, int r, int g, int b) {
    SetDefDrawEnv(&ctx->buffers[0].draw_env, 0, 0, w, h);
    SetDefDispEnv(&ctx->buffers[0].disp_env, 0, 0, w, h);
    SetDefDrawEnv(&ctx->buffers[1].draw_env, 0, h, w, h);
    SetDefDispEnv(&ctx->buffers[1].disp_env, 0, h, w, h);

    setRGB0(&ctx->buffers[0].draw_env, r, g, b);
    setRGB0(&ctx->buffers[1].draw_env, r, g, b);
    ctx->buffers[0].draw_env.isbg = 1;
    ctx->buffers[1].draw_env.isbg = 1;

    ctx->active_buffer = 0;
    ctx->next_packet   = ctx->buffers[0].buffer;
    ClearOTagR(ctx->buffers[0].ot, OT_LENGTH);
    ClearOTagR(ctx->buffers[1].ot, OT_LENGTH);

    SetDispMask(1);
}

/* Perf counters, refreshed every flip and read by the debug overlay:
   perf_frame_vblanks — frame period in vblanks (1 = full 60/50fps, 2 = half, ...)
   perf_packet_bytes  — bytes of the packet buffer used this frame (scene load proxy)
   perf_gpu_busy      — 1 if the GPU was still drawing the previous frame at flip
                        time, i.e. the frame is GPU/fill-bound rather than CPU-bound. */
int perf_frame_vblanks = 1;
int perf_packet_bytes  = 0;
int perf_gpu_busy      = 0;

/* Distance fog shared by the room mesh and the entities/sprites drawn in it, so
   enemies and pickups fade (and vanish) together with the walls around them
   instead of standing out un-fogged behind faded geometry. Each area's draw sets
   these to match its own room fog. render_fog_scale returns 256 at/inside
   fog_near (full colour) down to 0 at/beyond fog_far (fully fogged). */
int32_t g_fog_near = 350;
int32_t g_fog_far  = 1500;

int render_fog_scale(int32_t dist) {
    if (dist <= g_fog_near) return 256;
    if (dist >= g_fog_far)  return 0;
    return ((g_fog_far - dist) << 8) / (g_fog_far - g_fog_near);
}

void flip_buffers(RenderContext *ctx) {
    static uint32_t prev_vb = 0;

    RenderBuffer *draw_buffer = &ctx->buffers[ctx->active_buffer];

    /* Sample BEFORE the sync: how much geometry we queued, and whether the GPU
       is still chewing the previous frame (GPU-bound) or already idle (CPU-bound). */
    perf_packet_bytes = (int)(ctx->next_packet - draw_buffer->buffer);
    perf_gpu_busy     = IsIdleGPU(0) ? 0 : 1;

    DrawSync(0);
    VSync(0);

    /* Free-running vblank counter (VSync(-1)); diff = vblanks this frame took. */
    uint32_t now = (uint32_t)VSync(-1);
    perf_frame_vblanks = (int)(now - prev_vb);
    if (perf_frame_vblanks < 1) perf_frame_vblanks = 1;
    prev_vb = now;

    RenderBuffer *disp_buffer = &ctx->buffers[ctx->active_buffer ^ 1];

    PutDispEnv(&disp_buffer->disp_env);
    DrawOTagEnv(&draw_buffer->ot[OT_LENGTH - 1], &draw_buffer->draw_env);

    ctx->active_buffer ^= 1;
    ctx->next_packet    = disp_buffer->buffer;
    ClearOTagR(disp_buffer->ot, OT_LENGTH);
}

void draw_sky_gradient(RenderContext *ctx) {
    POLY_G3 *t0 = (POLY_G3 *)ctx->next_packet;
    setPolyG3(t0);
    setRGB0(t0, 40,  0, 46); t0->x0 =   0; t0->y0 =   0;
    setRGB1(t0, 40,  0, 46); t0->x1 = 320; t0->y1 =   0;
    setRGB2(t0, 25,  0, 29); t0->x2 =   0; t0->y2 = 240;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], t0);
    ctx->next_packet += sizeof(POLY_G3);

    POLY_G3 *t1 = (POLY_G3 *)ctx->next_packet;
    setPolyG3(t1);
    setRGB0(t1, 40,  0, 46); t1->x0 = 320; t1->y0 =   0;
    setRGB1(t1, 25,  0, 29); t1->x1 = 320; t1->y1 = 240;
    setRGB2(t1, 25,  0, 29); t1->x2 =   0; t1->y2 = 240;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], t1);
    ctx->next_packet += sizeof(POLY_G3);
}

void draw_faces(
    RenderContext *ctx,
    SVECTOR *verts,
    int faces[][4],
    uint8_t colors[][3],
    int face_count,
    int depth_bias
) {
    int i;
    for (i = 0; i < face_count; i++) {
        DVECTOR sv[4];
        int32_t otz, nclip;

        gte_ldv3(&verts[faces[i][0]], &verts[faces[i][1]], &verts[faces[i][2]]);
        gte_rtpt();
        gte_stsxy3c(sv);

        gte_nclip();
        gte_stopz(&nclip);
        if (nclip <= 0) continue;

        gte_ldv0(&verts[faces[i][3]]);
        gte_rtps();
        gte_stsxy(&sv[3]);

        gte_avsz4();
        gte_stotz(&otz);
        /* Horizontal faces (crate tops etc.) sort by their farthest corner so
           vertical faces standing against them win the seam (see render.h).
           After 4 RTPS the SZ FIFO holds all four corners. */
        if (poly_is_flat_y(&verts[faces[i][0]], &verts[faces[i][1]],
                           &verts[faces[i][2]], &verts[faces[i][3]])) {
            int32_t sz[4];
            gte_stsz4c(sz);
            otz = otz_far4(sz[0], sz[1], sz[2], sz[3]);
        }

        otz += depth_bias;
        if (otz <= 0) continue;
        if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
        if (otz >= OT_LENGTH) otz = OT_LENGTH - 1;

        int32_t face_cx = ((int32_t)verts[faces[i][0]].vx + verts[faces[i][2]].vx) / 2;
        int32_t face_cz = ((int32_t)verts[faces[i][0]].vz + verts[faces[i][2]].vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

        int32_t fog_start = 500;
        int32_t fog_end   = 3000;
        int32_t fog = dist;
        if (fog < fog_start) fog = fog_start;
        if (fog > fog_end)   fog = fog_end;
        int32_t range = fog_end - fog_start;
        int32_t fog_factor = ((fog_end - fog) << 8) / range;

        uint8_t r = (uint8_t)(((int32_t)colors[i][0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)colors[i][1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)colors[i][2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) continue;

        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, r, g, b);

        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
        poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}
