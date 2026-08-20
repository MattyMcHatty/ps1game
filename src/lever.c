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
#include "title.h"          /* current_area gate */
#include "lever.h"

#define MAX_LEVERS         8
#define LEVER_PUSH_MARGIN 30   /* extra gap between player and prop edge (as tables) */

/* Local footprint of "Lever.smx", straight off its SMX bbox: a 15x15 shaft 150
   long down the Z axis, origin at the centroid (NOT at the base, as the
   concrete/chainlink props are). */
#define LEVER_HALF_X       8   /* 7.5, rounded out */
#define LEVER_HALF_Y       8
#define LEVER_MIN_Z_OFF (-75)
#define LEVER_MAX_Z_OFF   75

/* The blue cap's centre in model space — the point a thrown lever pivots on. */
#define LEVER_PIVOT_Z    LEVER_MAX_Z_OFF

typedef struct {
    GameState area;                          /* only collides/draws in this room */
    int32_t   x, y, z, rot_y;                /* centroid; world y = y+GROUND_FLOOR_Y */
    int32_t   pitch;                         /* throw about local X at the blue cap */
    int32_t   min_x, max_x, min_z, max_z;    /* world AABB, baked at place time  */
    int       active;
} Lever;

static Lever levers[MAX_LEVERS];
static int   lever_count = 0;

static SMD  *lever_smd = NULL;
static void *lever_buf = NULL;

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

/* Startup: geometry only. Every face is flat-shaded, so there is no texture to
   register, upload or restore — and nothing for this prop to stomp in VRAM. */
void levers_load_assets(void) {
    lever_buf = read_file("\\TEX\\LEVER.SMD;1");
    if (lever_buf) lever_smd = smdInitData(lever_buf);
}

void levers_clear(void) { lever_count = 0; }

void lever_place(GameState area, int32_t x, int32_t y, int32_t z, int32_t rot_y) {
    if (lever_count >= MAX_LEVERS) return;
    Lever *l = &levers[lever_count++];
    l->area  = area;
    l->x = x;  l->y = y;  l->z = z;
    l->rot_y = rot_y;
    l->pitch = 0;
    l->active = 1;

    /* World AABB = axis-aligned bound of the rotated local rect, corner by
       corner (same as the chainlink door). Exact at any angle, computed once. */
    int32_t c = icos(rot_y), s = isin(rot_y);
    const int32_t lx[4] = { -LEVER_HALF_X,  LEVER_HALF_X,
                             LEVER_HALF_X, -LEVER_HALF_X };
    const int32_t lz[4] = { LEVER_MIN_Z_OFF, LEVER_MIN_Z_OFF,
                            LEVER_MAX_Z_OFF, LEVER_MAX_Z_OFF };
    int k;
    for (k = 0; k < 4; k++) {
        /* Same handedness as RotMatrix's Y rotation, which the draw uses. */
        int32_t wx = x + ((lx[k] * c + lz[k] * s) >> 12);
        int32_t wz = z + ((lz[k] * c - lx[k] * s) >> 12);
        if (k == 0) {
            l->min_x = l->max_x = wx;
            l->min_z = l->max_z = wz;
        } else {
            if (wx < l->min_x) l->min_x = wx;
            if (wx > l->max_x) l->max_x = wx;
            if (wz < l->min_z) l->min_z = wz;
            if (wz > l->max_z) l->max_z = wz;
        }
    }
}

void lever_set_pitch(int index, int32_t pitch) {
    if (index < 0 || index >= lever_count) return;
    levers[index].pitch = pitch;
}

/* Player push-out (dresser-style Minkowski AABB). Area-gated so the shared
   reception collision routine can call it unconditionally.

   In the Attic Exit this never actually fires: the levers are recessed into the
   walls, and the 195 wall standoff already holds the player farther out than
   any part of the shaft. It is here so a lever mounted somewhere reachable —
   on a free-standing fixture, say — behaves like the other props without
   needing new code. */
