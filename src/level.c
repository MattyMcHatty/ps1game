#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "level.h"

#include "vampire.h"
#include "player.h"
#include "bat.h"
#include "medipac.h"
#include "particles.h"

static void draw_panel(
    RenderContext *ctx,
    int ox, int oy, int oz,
    int ux, int uy, int uz,
    int vx, int vy, int vz,
    int segs_u, int segs_v,
    uint8_t cr, uint8_t cg, uint8_t cb,
    int depth_bias
) {
    int i, j;
    for (j = 0; j < segs_v; j++) {
        for (i = 0; i < segs_u; i++) {
            int bx = ox + i*ux + j*vx;
            int by = oy + i*uy + j*vy;
            int bz = oz + i*uz + j*vz;

            SVECTOR v[4];
            v[0].vx = bx;       v[0].vy = by;       v[0].vz = bz;       v[0].pad = 0;
            v[1].vx = bx+ux;    v[1].vy = by+uy;    v[1].vz = bz+uz;    v[1].pad = 0;
            v[2].vx = bx+ux+vx; v[2].vy = by+uy+vy; v[2].vz = bz+uz+vz; v[2].pad = 0;
            v[3].vx = bx+vx;    v[3].vy = by+vy;    v[3].vz = bz+vz;    v[3].pad = 0;

            DVECTOR sv[4];
            int32_t sz[4];
            int32_t otz, nclip;

            gte_ldv3(&v[0], &v[1], &v[2]);
            gte_rtpt();
            gte_stsxy3c(sv);

            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) continue;

            gte_ldv0(&v[3]);
            gte_rtps();
            gte_stsxy(&sv[3]);

            gte_stsz4c(sz);
            if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) continue;

            gte_avsz4();
            gte_stotz(&otz);

            otz += depth_bias;
            if (otz <= 0) continue;
            if (otz >= OT_LENGTH) otz = OT_LENGTH - 1;

            int32_t face_cx = bx + (ux + vx) / 2;
            int32_t face_cz = bz + (uz + vz) / 2;
            int32_t dx = face_cx - cam_x;
            int32_t dz = face_cz - cam_z;
            int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

            int32_t fog_start = 500;
            int32_t fog_end   = 3000;
            int32_t fog = dist;
            if (fog < fog_start) fog = fog_start;
            if (fog > fog_end)   fog = fog_end;
            int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

            uint8_t r = (uint8_t)((cr * fog_factor) >> 8);
            uint8_t g = (uint8_t)((cg * fog_factor) >> 8);
            uint8_t b = (uint8_t)((cb * fog_factor) >> 8);

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
}

static void draw_room(RenderContext *ctx) {
    draw_panel(ctx,  1800,-300,-1800,  -300,0,0,  0,600,0,  12,1,  80,80,80,  400);
    draw_panel(ctx, -1800,-300, 1800,   300,0,0,  0,600,0,  12,1,  80,80,80,  400);
    draw_panel(ctx,  1800,-300, 1800,  0,0,-300,  0,600,0,  12,1,  60,60,60,  400);
    draw_panel(ctx, -1800,-300,-1800,  0,0, 300,  0,600,0,  12,1,  60,60,60,  400);
    draw_panel(ctx, -1800,-300,-1800,   300,0,0,  0,0,300,  12,12,  40,40,40,  400);
    draw_panel(ctx, -1800, 300,-1800,  0,0,300,  300,0,0,   12,12,  50,50,70,  400);
}

void draw_scene(RenderContext *ctx) {
    MATRIX rot_matrix;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};

    RotMatrix(&neg_rot, &rot_matrix);

    VECTOR trans;
    trans.vx = -cam_x;
    trans.vy = 0;
    trans.vz = -cam_z;

    ApplyMatrixLV(&rot_matrix, &trans, &trans);

    rot_matrix.t[0] = trans.vx;
    rot_matrix.t[1] = trans.vy;
    rot_matrix.t[2] = trans.vz;

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_room(ctx);
    draw_vampire(ctx);
    draw_particles(ctx);
    draw_medipac(ctx);
    draw_bat(ctx);
    draw_hud(ctx);
}
