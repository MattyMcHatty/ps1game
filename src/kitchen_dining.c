#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "kitchen_dining.h"
#include "collision.h"
#include "kitchen_dining_mesh_collision.h"

static SMD  *kitchen_smd  = NULL;
static void *kitchen_buff = NULL;

static void *load_file_from_cd(const char *filename) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buff = malloc(sectors * 2048);
    if (!buff) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buff;
}

static void kitchen_dining_floor_zones_init(void) {
    int i = 0;

    /* Large kitchen room (FLOOR 7) */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -3294; floor_zones[i].max_x = -629;
    floor_zones[i].min_z = -1000; floor_zones[i].max_z =  1000;
    floor_zones[i].y     = 0;
    i++;

    /* Dining / entry area (FLOOR 8) */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -600; floor_zones[i].max_x = 600;
    floor_zones[i].min_z = -1000; floor_zones[i].max_z = 1000;
    floor_zones[i].y     = 0;
    i++;

    /* South corridor (FLOOR 9) */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -581; floor_zones[i].max_x = -8;
    floor_zones[i].min_z = -1699; floor_zones[i].max_z = -1025;
    floor_zones[i].y     = 0;
    i++;

    /* Southeast little room (FLOOR 7) */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x =  18; floor_zones[i].max_x = 591;
    floor_zones[i].min_z = -1699; floor_zones[i].max_z = -1025;
    floor_zones[i].y     = 0;
    i++;

    floor_zone_count = i;
}

void kitchen_dining_init(void) {
    kitchen_dining_collision_init(&current_collision_room);
    kitchen_dining_floor_zones_init();

    /* Dead center of the big empty kitchen room: x(-2196..-419) z(-800..800) */
    cam_x   = -1300;
    cam_y   = -149;   /* floor at world y=0 → target = 0 - GROUND_FLOOR_Y = -149 */
    cam_vy  = 0;
    cam_z   = 0;
    cam_rot = 1024;   /* facing east toward the doorway to the dining room */

    kitchen_buff = load_file_from_cd("\\KITCHN.SMD;1");
    if (kitchen_buff)
        kitchen_smd = smdInitData(kitchen_buff);
}

static void draw_kitchen_smd(RenderContext *ctx) {
    if (!kitchen_smd) return;

    uint8_t *p = (uint8_t *)kitchen_smd->p_prims;
    int i;

    for (i = 0; i < kitchen_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &kitchen_smd->p_verts[vi[0]];
        SVECTOR *v1 = &kitchen_smd->p_verts[vi[1]];
        SVECTOR *v2 = &kitchen_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 5000)
                { p += stride; continue; }
        }

        DVECTOR sv[4];
        int32_t sz[4];
        int32_t otz, nclip;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);

        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        if (!pt->nocull && !is_quad) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        if (is_quad) {
            SVECTOR *v3 = &kitchen_smd->p_verts[vi[3]];
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
        if (otz >= OT_LENGTH) otz = OT_LENGTH - 1;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 500, fog_end = 3000;
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8));
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
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8));
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }

        p += stride;
    }
}

void kitchen_dining_draw(RenderContext *ctx) {
    /* Dark interior background */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    MATRIX rot_matrix;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};

    RotMatrix(&neg_rot, &rot_matrix);

    VECTOR trans;
    trans.vx = -cam_x;
    trans.vy = -cam_y;
    trans.vz = -cam_z;

    ApplyMatrixLV(&rot_matrix, &trans, &trans);

    rot_matrix.t[0] = trans.vx;
    rot_matrix.t[1] = trans.vy;
    rot_matrix.t[2] = trans.vz;

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_kitchen_smd(ctx);

#ifdef DEBUG_COLLISION
    debug_draw_walls(ctx);
    debug_draw_coords(ctx);
#endif
}
