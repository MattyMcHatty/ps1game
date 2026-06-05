#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "level.h"
#include "collision.h"

#include "vampire.h"
#include "player.h"
#include "bat.h"
#include "sml_med.h"
#include "particles.h"
#include "crate.h"
#include "key.h"
#include "door.h"
#include "level1_tex_map.h"

static SMD  *room_smd  = NULL;
static void *room_buff = NULL;

/* tpage/clut for each texture: [0]=gravel [1]=rusty fence [2]=brick_wall [3]=double_door */
static uint16_t tex_tpage[4] = {0, 0, 0, 0};
static uint16_t tex_clut[4]  = {0, 0, 0, 0};

static void *load_file_from_cd(const char *filename, int *size_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) {
        if (size_out) *size_out = 0;
        return NULL;
    }
    int sectors = (file.size + 2047) / 2048;
    void *buff = malloc(sectors * 2048);
    if (!buff) {
        if (size_out) *size_out = 0;
        return NULL;
    }
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    if (size_out) *size_out = file.size;
    return buff;
}

static void load_tim_to_vram(const char *filename, int slot) {
    void *buf = load_file_from_cd(filename, NULL);
    if (!buf) return;

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);

    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);

    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
    }

    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    tex_clut[slot]  = getClut(tim.crect->x, tim.crect->y);

    free(buf);
}

void level_init(void) {
    CdInit();
    scSetClipRect(0, 0, SCREEN_XRES, SCREEN_YRES);

    load_tim_to_vram("\\GRAVEL.TIM;1",  0);
    load_tim_to_vram("\\FENCE.TIM;1",   1);
    load_tim_to_vram("\\BRIKWLL.TIM;1", 2);
    load_tim_to_vram("\\DBLDOOR.TIM;1", 3);

    room_buff = load_file_from_cd("\\TRMQ.SMD", NULL);
    if (room_buff) {
        room_smd = smdInitData(room_buff);
    }
}

/* SMD FT4 layout, stride=32:
   [0-3] SMD_PRI_TYPE  [4-11] v0..v3 uint16  [12-13] n0  [14-15] pad
   [16-18] r,g,b  [19] code  [20-27] UVs (ignored)  [28-31] tpage/clut (ignored)
   Texture per primitive from level1_tex_map.h (0xFF = untextured). */

/* World-space UV projection: floor uses XZ, walls use along-wall + Y. */
static void compute_uv(SVECTOR *v, SVECTOR *norm,
                        uint8_t *u_out, uint8_t *v_out) {
    int32_t abs_nx = norm->vx < 0 ? -norm->vx : norm->vx;
    int32_t abs_ny = norm->vy < 0 ? -norm->vy : norm->vy;
    int32_t abs_nz = norm->vz < 0 ? -norm->vz : norm->vz;

    /* Mask to 0-127: each texture has its own tpage, so V stays in 0-127. */
    if (abs_ny > abs_nx && abs_ny > abs_nz) {
        *u_out = (uint8_t)((v->vx >> 1) & 0x7F);
        *v_out = (uint8_t)((v->vz >> 1) & 0x7F);
    } else if (abs_nx >= abs_nz) {
        *u_out = (uint8_t)((v->vz >> 2) & 0x7F);
        *v_out = (uint8_t)((v->vy)      & 0x7F);
    } else {
        *u_out = (uint8_t)((v->vx >> 2) & 0x7F);
        *v_out = (uint8_t)((v->vy)      & 0x7F);
    }
}
static void draw_smd_room(RenderContext *ctx) {
    uint8_t *p = (uint8_t *)room_smd->p_prims;
    int i;
    for (i = 0; i < room_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &room_smd->p_verts[vi[0]];
        SVECTOR *v1 = &room_smd->p_verts[vi[1]];
        SVECTOR *v2 = &room_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 2250)
                { p += stride; continue; }
        }

        DVECTOR sv[4];
        int32_t sz[4];
        int32_t otz, nclip;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);

        /* Reject only GTE-saturated vertices (overflow causes GPU raster glitches).
           GTE clamps SXY to 11-bit signed +-1023; legitimate off-screen vertices
           can project well outside the screen rect without saturating. */
        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        if (!pt->nocull && !is_quad) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        /* After gte_rtpt(), SZ FIFO = [stale, v0, v1, v2] stored as sz[0..3].
           sz[0] is the discarded oldest value — only check sz[1]/sz[2]/sz[3]. */
        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        if (is_quad) {
            SVECTOR *v3 = &room_smd->p_verts[vi[3]];
            gte_ldv0(v3);
            gte_rtps();
            gte_stsxy(&sv[3]);
            gte_stsz(&sz[3]);
            if (sv[3].vx <= -1023 || sv[3].vx >= 1023 || sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
            if (sz[3] == 0) { p += stride; continue; }
            gte_avsz4();
        } else {
            gte_avsz3();
        }

        gte_stotz(&otz);
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        if (otz >= OT_LENGTH) otz = OT_LENGTH - 1;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 500, fog_end = 2200;
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        uint8_t tex_idx = (i < LEVEL1_PRIM_COUNT) ? level1_tex_map[i] : 0xFF;

        if (is_quad && tex_idx != 0xFF) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            SVECTOR *v3 = &room_smd->p_verts[vi[3]];
            SVECTOR *norm = &room_smd->p_norms[vi[4]];
            uint8_t u0,uv0, u1,uv1, u2,uv2, u3,uv3;
            compute_uv(v0, norm, &u0, &uv0);
            compute_uv(v1, norm, &u1, &uv1);
            compute_uv(v2, norm, &u2, &uv2);
            compute_uv(v3, norm, &u3, &uv3);
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8));
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
            poly->u0=u0; poly->v0=uv0;
            poly->u1=u1; poly->v1=uv1;
            poly->u2=u2; poly->v2=uv2;
            poly->u3=u3; poly->v3=uv3;
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT4);
        } else if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8));
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else {
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8));
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }

        p += stride;
    }
}

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

            uint8_t r = (uint8_t)(((int32_t)cr * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)cg * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)cb * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

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
    draw_sky_gradient(ctx);

    MATRIX rot_matrix;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};

    RotMatrix(&neg_rot, &rot_matrix);

    VECTOR trans;
    trans.vx = -cam_x;
    trans.vy = -cam_y;
    trans.vz = -cam_z;

    ApplyMatrixLV(&rot_matrix, &trans, &trans);

    rot_matrix.t[0] = trans.vx;
    rot_matrix.t[1] = trans.vy;
    rot_matrix.t[2] = trans.vz;

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    if (room_smd) {
        draw_smd_room(ctx);
    } else {
        draw_room(ctx);
    }
    crates_draw(ctx);
    /* crates_draw sets a per-crate GTE matrix — restore the view matrix
       so all subsequent world-space draws project correctly. */
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);
    draw_vampire(ctx);
    draw_particles(ctx);
    sml_meds_draw(ctx);
    keys_draw(ctx);
    door_draw(ctx);
    draw_bat(ctx);
    draw_hud(ctx);

#ifdef DEBUG_COLLISION
    debug_draw_walls(ctx);
    debug_draw_coords(ctx);
#endif
}
