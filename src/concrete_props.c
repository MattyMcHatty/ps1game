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
#include "texmgr.h"
#include "title.h"          /* current_area gate: props only live in the conservatory */
#include "concrete_props.h"

#define MAX_CONCRETE_PROPS   8
#define CPROP_PUSH_MARGIN   30   /* extra gap between player and prop edge (as tables) */

/* Per-model footprint half-extents and solid height, taken from the SMX bboxes:
   block is a wide low slab (x +/-195, z +/-70, ~100 tall); chair is small and
   tall (x +/-35, z +/-40, ~176 tall). */
static void model_dims(int model, int32_t *half_w, int32_t *half_d, int32_t *solid_h) {
    if (model == CONCRETE_BLOCK) {
        *half_w = 195; *half_d = 70;  *solid_h = 100;
    } else {
        *half_w = 35;  *half_d = 40;  *solid_h = 176;
    }
}

typedef struct {
    int      model;                      /* CONCRETE_BLOCK / CONCRETE_CHAIR */
    int32_t  x, y, z, rot_y;             /* y = standing reference; world = y+GROUND_FLOOR_Y */
    int32_t  active;
} ConcreteProp;

static ConcreteProp props[MAX_CONCRETE_PROPS];
static int          prop_count = 0;

static SMD  *block_smd = NULL, *chair_smd = NULL;
static void *block_buf = NULL, *chair_buf = NULL;
static int   cncrte_tex = -1;   /* texmgr id of the shared concrete texture */

static void *read_file(const char *name) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)name)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

/* Startup: load both model SMDs and register the shared texture (RAM-resident
   for a pure-LoadImage upload on room entry). */
void concrete_props_load_assets(void) {
    block_buf = read_file("\\TEX\\CONBLOCK.SMD;1");
    if (block_buf) block_smd = smdInitData(block_buf);
    chair_buf = read_file("\\TEX\\CONCHAIR.SMD;1");
    if (chair_buf) chair_smd = smdInitData(chair_buf);

    cncrte_tex = texmgr_register("\\TEX\\CNCRTE.TIM;1");
}

/* Upload the concrete texture into its VRAM slot (kchn_tile's, which the
   conservatory does not use) from the resident RAM copy. Pure LoadImage. */
void concrete_props_upload_textures(void) {
    texmgr_upload(cncrte_tex);
}

static void add_prop(int model, int32_t x, int32_t y, int32_t z, int32_t rot_y) {
    if (prop_count >= MAX_CONCRETE_PROPS) return;
    ConcreteProp *p = &props[prop_count++];
    p->model  = model;
    p->x = x;  p->y = y;  p->z = z;
    p->rot_y  = rot_y;
    p->active = 1;
}

/* Place the conservatory's concrete props. y = -149 seats a base-origin model
   on the conservatory floor (world y=0); rot_y is 0..4096 = a full turn
   (512 = 45deg, 1024 = 90deg). */
void concrete_props_place(void) {
    /* All nudged 150 west (-x), toward the west wall (x=-2599). */
    prop_count = 0;
    add_prop(CONCRETE_BLOCK, -2236, -149, 1218, 1536);   /* rot 90deg           */
    add_prop(CONCRETE_CHAIR, -2416, -149,  921,    0);   /* another -45deg CCW  */
    add_prop(CONCRETE_CHAIR, -2456, -149,  590,    0);   /* another -45deg CCW  */
    add_prop(CONCRETE_CHAIR, -1818, -149, 1483,  512);   /* +200 north (+z) */
}

/* Axis-aligned bound of the rotated footprint (same maths as the dresser). */
static void rotated_half_extents(int model, int32_t rot_y, int32_t *hw, int32_t *hd) {
    int32_t half_w, half_d, sh;
    model_dims(model, &half_w, &half_d, &sh);
    int32_t c = icos(rot_y), s = isin(rot_y);
    if (c < 0) c = -c;
    if (s < 0) s = -s;
    *hw = (half_w * c + half_d * s) >> 12;
    *hd = (half_w * s + half_d * c) >> 12;
}