void levers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < lever_count; i++) {
        Lever *l = &levers[i];
        if (!l->active || l->area != current_area) continue;

        /* Vertical gate: the shaft is a small fixture up at chest height, not a
           floor-to-ceiling body, so a player whose span misses it walks under or
           over it rather than being pushed. */
        int32_t centre = l->y + GROUND_FLOOR_Y;
        int32_t body_bot = py + GROUND_FLOOR_Y, body_top = py - 30;
        if (body_top > centre + LEVER_HALF_Y || centre - LEVER_HALF_Y > body_bot)
            continue;

        int32_t min_x = l->min_x - radius - LEVER_PUSH_MARGIN;
        int32_t max_x = l->max_x + radius + LEVER_PUSH_MARGIN;
        int32_t min_z = l->min_z - radius - LEVER_PUSH_MARGIN;
        int32_t max_z = l->max_z + radius + LEVER_PUSH_MARGIN;

        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        /* Push out along the axis with the smallest penetration. */
        int32_t push_l = *px - min_x;
        int32_t push_r = max_x - *px;
        int32_t push_f = *pz - min_z;
        int32_t push_b = max_z - *pz;

        int32_t min_push = push_l, dx = -push_l, dz = 0;
        if (push_r < min_push) { min_push = push_r; dx =  push_r; dz = 0; }
        if (push_f < min_push) { min_push = push_f; dx = 0; dz = -push_f; }
        if (push_b < min_push) {                    dx = 0; dz =  push_b; }

        *px += dx;
        *pz += dz;
    }
}

/* Hitscan volume: the real shaft, no player standoff, height-aware. */
int levers_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack) {
    int i;
    for (i = 0; i < lever_count; i++) {
        Lever *l = &levers[i];
        if (!l->active || l->area != current_area) continue;
        int32_t centre = l->y + GROUND_FLOOR_Y;
        if (y < centre - LEVER_HALF_Y - slack || y > centre + LEVER_HALF_Y + slack) continue;
        if (x < l->min_x - slack || x > l->max_x + slack) continue;
        if (z < l->min_z - slack || z > l->max_z + slack) continue;
        return 1;
    }
    return 0;
}

/* See the header. */
int levers_any_solid(void) {
    int i;
    for (i = 0; i < lever_count; i++)
        if (levers[i].active && levers[i].area == current_area) return 1;
    return 0;
}

/* Render every instance in the current area, with the same fog and cull budget
   as the interior rooms. Every face is flat-shaded (POLY_F3/F4) — the model
   carries no UVs, and its per-face colours are what mark the blue mounting cap
   and red tip. Restores the caller's view matrix before returning. */
void levers_draw(RenderContext *ctx) {
    if (lever_count == 0 || !lever_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < lever_count; i++) {
        Lever *lv = &levers[i];
        if (!lv->active || lv->area != current_area) continue;

        int32_t dcx = lv->x - cam_x, dcz = lv->z - cam_z;
        if ((dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz) > 1500) continue;

        MATRIX pm, combined;
        SVECTOR prot = {0, lv->rot_y, 0, 0};
        RotMatrix(&prot, &pm);
        VECTOR pos = {lv->x, lv->y + GROUND_FLOOR_Y, lv->z};
        TransMatrix(&pm, &pos);

        /* A thrown lever gets an extra rotation about its LOCAL X, applied
           BEFORE the yaw so "down" means the same on both walls. Rotating about
           a pivot P rather than the origin is R with the translation
           P - R*P baked in; here P = (0,0,LEVER_PIVOT_Z), so
           R*P = (0, -P.z*sin, P.z*cos) and the offset falls out to
           (0, P.z*sin, P.z*(1-cos)). */
        if (lv->pitch) {
            MATRIX px, model;
            SVECTOR xrot = {(int16_t)lv->pitch, 0, 0, 0};
            RotMatrix(&xrot, &px);
            VECTOR pv;
            pv.vx = 0;
            pv.vy = (LEVER_PIVOT_Z * isin(lv->pitch)) >> 12;
            pv.vz = LEVER_PIVOT_Z - ((LEVER_PIVOT_Z * icos(lv->pitch)) >> 12);
            TransMatrix(&px, &pv);
            CompMatrixLV(&pm, &px, &model);       /* pitch first, then the yaw  */
            CompMatrixLV(&view, &model, &combined);
        } else {
            CompMatrixLV(&view, &pm, &combined);
        }

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        uint8_t *p = (uint8_t *)lever_smd->p_prims;
        int pi;
        for (pi = 0; pi < lever_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt      = (SMD_PRI_TYPE *)p;
            uint8_t       stride  = pt->len;
            int           is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &lever_smd->p_verts[vi[0]];
            SVECTOR *v1 = &lever_smd->p_verts[vi[1]];
            SVECTOR *v2 = &lever_smd->p_verts[vi[2]];

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
                v3 = &lever_smd->p_verts[vi[3]];
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
            /* Horizontal polys sort by their farthest corner (see render.h).
               That test reads MODEL-space vy and is only valid while the prop
               rotates about Y alone — a pitched lever's flat faces are no longer
               flat in the world, so it is skipped for the throw. */
            if (!lv->pitch && poly_is_flat_y(v0, v1, v2, v3))
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

    /* Restore the camera view matrix for whatever the caller draws next. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
