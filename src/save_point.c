#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "save_point.h"

SavePoint save_points[MAX_SAVE_POINTS];
int       save_point_count = 0;

static SMD  *sp_smd    = NULL;
static void *sp_buffer = NULL;

static void load_file(const char *name, void **buf_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, name)) { *buf_out = NULL; return; }
    int sectors = (file.size + 2047) / 2048;
    *buf_out = malloc(sectors * 2048);
    if (!*buf_out) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)*buf_out, CdlModeSpeed);
    CdReadSync(0, NULL);
}

void save_points_init(void) {
    load_file("\\SAVEPT.SMD;1", &sp_buffer);
    if (sp_buffer)
        sp_smd = smdInitData(sp_buffer);
}

void save_points_clear(void) {
    save_point_count = 0;
}

int save_point_add(int32_t x, int32_t y, int32_t z, int32_t rot_y, int32_t scale) {
    if (save_point_count >= MAX_SAVE_POINTS) return -1;
    int i = save_point_count++;
    save_points[i].x      = x;
    save_points[i].y      = y;
    save_points[i].z      = z;
    save_points[i].rot_y  = rot_y;
    save_points[i].scale  = scale;
    save_points[i].active = 1;
    return i;
}

/* Render all active instances with a flat-shaded prim path (the model has no
   textures — its per-face colours are baked into the SMD). Same view transform,
   screen-bounds/back-face culling, depth sort and interior fog as the other
   props, so it composes with the room geometry. */
void save_points_draw(RenderContext *ctx) {
    if (!sp_smd) return;

    MATRIX view;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &view);
    VECTOR vt = {-cam_x, -cam_y, -cam_z};
    ApplyMatrixLV(&view, &vt, &vt);
    view.t[0] = vt.vx;
    view.t[1] = vt.vy;
    view.t[2] = vt.vz;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < save_point_count; i++) {
        SavePoint *s = &save_points[i];
        if (!s->active) continue;

        int32_t dcx = s->x - cam_x, dcz = s->z - cam_z;
        if ((dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz) > 2300) continue;

        MATRIX m, combined;
        SVECTOR rr = {0, s->rot_y, 0, 0};
        RotMatrix(&rr, &m);
        /* Fold a uniform scale into the rotation part (4096 = 1.0). Scaling only
           m[][] (not t) shrinks the model about its own origin, so the feet stay
           on the floor. */
        if (s->scale != 4096) {
            int a, b;
            for (a = 0; a < 3; a++)
                for (b = 0; b < 3; b++)
                    m.m[a][b] = (int16_t)(((int32_t)m.m[a][b] * s->scale) >> 12);
        }
        VECTOR pos = {s->x, s->y + GROUND_FLOOR_Y, s->z};
        TransMatrix(&m, &pos);
        CompMatrixLV(&view, &m, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        uint8_t *p = (uint8_t *)sp_smd->p_prims;
        int pi;
        for (pi = 0; pi < sp_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
            uint8_t       stride = pt->len;
            int           is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &sp_smd->p_verts[vi[0]];
            SVECTOR *v1 = &sp_smd->p_verts[vi[1]];
            SVECTOR *v2 = &sp_smd->p_verts[vi[2]];

            DVECTOR sv[4];
            int32_t sz[4], otz, nclip;

            gte_ldv3(v0, v1, v2);
            gte_rtpt();
            gte_stsxy3c(sv);

            if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
                sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
                sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
                p += stride; continue;
            }

            if (!pt->nocull) {
                gte_nclip();
                gte_stopz(&nclip);
                if (nclip <= 0) { p += stride; continue; }
            }

            gte_stsz4c(sz);
            if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

            if (is_quad) {
                SVECTOR *v3 = &sp_smd->p_verts[vi[3]];
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
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            /* Distance fog toward the dark interior, matching the rooms. */
            int32_t fog_start = 350, fog_end = 2200;
            int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
            int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
            int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

            if (is_quad) {
                if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
                POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
                setPolyF4(poly);
                setRGB0(poly, r, g, b);
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
                setRGB0(poly, r, g, b);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F3);
            }

            p += stride;
        }
    }

    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
