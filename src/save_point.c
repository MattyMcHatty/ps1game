#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "door.h"           /* door_draw_string_billboard (shared pixel font) */
#include "save_point.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

SavePoint save_points[MAX_SAVE_POINTS];
int       save_point_count = 0;

static SMD  *sp_smd    = NULL;
static void *sp_buffer = NULL;

/* Model-space XZ half-extents, captured from the mesh at load; the per-instance
   footprint is these scaled by the instance's `scale` and rotated by rot_y. */
static int32_t sp_half_w = 0, sp_half_d = 0;

/* "Save" prompt tuning. */
#define SAVE_TEXT_RADIUS  1100   /* show the label within this XZ distance      */
#define SAVE_TEXT_FADE     700   /* start fading out past this distance         */
#define SAVE_TEXT_PIXEL      5   /* world units per font pixel                  */
#define SAVE_TEXT_RISE     140   /* label height above the model's floor base   */
#define SAVE_SPIN_SPEED      8   /* rot_y units/frame (4096 = full turn ~8.5s)  */

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

    /* Capture the mesh's XZ half-extents for collision (assumes the model is
       centred on its origin, as the other props are). */
    if (sp_smd) {
        int i;
        for (i = 0; i < sp_smd->n_verts; i++) {
            int32_t ax = sp_smd->p_verts[i].vx; if (ax < 0) ax = -ax;
            int32_t az = sp_smd->p_verts[i].vz; if (az < 0) az = -az;
            if (ax > sp_half_w) sp_half_w = ax;
            if (az > sp_half_d) sp_half_d = az;
        }
    }
}

/* Player collision against each save point: an axis-aligned box the size of the
   mesh footprint (scaled + rotated per instance), pushed out like the other
   props. Single-level placement, so no Y gate. */
void save_points_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;
    int i;
    for (i = 0; i < save_point_count; i++) {
        SavePoint *s = &save_points[i];
        if (!s->active) continue;

        int32_t hw = (sp_half_w * s->scale) >> 12;
        int32_t hd = (sp_half_d * s->scale) >> 12;
        /* Axis-aligned bound of the rotated footprint (same maths as the dresser). */
        int32_t c = icos(s->rot_y), sn = isin(s->rot_y);
        if (c  < 0) c  = -c;
        if (sn < 0) sn = -sn;
        int32_t bw = (hw * c + hd * sn) >> 12;
        int32_t bd = (hw * sn + hd * c) >> 12;

        int32_t min_x = s->x - bw - radius, max_x = s->x + bw + radius;
        int32_t min_z = s->z - bd - radius, max_z = s->z + bd + radius;
        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        int32_t pl = *px - min_x, pr = max_x - *px;
        int32_t pf = *pz - min_z, pb = max_z - *pz;
        int32_t m = pl, ddx = -pl, ddz = 0;
        if (pr < m) { m = pr; ddx =  pr; ddz = 0; }
        if (pf < m) { m = pf; ddx = 0; ddz = -pf; }
        if (pb < m) {         ddx = 0; ddz =  pb; }
        *px += ddx; *pz += ddz;
    }
}

void save_points_clear(void) {
    save_point_count = 0;
}

/* Spin every save point slowly about its Y axis, forever. */
void save_points_update(void) {
    int i;
    for (i = 0; i < save_point_count; i++)
        if (save_points[i].active)
            save_points[i].rot_y = (save_points[i].rot_y + SAVE_SPIN_SPEED) & 4095;
}

/* ---- Circle-to-save interaction -------------------------------------------
   Same edge-detected Circle pattern as the room doors: a fresh press (not a held
   one carried in from a transition) while within SAVE_TRIGGER_RADIUS of any
   active save point opens the save flow. */
#define SAVE_TRIGGER_RADIUS 500

static int save_circle_prev = 1;   /* start "held" so an entry press won't fire */

void save_point_arm(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    save_circle_prev = held;
}

int save_point_triggered(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int just = held && !save_circle_prev;
    save_circle_prev = held;
    if (!just) return 0;

    int i;
    for (i = 0; i < save_point_count; i++) {
        SavePoint *s = &save_points[i];
        if (!s->active) continue;
        int32_t dx = cam_x - s->x, dz = cam_z - s->z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz < SAVE_TRIGGER_RADIUS) return 1;
    }
    return 0;
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
            int32_t sz[4], otz;

            gte_ldv3(v0, v1, v2);
            gte_rtpt();
            gte_stsxy3c(sv);

            if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
                sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
                sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
                p += stride; continue;
            }

            /* No backface cull: the save-point model is built entirely from
               triangle-shaped (degenerate) quads whose first-triangle winding is
               unreliable. It's a tiny model, so just draw every face rather than
               risk the flicker/disappear glitch. */

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

    /* Floating purple "Save" label above each nearby save point (billboarded so
       it faces the player), fading out with distance like the door prompts. */
    for (i = 0; i < save_point_count; i++) {
        SavePoint *s = &save_points[i];
        if (!s->active) continue;

        int32_t dx = cam_x - s->x, dz = cam_z - s->z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (dist >= SAVE_TEXT_RADIUS) continue;

        int fade = 256;
        if (dist > SAVE_TEXT_FADE) {
            int range = SAVE_TEXT_RADIUS - SAVE_TEXT_FADE;
            fade = 256 - (((dist - SAVE_TEXT_FADE) * 256) / range);
        }

        int32_t base_y = s->y + GROUND_FLOOR_Y;   /* model's floor level, world Y */
        door_draw_string_billboard(ctx, "Save",
                                   s->x, base_y - SAVE_TEXT_RISE, s->z,
                                   170, 40, 230, fade, SAVE_TEXT_PIXEL);
    }
}
