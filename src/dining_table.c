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
#include "dining_table.h"

DiningTable dining_tables[MAX_DINING_TABLES];
int         dining_table_count = 0;

static SMD  *table_smd    = NULL;
static void *table_buffer = NULL;

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

void dining_tables_init(void) {
    /* Load the table geometry. The model is textured with wd_flr, which the
       kitchen already keeps resident in VRAM — dining_tables_draw is handed
       that texture's tpage/clut, so no texture is loaded here. */
    load_file("\\DINTABLE.SMD;1", &table_buffer);
    if (table_buffer)
        table_smd = smdInitData(table_buffer);

    /* Five tables across the large kitchen room. y=-149 is the kitchen
       "standing on the floor" reference (matches the player/zombie); the draw
       adds GROUND_FLOOR_Y so the model's feet rest on the floor at world y=0.
       Footprint ~306x200 (tabletop X ±153, Z ±100).
       Each gets its own Y rotation (0..4096 = full turn) so the tables face
       different directions and look haphazardly arranged. */
    static const int32_t pos[][3] = {
        /*   x      z    rot_y */
        { -1262, -536,   412 },
        { -1418,  556,  1730 },
        { -2035, -107,  2950 },
        { -2577, -520,   880 },
        { -2456,  525,  3580 },
    };
    int i;
    for (i = 0; i < (int)(sizeof(pos) / sizeof(pos[0])); i++) {
        dining_tables[i].x      = pos[i][0];
        dining_tables[i].y      = -149;
        dining_tables[i].z      = pos[i][1];
        dining_tables[i].rot_y  = pos[i][2];
        dining_tables[i].active = 1;
        dining_tables[i].half_w = 153;   /* tabletop footprint: X ±153, Z ±100 */
        dining_tables[i].half_d = 100;
    }
    dining_table_count = i;
}

void dining_tables_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;   /* kitchen is a single floor level — no vertical gating needed */
    int i;
    for (i = 0; i < dining_table_count; i++) {
        DiningTable *t = &dining_tables[i];
        if (!t->active) continue;

        /* Minkowski-expanded AABB plus a small push margin (same scheme as
           Crate) so the camera stops clear of the visible tabletop edges. */
        int32_t min_x = t->x - t->half_w - radius - DTABLE_PUSH_MARGIN;
        int32_t max_x = t->x + t->half_w + radius + DTABLE_PUSH_MARGIN;
        int32_t min_z = t->z - t->half_d - radius - DTABLE_PUSH_MARGIN;
        int32_t max_z = t->z + t->half_d + radius + DTABLE_PUSH_MARGIN;

        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        /* Push out along the axis with the smallest penetration. */
        int32_t push_l = *px - min_x;
        int32_t push_r = max_x - *px;
        int32_t push_f = *pz - min_z;
        int32_t push_b = max_z - *pz;

        int32_t min_push = push_l, px_delta = -push_l, pz_delta = 0;
        if (push_r < min_push) { min_push = push_r; px_delta =  push_r; pz_delta = 0; }
        if (push_f < min_push) { min_push = push_f; px_delta = 0; pz_delta = -push_f; }
        if (push_b < min_push) {                    px_delta = 0; pz_delta =  push_b; }

        *px += px_delta;
        *pz += pz_delta;
    }
}

/* Render the tables using the SAME textured-prim path as the kitchen geometry
   (per-poly UVs from the SMD, the 128 texture window set in kitchen_dining_draw,
   and matching distance fog) so they blend in with the room. The caller passes
   the wd_flr texture's VRAM tpage/clut. */
void dining_tables_draw(RenderContext *ctx, uint16_t tpage, uint16_t clut) {
    if (!table_smd) return;

    /* Camera view matrix (same as the kitchen/crate renderers). */
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
    for (i = 0; i < dining_table_count; i++) {
        DiningTable *t = &dining_tables[i];
        if (!t->active) continue;

        int32_t dcx = t->x - cam_x, dcz = t->z - cam_z;
        if ((dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz) > 2600) continue;

        /* Combine the view matrix with this table's world transform. */
        MATRIX table_m, combined;
        SVECTOR tr = {0, t->rot_y, 0, 0};
        RotMatrix(&tr, &table_m);
        VECTOR pos = {t->x, t->y + GROUND_FLOOR_Y, t->z};
        TransMatrix(&table_m, &pos);
        CompMatrixLV(&view, &table_m, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        uint8_t *p = (uint8_t *)table_smd->p_prims;
        int pi;
        for (pi = 0; pi < table_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
            uint8_t       stride = pt->len;
            int           is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &table_smd->p_verts[vi[0]];
            SVECTOR *v1 = &table_smd->p_verts[vi[1]];
            SVECTOR *v2 = &table_smd->p_verts[vi[2]];

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
                SVECTOR *v3 = &table_smd->p_verts[vi[3]];
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
            /* Stay below the texture-window primitive at OT_LENGTH-1 so it is
               processed first (same rule as the kitchen geometry). */
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            /* Distance fog from the table centre — cheap and consistent with
               the room (the model is small relative to the fog band). */
            int32_t fog_start = 500, fog_end = 2200;
            int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
            int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
            int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

            if (is_quad) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                uint8_t *uv = p + 20;
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                setRGB0(poly, r, g, b);
                poly->tpage = tpage;
                poly->clut  = clut;
                poly->u0=uv[0]; poly->v0=uv[1];
                poly->u1=uv[2]; poly->v1=uv[3];
                poly->u2=uv[4]; poly->v2=uv[5];
                poly->u3=uv[6]; poly->v3=uv[7];
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT4);
            } else {
                if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
                uint8_t *uv = p + 20;
                POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
                setPolyFT3(poly);
                setRGB0(poly, r, g, b);
                poly->tpage = tpage;
                poly->clut  = clut;
                poly->u0=uv[0]; poly->v0=uv[1];
                poly->u1=uv[2]; poly->v1=uv[3];
                poly->u2=uv[4]; poly->v2=uv[5];
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT3);
            }

            p += stride;
        }
    }

    /* Restore the camera view matrix for whatever the caller draws next. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
