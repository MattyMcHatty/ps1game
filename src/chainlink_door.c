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
#include "chainlink_door.h"

#define MAX_CHAINLINK_DOORS  4
#define CLDOOR_PUSH_MARGIN  30   /* extra gap between player and prop edge (as tables) */

/* Local footprint of "Chainlink Door.smx", straight off its SMX bbox. The origin
   is centred in X but sits on the FRONT face in Z, so these are signed offsets
   from the origin rather than half-extents. */
#define CLDOOR_MIN_X_OFF  (-300)
#define CLDOOR_MAX_X_OFF    300
#define CLDOOR_MIN_Z_OFF      0
#define CLDOOR_MAX_Z_OFF     31
#define CLDOOR_SOLID_H      507   /* model spans y[-507,0], base at y=0 */

typedef struct {
    GameState area;                          /* only collides/draws in this room */
    int32_t   x, y, z, rot_y;                /* y = standing reference; world = y+GROUND_FLOOR_Y */
    int32_t   min_x, max_x, min_z, max_z;    /* world AABB, baked at place time  */
    int       active;
} ChainlinkDoor;

static ChainlinkDoor doors[MAX_CHAINLINK_DOORS];
static int           door_count = 0;

static SMD  *cldoor_smd = NULL;
static void *cldoor_buf = NULL;
static uint16_t cldoor_tpage = 0, cldoor_clut = 0;

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

/* Startup: load the model, and take chnlnk's tpage/clut from its TIM HEADER —
   no texmgr_register and no LoadImage. The East Stairwell already owns the RAM
   copy of chnlnk and every room that shows this prop uploads it on entry
   (attic_exit_upload_textures -> east_stairwell_upload_chnlnk). A second
   registration would buy nothing and risks pushing another module past
   TEXMGR_MAX, which fails SILENTLY and hands back tpage/clut 0. */
void chainlink_doors_load_assets(void) {
    cldoor_buf = read_file("\\TEX\\CHNLNKDR.SMD;1");
    if (cldoor_buf) cldoor_smd = smdInitData(cldoor_buf);

    void *tim = read_file("\\TEX\\CHNLNK.TIM;1");
    if (tim) {
        TIM_IMAGE info;
        GetTimInfo((uint32_t *)tim, &info);
        if (info.mode & 0x8) cldoor_clut = getClut(info.crect->x, info.crect->y);
        cldoor_tpage = getTPage(info.mode & 0x3, 0, info.prect->x, info.prect->y);
        free(tim);
    }
}

void chainlink_doors_clear(void) { door_count = 0; }

void chainlink_door_place(GameState area, int32_t x, int32_t y, int32_t z,
                          int32_t rot_y) {
    if (door_count >= MAX_CHAINLINK_DOORS) return;
    ChainlinkDoor *d = &doors[door_count++];
    d->area  = area;
    d->x = x;  d->y = y;  d->z = z;
    d->rot_y = rot_y;
    d->active = 1;

    /* World AABB = axis-aligned bound of the rotated local rect. Rotating the
       four corners covers the asymmetric Z origin exactly, at any angle; the
       concrete props' half-extent formula assumes a centred rect and would
       misplace this one. Done once, at place time. */
    int32_t c = icos(rot_y), s = isin(rot_y);
    const int32_t lx[4] = { CLDOOR_MIN_X_OFF, CLDOOR_MAX_X_OFF,
                            CLDOOR_MAX_X_OFF, CLDOOR_MIN_X_OFF };
    const int32_t lz[4] = { CLDOOR_MIN_Z_OFF, CLDOOR_MIN_Z_OFF,
                            CLDOOR_MAX_Z_OFF, CLDOOR_MAX_Z_OFF };
    int k;
    for (k = 0; k < 4; k++) {
        /* Same handedness as RotMatrix's Y rotation, which the draw uses. */
        int32_t wx = x + ((lx[k] * c + lz[k] * s) >> 12);
        int32_t wz = z + ((lz[k] * c - lx[k] * s) >> 12);
        if (k == 0) {
            d->min_x = d->max_x = wx;
            d->min_z = d->max_z = wz;
        } else {
            if (wx < d->min_x) d->min_x = wx;
            if (wx > d->max_x) d->max_x = wx;
            if (wz < d->min_z) d->min_z = wz;
            if (wz > d->max_z) d->max_z = wz;
        }
    }
}

/* Player push-out (dresser-style Minkowski AABB). Area-gated so the shared
   reception collision routine can call it unconditionally. */
void chainlink_doors_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;   /* the rooms that use it are single flat floors */
    int i;
    for (i = 0; i < door_count; i++) {
        ChainlinkDoor *d = &doors[i];
        if (!d->active || d->area != current_area) continue;

        int32_t min_x = d->min_x - radius - CLDOOR_PUSH_MARGIN;
        int32_t max_x = d->max_x + radius + CLDOOR_PUSH_MARGIN;
        int32_t min_z = d->min_z - radius - CLDOOR_PUSH_MARGIN;
        int32_t max_z = d->max_z + radius + CLDOOR_PUSH_MARGIN;

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

/* Hitscan volume: the real panel, no player standoff, height-aware. The room's
   own fence panels are collision walls and stop shots, so this one does too. */
int chainlink_doors_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack) {
    int i;
    for (i = 0; i < door_count; i++) {
        ChainlinkDoor *d = &doors[i];
        if (!d->active || d->area != current_area) continue;
        /* Base rests on the floor at (d->y + GROUND_FLOOR_Y); the panel reaches
           CLDOOR_SOLID_H above it (-Y is up). */
        int32_t base = d->y + GROUND_FLOOR_Y;
        if (y < base - CLDOOR_SOLID_H || y > base) continue;
        if (x < d->min_x - slack || x > d->max_x + slack) continue;
        if (z < d->min_z - slack || z > d->max_z + slack) continue;
        return 1;
    }
    return 0;
}

/* Render every instance in the current area, with the same fog and cull budget
   as the interior rooms. chnlnk is 4bpp with its wire gaps on CLUT entry 0,
   which the GPU skips outright — so the panel is see-through with no blend
   state and no extra sorting, exactly like the cage in the room mesh. Restores
   the caller's view matrix before returning. */
void chainlink_doors_draw(RenderContext *ctx) {
    if (door_count == 0 || !cldoor_smd) return;

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
    for (i = 0; i < door_count; i++) {
        ChainlinkDoor *dr = &doors[i];
        if (!dr->active || dr->area != current_area) continue;

        int32_t dcx = dr->x - cam_x, dcz = dr->z - cam_z;
        if ((dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz) > 1500) continue;

        MATRIX pm, combined;
        SVECTOR prot = {0, dr->rot_y, 0, 0};
        RotMatrix(&prot, &pm);
        VECTOR pos = {dr->x, dr->y + GROUND_FLOOR_Y, dr->z};
        TransMatrix(&pm, &pos);
        CompMatrixLV(&view, &pm, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        uint8_t *p = (uint8_t *)cldoor_smd->p_prims;
        int pi;
        for (pi = 0; pi < cldoor_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt      = (SMD_PRI_TYPE *)p;
            uint8_t       stride  = pt->len;
            int           is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &cldoor_smd->p_verts[vi[0]];
            SVECTOR *v1 = &cldoor_smd->p_verts[vi[1]];
            SVECTOR *v2 = &cldoor_smd->p_verts[vi[2]];

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
                v3 = &cldoor_smd->p_verts[vi[3]];
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
                poly->tpage = cldoor_tpage;
                poly->clut  = cldoor_clut;
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
                poly->tpage = cldoor_tpage;
                poly->clut  = cldoor_clut;
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
