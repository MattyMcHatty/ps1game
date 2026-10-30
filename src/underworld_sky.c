#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "tim_slots.h"
#include "cdaudio.h"
#include "descent_to_hell.h"
#include "underworld_sky.h"

/* The Underworld skybox - see underworld_sky.h. */

static void *sky_buff = NULL;
static SMD  *sky_smd  = NULL;

/* ---- The texture -----------------------------------------------------------
   ONE, and not ours: hell bg, the Descent's backdrop (4bpp x640 y0, CLUT
   576,502), uploaded through its narrow uploader rather than registered twice.
   It sits at Voff 0 of a page start, so the sky draws under the room's 128
   window like everything else - the ring's v runs 0..128 and wraps cleanly. */
static uint16_t sky_tpage;
static uint16_t sky_clut;

void underworld_sky_load_assets(void) {
    sky_tpage = TIM_TPAGE_HELLBG;
    sky_clut  = TIM_CLUT_HELLBG;
}

/* Pure LoadImage from the RAM copy texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void underworld_sky_upload_textures(void) {
    descent_to_hell_upload_hell_bg();
}

/* ---- The model -------------------------------------------------------------
   Read on entry to a sky room, in STATE_LOADING. A room-to-room move inside the
   chapter keeps it (the read is skipped), and main.c's load_area_geometry hands
   it back on the way into any room that does not draw it. */
void underworld_sky_unload(void) {
    if (sky_buff) free(sky_buff);
    sky_buff = NULL;
    sky_smd  = NULL;
}

void underworld_sky_load(void) {
    if (sky_smd) return;
    CdlFILE file;
    /* room_arena_load resumes CD-DA on its way out, so this read brackets
       itself (descent_to_hell_load_geometry's tube, the same note). */
    cdaudio_suspend();
    if (CdSearchFile(&file, "\\TEXUNDW\\UWSKY.SMD;1")) {
        int sectors = (file.size + 2047) / 2048;
        sky_buff = malloc(sectors * 2048);
        if (sky_buff) {
            CdControl(CdlSetloc, &file.pos, NULL);
            CdRead(sectors, (uint32_t *)sky_buff, CdlModeSpeed);
            CdReadSync(0, NULL);
            sky_smd = smdInitData(sky_buff);
        }
    }
    cdaudio_resume();
}

/* ---- The draw --------------------------------------------------------------
   Each face is a bilinear grid of UW_SKY_SUB x UW_SKY_SUB cells over its four
   corners, in GPU quad order (v0-v1 one edge, v2-v3 the opposite one). Every
   grid corner is projected once; a cell is queued only if all four corners are
   in front of the camera and inside the GTE's +-1023, and its on-screen size
   is inside the GPU's 1023 x 511 primitive limit.

   4 x 4 cells is ~1950 x 3000 world units a cell on the ring's sixteen faces
   (each ~7800 wide, 12000 tall, radius 20000). The nearest the ring ever is to
   a camera inside a room is ~16500, so a visible cell is ~50 px at most, and a
   cell with a corner behind the camera is ~80 degrees off the view axis - far
   outside the screen, so dropping it loses nothing. A face whose four corners
   are ALL behind the camera is rejected before its grid is built, so a frame
   cuts about half the ring.

   NO FOG AND NO DISTANCE CULL, by the chapter's rule (src/underworld.h): the
   vertex colour goes to the GPU untouched. */
#define UW_SKY_SHIFT   2
#define UW_SKY_SUB     (1 << UW_SKY_SHIFT)
#define UW_SKY_ROW     (UW_SKY_SUB + 1)
#define UW_SKY_NEAR    32

static DVECTOR g_sv[UW_SKY_ROW * UW_SKY_ROW];
static int32_t g_sz[UW_SKY_ROW * UW_SKY_ROW];
static uint8_t g_u [UW_SKY_ROW * UW_SKY_ROW];
static uint8_t g_v [UW_SKY_ROW * UW_SKY_ROW];
static uint8_t g_ok[UW_SKY_ROW * UW_SKY_ROW];

static int on_screen(const DVECTOR *v) {
    return v->vx > -1023 && v->vx < 1023 && v->vy > -1023 && v->vy < 1023;
}

