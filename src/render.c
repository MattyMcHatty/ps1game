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

void flip_buffers(RenderContext *ctx) {
    DrawSync(0);
    VSync(0);

    RenderBuffer *draw_buffer = &ctx->buffers[ctx->active_buffer];
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

        otz += depth_bias;
        if (otz <= 0) continue;
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
