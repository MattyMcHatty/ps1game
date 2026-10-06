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
#include "title.h"          /* current_area gate */
#include "gula_tablet.h"

/* The Gula Tablet — see gula_tablet.h for why its mesh is its position as well
   as its collision. */

static GameState gt_area   = STATE_TITLE;
static int       gt_active = 0;

static SMD  *gt_smd = NULL;
static void *gt_buf = NULL;

/* The collision mesh, and it is the SAME mesh the draw uses, measured at load.
   WORLD space, since the export is (gula_tablet.h). -Y is up, so gt_min_y is
   the top of the slab and gt_max_y its base. */
static int32_t gt_min_x = 0, gt_max_x = 0;   /* -1292 .. -1242 as authored */
static int32_t gt_min_z = 0, gt_max_z = 0;   /*  -300 ..   300             */
static int32_t gt_min_y = 0, gt_max_y = 0;   /*  -800 ..     0             */

static int gt_tex = -1;

/* How far the slab stands above its export's position, in world units (+ve is
   up). gula_tablet_set_rise(); 0 on every gula_tablet_place(). */
static int32_t gt_rise = 0;

/* The player's head, relative to cam_y — the crib's CR_PLAYER_HEAD, and the
   same figure apply_collision_* uses for its own body span. */
#define GT_PLAYER_HEAD 30

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
void gula_tablet_load_assets(void) {
    /* BANK: Chapter 3 only. Derived, not guessed — py tools/check_tex_banks.py
       walks the uploader call graph (nursery_upload_textures reaches this
       module) and fails if CATACOMBS is not in the mask. */
    texmgr_set_bank(TEXBANK_CATACOMBS);

    gt_buf = read_file("\\TEXCTCMB\\GULATBLT.SMD;1");
    if (gt_buf) gt_smd = smdInitData(gt_buf);

    /* MEASURE THE MESH. No re-centre: the export's own coordinates are where
       it stands (gula_tablet.h). */
    if (gt_smd && gt_smd->n_verts > 0) {
        int i;
        gt_min_x = gt_max_x = gt_smd->p_verts[0].vx;
        gt_min_y = gt_max_y = gt_smd->p_verts[0].vy;
        gt_min_z = gt_max_z = gt_smd->p_verts[0].vz;
        for (i = 1; i < gt_smd->n_verts; i++) {
            int32_t vx = gt_smd->p_verts[i].vx;
            int32_t vy = gt_smd->p_verts[i].vy;
            int32_t vz = gt_smd->p_verts[i].vz;
            if (vx < gt_min_x) gt_min_x = vx;
            if (vx > gt_max_x) gt_max_x = vx;
            if (vy < gt_min_y) gt_min_y = vy;
            if (vy > gt_max_y) gt_max_y = vy;
            if (vz < gt_min_z) gt_min_z = vz;
            if (vz > gt_max_z) gt_max_z = vz;
        }
    }

    gt_tex = texmgr_register("\\TEXCTCMB\\GULATBLT.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. Called from nursery_upload_textures(). */
void gula_tablet_upload_texture(void) {
    texmgr_upload(gt_tex);
}

void gula_tablets_clear(void) { gt_active = 0; }

void gula_tablet_place(GameState area) {
    gt_area   = area;
    gt_active = 1;
    gt_rise   = 0;
}

void gula_tablet_set_rise(int32_t rise) { gt_rise = rise; }

/* Player push-out against the measured box, Minkowski-expanded by the caller's
   radius and resolved along the shallowest axis — the crib's scheme. */
void gula_tablets_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    if (!gt_active || gt_area != current_area || !gt_smd) return;

    /* -Y is up, so a slab lifted by gt_rise spans y[min - rise, max - rise].
       Lifted clear of the head, the test below lets the player walk under. */
    int32_t feet = py + GROUND_FLOOR_Y, head = py - GT_PLAYER_HEAD;
    if (head >= gt_max_y - gt_rise || feet <= gt_min_y - gt_rise) return;

    int32_t min_x = gt_min_x - radius, max_x = gt_max_x + radius;
    int32_t min_z = gt_min_z - radius, max_z = gt_max_z + radius;
    if (*px <= min_x || *px >= max_x) return;
    if (*pz <= min_z || *pz >= max_z) return;

    int32_t pl = *px - min_x, pr = max_x - *px;
    int32_t pf = *pz - min_z, pb = max_z - *pz;
    int32_t m = pl, ddx = -pl, ddz = 0;
    if (pr < m) { m = pr; ddx =  pr; ddz = 0; }
    if (pf < m) { m = pf; ddx = 0; ddz = -pf; }
    if (pb < m) {         ddx = 0; ddz =  pb; }
    *px += ddx; *pz += ddz;
}

/* The crib's primitive loop with no model matrix: the vertices are already
   world space, so the plain view transform is the whole of it. */
void gula_tablets_draw(RenderContext *ctx) {
    if (!gt_active || gt_area != current_area || !gt_smd) return;

    /* Cull at the room's fog-far, from the box's plan centre, through the
       room's lights — the crib's rule. */
    int32_t gcx = (gt_min_x + gt_max_x) / 2, gcz = (gt_min_z + gt_max_z) / 2;
    int32_t dcx = gcx - cam_x, dcz = gcz - cam_z;
    int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
    dist = render_light_dist(gcx, gcz, dist);
    if (dist > g_fog_far) return;

    MATRIX view;
    camera_build_view(&view);
    /* THE RISE, folded into the translation: a world point p lifted by r is
       p + (0,-r,0), which the view takes to R*p + (T - r * R's y column). One
       multiply per row, no model matrix. */
    if (gt_rise) {
        view.t[0] -= (view.m[0][1] * gt_rise) >> 12;
        view.t[1] -= (view.m[1][1] * gt_rise) >> 12;
        view.t[2] -= (view.m[2][1] * gt_rise) >> 12;
    }
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(gt_tex);
    uint16_t cl = texmgr_clut(gt_tex);
    int32_t  fog_factor = render_fog_scale(dist);

    uint8_t *p = (uint8_t *)gt_smd->p_prims;
    int pi;
    for (pi = 0; pi < gt_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
        uint8_t       stride   = pt->len;
        int           is_quad  = (pt->type >= 2);
        int           textured = pt->texture;

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &gt_smd->p_verts[vi[0]];
        SVECTOR *v1 = &gt_smd->p_verts[vi[1]];
        SVECTOR *v2 = &gt_smd->p_verts[vi[2]];

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
            SVECTOR *v3 = &gt_smd->p_verts[vi[3]];
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
        /* NO +40, the crib's rule: the slab stands 1 unit off the west wall, so
           at the room's own bias it would tie with the stone behind it and
           lose. True depth lifts it 160 units clear; a wall genuinely in front
           is still hundreds of buckets nearer. */
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