static void draw_face(RenderContext *ctx, uint8_t *p, uint8_t *buf_end,
                      int32_t cx, int32_t cy, int32_t cz) {
    uint16_t *vi  = (uint16_t *)(p + 4);
    uint8_t  *col = p + 16;
    uint8_t  *uv  = p + 20;
    int32_t qx[4], qy[4], qz[4], qu[4], qv[4];
    int k, s, t, behind = 0;
    int left = 0, right = 0, above = 0, below = 0;

    for (k = 0; k < 4; k++) {
        SVECTOR *v = &sky_smd->p_verts[vi[k]];
        qx[k] = v->vx + cx;
        qy[k] = v->vy + cy;
        qz[k] = v->vz + cz;
        qu[k] = uv[k * 2];
        qv[k] = uv[k * 2 + 1];

        SVECTOR w = { (int16_t)qx[k], (int16_t)qy[k], (int16_t)qz[k], 0 };
        DVECTOR sv;
        int32_t sz;
        gte_ldv0(&w);
        gte_rtps();
        gte_stsxy(&sv);
        gte_stsz(&sz);
        if (sz <= UW_SKY_NEAR) behind++;
        if (sv.vx < 0)            left++;
        if (sv.vx >= SCREEN_XRES) right++;
        if (sv.vy < 0)            above++;
        if (sv.vy >= SCREEN_YRES) below++;
    }
    if (behind == 4) return;   /* the whole face is behind the camera */

    /* >>> A FACE WHOLLY OFF ONE SIDE OF THE SCREEN IS REJECTED HERE TOO. <<<
       Only when all four corners are IN FRONT: then every grid point, being a
       blend of the corners, lies in the convex region "in front of the camera
       and beyond that screen edge", so every cell the grid would build is off
       the screen and the cell loop below would discard it after projecting 25
       points to find that out. The GTE clamps SX/SY to +-1024, which keeps a
       point on the same side of 0 and of the screen size, so the counts are
       exact. A face with a corner behind the camera is left to the cell loop:
       its corners' screen positions prove nothing. Measured (Oct 2026, Hell
       Hath No Fury, force-booted): 53-55 hblanks a frame -> 36, in every
       direction, and in every room that draws the ring. */
    if (behind == 0 &&
        (left == 4 || right == 4 || above == 4 || below == 4)) return;

    /* Project the grid, a row at a time down the v0->v2 / v1->v3 edges. */
    for (t = 0; t <= UW_SKY_SUB; t++) {
        int32_t lx = qx[0] + (((qx[2] - qx[0]) * t) >> UW_SKY_SHIFT);
        int32_t ly = qy[0] + (((qy[2] - qy[0]) * t) >> UW_SKY_SHIFT);
        int32_t lz = qz[0] + (((qz[2] - qz[0]) * t) >> UW_SKY_SHIFT);
        int32_t lu = qu[0] + (((qu[2] - qu[0]) * t) >> UW_SKY_SHIFT);
        int32_t lv = qv[0] + (((qv[2] - qv[0]) * t) >> UW_SKY_SHIFT);
        int32_t rx = qx[1] + (((qx[3] - qx[1]) * t) >> UW_SKY_SHIFT);
        int32_t ry = qy[1] + (((qy[3] - qy[1]) * t) >> UW_SKY_SHIFT);
        int32_t rz = qz[1] + (((qz[3] - qz[1]) * t) >> UW_SKY_SHIFT);
        int32_t ru = qu[1] + (((qu[3] - qu[1]) * t) >> UW_SKY_SHIFT);
        int32_t rv = qv[1] + (((qv[3] - qv[1]) * t) >> UW_SKY_SHIFT);
        int g = t * UW_SKY_ROW;
        for (s = 0; s <= UW_SKY_SUB; s++, g++) {
            SVECTOR w;
            w.vx  = (int16_t)(lx + (((rx - lx) * s) >> UW_SKY_SHIFT));
            w.vy  = (int16_t)(ly + (((ry - ly) * s) >> UW_SKY_SHIFT));
            w.vz  = (int16_t)(lz + (((rz - lz) * s) >> UW_SKY_SHIFT));
            w.pad = 0;
            g_u[g] = (uint8_t)(lu + (((ru - lu) * s) >> UW_SKY_SHIFT));
            g_v[g] = (uint8_t)(lv + (((rv - lv) * s) >> UW_SKY_SHIFT));

            gte_ldv0(&w);
            gte_rtps();
            gte_stsxy(&g_sv[g]);
            gte_stsz(&g_sz[g]);
            g_ok[g] = (g_sz[g] > UW_SKY_NEAR) && on_screen(&g_sv[g]);
        }
    }

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    for (t = 0; t < UW_SKY_SUB; t++) {
        for (s = 0; s < UW_SKY_SUB; s++) {
            int a = t * UW_SKY_ROW + s, b = a + 1, c = a + UW_SKY_ROW, d = c + 1;
            if (!g_ok[a] || !g_ok[b] || !g_ok[c] || !g_ok[d]) continue;

            int32_t x0 = g_sv[a].vx, x1 = x0, y0 = g_sv[a].vy, y1 = y0;
            const DVECTOR *e[3] = { &g_sv[b], &g_sv[c], &g_sv[d] };
            for (k = 0; k < 3; k++) {
                if (e[k]->vx < x0) x0 = e[k]->vx;
                if (e[k]->vx > x1) x1 = e[k]->vx;
                if (e[k]->vy < y0) y0 = e[k]->vy;
                if (e[k]->vy > y1) y1 = e[k]->vy;
            }
            if (x1 - x0 > 1023 || y1 - y0 > 511) continue;
            if (x1 < 0 || x0 >= SCREEN_XRES || y1 < 0 || y0 >= SCREEN_YRES) continue;

            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, col[0], col[1], col[2]);
            poly->tpage = sky_tpage;
            poly->clut  = sky_clut;
            poly->u0 = g_u[a]; poly->v0 = g_v[a];
            poly->u1 = g_u[b]; poly->v1 = g_v[b];
            poly->u2 = g_u[c]; poly->v2 = g_v[c];
            poly->u3 = g_u[d]; poly->v3 = g_v[d];
            poly->x0 = g_sv[a].vx; poly->y0 = g_sv[a].vy;
            poly->x1 = g_sv[b].vx; poly->y1 = g_sv[b].vy;
            poly->x2 = g_sv[c].vx; poly->y2 = g_sv[c].vy;
            poly->x3 = g_sv[d].vx; poly->y3 = g_sv[d].vy;
            addPrim(&ot[UW_SKY_OT], poly);
            ctx->next_packet += sizeof(POLY_FT4);
        }
    }
}

void underworld_sky_draw(RenderContext *ctx, int32_t cx, int32_t cy, int32_t cz) {
    if (!sky_smd) return;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    uint8_t *p = (uint8_t *)sky_smd->p_prims;
    int i, n = sky_smd->n_prims;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        if (pt->type >= 2)
            draw_face(ctx, p, buf_end, cx, cy, cz);
        p += pt->len;
    }
}
