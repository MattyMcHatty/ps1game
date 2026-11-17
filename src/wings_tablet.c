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
#include "underworld.h"     /* the chapter's fog */
#include "underworld_sky.h" /* UW_SKY_OT         */
#include "wings_tablet.h"

/* The Wings Tablet - see wings_tablet.h. The Gula Tablet's module (an in-place
   export measured into an axis-aligned box) with Sharur's Tablet's Chapter 4
   draw terms: the chapter's fog colour, and the far clamp in front of the
   sky. */

static SMD  *wt_smd = NULL;
static void *wt_buf = NULL;

/* The collision box, measured off the SAME mesh the draw uses, at load. WORLD
   space, since the export is. -Y is up, so wt_min_y is the top of the slab and
   wt_max_y its base. */
static int32_t wt_min_x = 0, wt_max_x = 0;   /*  -168 .. 1032 as authored */
static int32_t wt_min_z = 0, wt_max_z = 0;   /*  2880 .. 2980             */
static int32_t wt_min_y = 0, wt_max_y = 0;   /* -1100 ..  -500            */

static int wt_tex = -1;

/* The player's head, relative to cam_y - the figure apply_collision_* uses
   for its own body span. */
#define WT_PLAYER_HEAD 30

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

/* The view, loaded without the SDK's gte_SetRotMatrix/TransMatrix, which
   clobber $t0/$t1 undeclared (sharur_tablet.c's sh_set_view, verbatim). */
static inline void wt_set_view(const MATRIX *m) {
    uint32_t r0 = (uint16_t)m->m[0][0] | ((uint32_t)(uint16_t)m->m[0][1] << 16);
    uint32_t r1 = (uint16_t)m->m[0][2] | ((uint32_t)(uint16_t)m->m[1][0] << 16);
    uint32_t r2 = (uint16_t)m->m[1][1] | ((uint32_t)(uint16_t)m->m[1][2] << 16);
    uint32_t r3 = (uint16_t)m->m[2][0] | ((uint32_t)(uint16_t)m->m[2][1] << 16);
    uint32_t r4 = (uint16_t)m->m[2][2];
    __asm__ volatile (
        "ctc2 %0, $0;"
        "ctc2 %1, $1;"
        "ctc2 %2, $2;"
        "ctc2 %3, $3;"
        "ctc2 %4, $4"
        : : "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(r4));
    __asm__ volatile (
        "ctc2 %0, $5;"
        "ctc2 %1, $6;"
        "ctc2 %2, $7"
        : : "r"(m->t[0]), "r"(m->t[1]), "r"(m->t[2]));
}

/* Startup. A 2.6 KB mesh held for the run, and a texture registration that
   reads nothing but a TIM header until the Underworld's bank is selected. */
void wings_tablet_load_assets(void) {
    /* BANK: Chapter 4. Derived, not guessed - py tools/check_tex_banks.py
       walks the uploader call graph (wingscape_upload_textures reaches this
       module) and fails if UNDERWORLD is not in the mask. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);

    wt_buf = read_file("\\TEXUNDW\\WINGTBLT.SMD;1");
    if (wt_buf) wt_smd = smdInitData(wt_buf);

    /* MEASURE THE MESH. No re-centre: the export's own coordinates are where
       it stands. */
    if (wt_smd && wt_smd->n_verts > 0) {
        int i;
        wt_min_x = wt_max_x = wt_smd->p_verts[0].vx;
        wt_min_y = wt_max_y = wt_smd->p_verts[0].vy;
        wt_min_z = wt_max_z = wt_smd->p_verts[0].vz;
        for (i = 1; i < wt_smd->n_verts; i++) {
            int32_t vx = wt_smd->p_verts[i].vx;
            int32_t vy = wt_smd->p_verts[i].vy;
            int32_t vz = wt_smd->p_verts[i].vz;
            if (vx < wt_min_x) wt_min_x = vx;
            if (vx > wt_max_x) wt_max_x = vx;
            if (vy < wt_min_y) wt_min_y = vy;
            if (vy > wt_max_y) wt_max_y = vy;
            if (vz < wt_min_z) wt_min_z = vz;
            if (vz > wt_max_z) wt_max_z = vz;
        }
    }

    wt_tex = texmgr_register("\\TEXUNDW\\WINGTBLT.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. Called from wingscape_upload_textures(). */
void wings_tablet_upload_texture(void) {
    if (wt_tex >= 0) texmgr_upload(wt_tex);
}

/* Player push-out against the measured box, Minkowski-expanded by the caller's
   radius and resolved along the shallowest axis - the Gula Tablet's scheme. */
void wings_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    if (current_area != STATE_WINGSCAPE || !wt_smd) return;

    int32_t feet = py + GROUND_FLOOR_Y, head = py - WT_PLAYER_HEAD;
    if (head >= wt_max_y || feet <= wt_min_y) return;

    int32_t min_x = wt_min_x - radius, max_x = wt_max_x + radius;
    int32_t min_z = wt_min_z - radius, max_z = wt_max_z + radius;
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

/* The Gula Tablet's primitive loop with no model matrix: the vertices are
   already world space, so the plain view transform is the whole of it. */
void wings_tablet_draw(RenderContext *ctx) {
    if (current_area != STATE_WINGSCAPE || !wt_smd) return;

    /* Culled at fog-far from the box's CENTRE, less its half-length so an end
       still in view is not lost with the middle (Sharur's Tablet's rule). */
    int32_t gcx = (wt_min_x + wt_max_x) / 2, gcz = (wt_min_z + wt_max_z) / 2;
    int32_t dcx = gcx - cam_x, dcz = gcz - cam_z;
    int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
    if (dist - (wt_max_x - wt_min_x) / 2 > g_fog_far) return;

    MATRIX view;
    camera_build_view(&view);
    wt_set_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(wt_tex);
    uint16_t cl = texmgr_clut(wt_tex);
    int32_t  fog_factor = render_fog_scale(dist);

    uint8_t *p = (uint8_t *)wt_smd->p_prims;
    int pi;
    for (pi = 0; pi < wt_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
        uint8_t       stride   = pt->len;
        int           is_quad  = (pt->type >= 2);
        int           textured = pt->texture;

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &wt_smd->p_verts[vi[0]];
        SVECTOR *v1 = &wt_smd->p_verts[vi[1]];
        SVECTOR *v2 = &wt_smd->p_verts[vi[2]];

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
            SVECTOR *v3 = &wt_smd->p_verts[vi[3]];
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
        /* TRUE DEPTH, no +40: the room's terrace sorts at its far corner plus
           40, so the slab standing on it wins the tie. In front of the sky. */
        if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
        if (otz >= UW_SKY_OT)   otz = UW_SKY_OT - 1;

        uint8_t *col = p + 16;
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + UW_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + UW_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + UW_FOG_B * (256 - fog_factor)) >> 8);

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
