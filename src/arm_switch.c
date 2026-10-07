#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "texmgr.h"
#include "title.h"          /* current_area gate */
#include "arm_switch.h"

/* The Arm Switches — see arm_switch.h for why the export fixes X and Y and
   every switch is the same mesh slid along Z. */

static GameState as_area  = STATE_TITLE;
static int       as_count = 0;
static int32_t   as_z[ARM_SWITCH_MAX];     /* each switch's centre Z */

static SMD  *as_smd = NULL;
static void *as_buf = NULL;

/* The export's box, measured at load. WORLD space (arm_switch.h). Only the
   centre is used: X/Y for the cull and fog, Z as the origin each switch's
   offset is taken from. */
static int32_t as_cx = 0, as_cz = 0;       /* 2326, 2592 as authored */

static int as_tex = -1;

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

/* Startup. One sector of geometry held for the run, and a texture registration
   that reads nothing but a TIM header until the Catacombs bank is selected. */
void arm_switch_load_assets(void) {
    /* BANK: Chapter 3 only. Derived, not guessed — py tools/check_tex_banks.py
       walks the uploader call graph (the_head_upload_textures reaches this
       module) and fails if CATACOMBS is not in the mask. */
    texmgr_set_bank(TEXBANK_CATACOMBS);

    as_buf = read_file("\\TEXCTCMB\\ARMSWTCH.SMD;1");
    if (as_buf) as_smd = smdInitData(as_buf);

    /* MEASURE THE MESH. No re-centre: the export's own X and Y are where
       every switch stands, and its Z centre is what each one slides from. */
    if (as_smd && as_smd->n_verts > 0) {
        int i;
        int32_t min_x, max_x, min_z, max_z;
        min_x = max_x = as_smd->p_verts[0].vx;
        min_z = max_z = as_smd->p_verts[0].vz;
        for (i = 1; i < as_smd->n_verts; i++) {
            int32_t vx = as_smd->p_verts[i].vx;
            int32_t vz = as_smd->p_verts[i].vz;
            if (vx < min_x) min_x = vx;
            if (vx > max_x) max_x = vx;
            if (vz < min_z) min_z = vz;
            if (vz > max_z) max_z = vz;
        }
        as_cx = (min_x + max_x) / 2;
        as_cz = (min_z + max_z) / 2;
    }

    as_tex = texmgr_register("\\TEXCTCMB\\LMSHARM.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. Called from the_head_upload_textures(). */
void arm_switch_upload_texture(void) {
    texmgr_upload(as_tex);
}

void arm_switches_clear(void) { as_count = 0; }

void arm_switch_add(GameState area, int32_t z) {
    if (as_count >= ARM_SWITCH_MAX) return;
    as_area = area;
    as_z[as_count++] = z;
}

/* One switch: the Gula Tablet's primitive loop, with the Z slide folded into
   the view's translation. A world point p moved by (0,0,dz) is p + dz*e_z,
   which the view takes to R*p + (T + dz * R's z column) — one multiply per
   row, no model matrix. */
static void draw_one(RenderContext *ctx, const MATRIX *base, int32_t z,
                     uint8_t *buf_end, uint16_t tp, uint16_t cl) {
    int32_t dcx = as_cx - cam_x, dcz = z - cam_z;
    int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
    dist = render_light_dist(as_cx, z, dist);
    if (dist > g_fog_far) return;

    MATRIX view = *base;
    int32_t dz = z - as_cz;
    if (dz) {
        view.t[0] += (view.m[0][2] * dz) >> 12;
        view.t[1] += (view.m[1][2] * dz) >> 12;
        view.t[2] += (view.m[2][2] * dz) >> 12;
    }
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    int32_t fog_factor = render_fog_scale(dist);

    uint8_t *p = (uint8_t *)as_smd->p_prims;
    int pi;
    for (pi = 0; pi < as_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
        uint8_t       stride   = pt->len;
        int           is_quad  = (pt->type >= 2);
        int           textured = pt->texture;

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &as_smd->p_verts[vi[0]];
        SVECTOR *v1 = &as_smd->p_verts[vi[1]];
        SVECTOR *v2 = &as_smd->p_verts[vi[2]];

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
            SVECTOR *v3 = &as_smd->p_verts[vi[3]];
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
        /* NO +40, the crib's rule: the arm is sunk into the relief, so at the
           room's own bias its root would tie with the wall and lose. */
        if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        /* Fog to the chapter's clear colour (7,6,9), as every Chapter 3
           prop's is. */
        uint8_t *col = p + 16;
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 7 * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 6 * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 9 * (256 - fog_factor)) >> 8);

        if (is_quad && textured) {
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
        } else if (is_quad) {
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
        } else if (textured) {
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

void arm_switches_draw(RenderContext *ctx) {
    if (!as_count || as_area != current_area || !as_smd) return;

    MATRIX base;
    camera_build_view(&base);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(as_tex);
    uint16_t cl = texmgr_clut(as_tex);

    int i;
    for (i = 0; i < as_count; i++)
        draw_one(ctx, &base, as_z[i], buf_end, tp, cl);
}
