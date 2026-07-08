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
#include "dresser.h"
#include "dresser_tex_map.h"

Dresser dressers[MAX_DRESSERS];
int     dresser_count = 0;

static SMD  *dresser_smd    = NULL;
static void *dresser_buffer = NULL;

/* The dresser's OWN texture, streamed into a spare VRAM slot on room entry. Its
   TIM bytes + parsed header stay resident (preloaded at startup) so the entry
   upload is a pure LoadImage with NO mid-game CD read — same scheme reception
   uses for its unique textures. */
static uint8_t  *dresser_tim_buf = NULL;
static TIM_IMAGE dresser_tim;
static uint16_t  dresser_tpage = 0;
static uint16_t  dresser_clut  = 0;

static void *read_file(const char *name) {
    CdlFILE file;
    if (!CdSearchFile(&file, name)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

/* Startup: load geometry and preload the dresser texture into resident RAM,
   capturing its tpage/clut. All CD access happens here, where it is safe (never
   mid-game). The texture is uploaded to VRAM separately (dresser_upload_texture)
   on the room transition. */
void dresser_load_assets(void) {
    dresser_buffer = read_file("\\DRESSER.SMD;1");
    if (dresser_buffer)
        dresser_smd = smdInitData(dresser_buffer);

    dresser_tim_buf = read_file("\\TEX\\DRESSER.TIM;1");
    if (dresser_tim_buf) {
        GetTimInfo((uint32_t *)dresser_tim_buf, &dresser_tim);
        if (dresser_tim.mode & 0x8)
            dresser_clut = getClut(dresser_tim.crect->x, dresser_tim.crect->y);
        dresser_tpage = getTPage(dresser_tim.mode & 0x3, 0,
                                 dresser_tim.prect->x, dresser_tim.prect->y);
    }
}

/* Room entry: upload the dresser texture from its resident RAM copy. Pure
   LoadImage, no CD access — safe during the room transition (the caller idles
   the GPU with DrawSync first), exactly like reception_upload_textures. */
void dresser_upload_texture(void) {
    if (!dresser_tim_buf) return;
    LoadImage(dresser_tim.prect, dresser_tim.paddr);
    DrawSync(0);
    if (dresser_tim.mode & 0x8) {
        LoadImage(dresser_tim.crect, dresser_tim.caddr);
        DrawSync(0);
    }
}

void dressers_clear(void) {
    dresser_count = 0;
}

int dresser_add(int32_t x, int32_t y, int32_t z, int32_t rot_y) {
    if (dresser_count >= MAX_DRESSERS) return -1;
    int i = dresser_count++;
    dressers[i].x      = x;
    dressers[i].y      = y;
    dressers[i].z      = z;
    dressers[i].rot_y  = rot_y;
    dressers[i].active = 1;
    dressers[i].half_w = 195;   /* model footprint: X +/-195, Z +/-50 */
    dressers[i].half_d = 50;
    return i;
}

void dressers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;   /* placed on a single floor level per instance — no vertical gate */
    int i;
    for (i = 0; i < dresser_count; i++) {
        Dresser *d = &dressers[i];
        if (!d->active) continue;

        /* The dresser is elongated (390x100), so account for its Y rotation:
           the axis-aligned bound of the rotated footprint is
             (|hw*cos| + |hd*sin|, |hw*sin| + |hd*cos|).
           isin/icos are fixed-point (4096 = 1.0). */
        int32_t c = icos(d->rot_y), s = isin(d->rot_y);
        if (c < 0) c = -c;
        if (s < 0) s = -s;
        int32_t hw = (d->half_w * c + d->half_d * s) >> 12;
        int32_t hd = (d->half_w * s + d->half_d * c) >> 12;

        /* Minkowski-expanded AABB plus a push margin (same scheme as the dining
           table) so the camera stops clear of the visible edges. */
        int32_t min_x = d->x - hw - radius - DRESSER_PUSH_MARGIN;
        int32_t max_x = d->x + hw + radius + DRESSER_PUSH_MARGIN;
        int32_t min_z = d->z - hd - radius - DRESSER_PUSH_MARGIN;
        int32_t max_z = d->z + hd + radius + DRESSER_PUSH_MARGIN;

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

/* Render all active dressers using the same textured-prim path as the room
   geometry (per-poly UVs from the SMD, the 128 texture window the room sets, and
   matching distance fog). Each face is drawn with either the room's wd_flr
   texture (tex slot 0, passed in) or the dresser's own texture (slot 1), per the
   dresser_tex_map. Restores the caller's view matrix before returning. */
void dressers_draw(RenderContext *ctx, uint16_t wdflr_tpage, uint16_t wdflr_clut) {
    if (!dresser_smd) return;

    /* Camera view matrix (same construction as the other prop renderers). */
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
    for (i = 0; i < dresser_count; i++) {
        Dresser *d = &dressers[i];
        if (!d->active) continue;

        int32_t dcx = d->x - cam_x, dcz = d->z - cam_z;
        if ((dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz) > 2300) continue;

        /* Combine the view matrix with this dresser's world transform. */
        MATRIX dm, combined;
        SVECTOR dr = {0, d->rot_y, 0, 0};
        RotMatrix(&dr, &dm);
        VECTOR pos = {d->x, d->y + GROUND_FLOOR_Y, d->z};
        TransMatrix(&dm, &pos);
        CompMatrixLV(&view, &dm, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        uint8_t *p = (uint8_t *)dresser_smd->p_prims;
        int pi;
        for (pi = 0; pi < dresser_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
            uint8_t       stride = pt->len;
            int           is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &dresser_smd->p_verts[vi[0]];
            SVECTOR *v1 = &dresser_smd->p_verts[vi[1]];
            SVECTOR *v2 = &dresser_smd->p_verts[vi[2]];

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
                SVECTOR *v3 = &dresser_smd->p_verts[vi[3]];
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
            /* Stay below the room's texture-window primitive at OT_LENGTH-1 so it
               is processed first (same rule as the room geometry). */
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            /* Distance fog from the dresser centre — consistent with the room. */
            int32_t fog_start = 350, fog_end = 2200;
            int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
            int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
            int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

            /* Pick the texture for this face: slot 0 = room wd_flr, slot 1 = the
               dresser's own texture. Both sit at page-top (V 0-127), so the
               room's single 128 texture window serves them. */
            uint8_t slot = (pi < DRESSER_PRIM_COUNT) ? dresser_tex_map[pi] : 0;
            uint16_t tp = slot ? dresser_tpage : wdflr_tpage;
            uint16_t cl = slot ? dresser_clut  : wdflr_clut;

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