/* Player push-out (dresser-style Minkowski AABB). Gated to the conservatory so
   the shared reception collision routine can call this unconditionally. */
void concrete_props_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;   /* single flat floor — no vertical gating needed */
    if (current_area != STATE_CONSERVATORY) return;
    int i;
    for (i = 0; i < prop_count; i++) {
        ConcreteProp *p = &props[i];
        if (!p->active) continue;

        int32_t hw, hd;
        rotated_half_extents(p->model, p->rot_y, &hw, &hd);

        int32_t min_x = p->x - hw - radius - CPROP_PUSH_MARGIN;
        int32_t max_x = p->x + hw + radius + CPROP_PUSH_MARGIN;
        int32_t min_z = p->z - hd - radius - CPROP_PUSH_MARGIN;
        int32_t max_z = p->z + hd + radius + CPROP_PUSH_MARGIN;

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

int concrete_props_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack) {
    if (current_area != STATE_CONSERVATORY) return 0;
    int i;
    for (i = 0; i < prop_count; i++) {
        ConcreteProp *p = &props[i];
        if (!p->active) continue;
        /* Vertical span in world Y: base rests on the floor at
           (p->y + GROUND_FLOOR_Y), body reaches solid_h above it. */
        int32_t half_w, half_d, solid_h;
        model_dims(p->model, &half_w, &half_d, &solid_h);
        int32_t base = p->y + GROUND_FLOOR_Y;
        if (y < base - solid_h || y > base) continue;
        int32_t hw, hd;
        rotated_half_extents(p->model, p->rot_y, &hw, &hd);
        if (x < p->x - hw - slack || x > p->x + hw + slack) continue;
        if (z < p->z - hd - slack || z > p->z + hd + slack) continue;
        return 1;
    }
    return 0;
}

/* Render all placed props with the conservatory's fog and texture window. Both
   models are fully textured with cncrte; horizontal faces use the far-corner
   sort (render.h). Restores the caller's view matrix before returning. */
void concrete_props_draw(RenderContext *ctx) {
    if (prop_count == 0) return;

    MATRIX view;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &view);
    VECTOR vt = {-cam_x, -cam_y, -cam_z};
    ApplyMatrixLV(&view, &vt, &vt);
    view.t[0] = vt.vx;
    view.t[1] = vt.vy;
    view.t[2] = vt.vz;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(cncrte_tex);
    uint16_t cl = texmgr_clut(cncrte_tex);

    int i;
    for (i = 0; i < prop_count; i++) {
        ConcreteProp *pr = &props[i];
        if (!pr->active) continue;
        SMD *smd = (pr->model == CONCRETE_CHAIR) ? chair_smd : block_smd;
        if (!smd) continue;

        int32_t dcx = pr->x - cam_x, dcz = pr->z - cam_z;
        if ((dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz) > 1500) continue;

        MATRIX pm, combined;
        SVECTOR prot = {0, pr->rot_y, 0, 0};
        RotMatrix(&prot, &pm);
        VECTOR pos = {pr->x, pr->y + GROUND_FLOOR_Y, pr->z};
        TransMatrix(&pm, &pos);
        CompMatrixLV(&view, &pm, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        uint8_t *p = (uint8_t *)smd->p_prims;
        int pi;
        for (pi = 0; pi < smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
            uint8_t       stride = pt->len;
            int           is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &smd->p_verts[vi[0]];
            SVECTOR *v1 = &smd->p_verts[vi[1]];
            SVECTOR *v2 = &smd->p_verts[vi[2]];

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

            SVECTOR *v3    = 0;
            int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
            if (is_quad) {
                v3 = &smd->p_verts[vi[3]];
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
            /* Horizontal polys sort by their farthest corner (see render.h). */
            if (poly_is_flat_y(v0, v1, v2, v3))
                otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                              : otz_far3(sz[1], sz[2], sz[3]);
            if (otz <= 0) { p += stride; continue; }
            otz += 40;
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            int32_t fog_start = 350, fog_end = 1500;   /* matches the room */
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
                poly->tpage = tp;
                poly->clut  = cl;
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
                poly->tpage = tp;
                poly->clut  = cl;
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
