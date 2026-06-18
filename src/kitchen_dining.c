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
#include "kitchen_dining_tex_map.h"

static SMD  *kitchen_smd  = NULL;
static void *kitchen_buff = NULL;

/* tpage/clut per texture slot, indexed by the SMX texture order:
   0=stn_stl 1=kchn_tile 2=wd_flr 3=red_wlppr 4=double_door
   5=inr_dbl_dr 6=red_crpt 7=stove 8=din_cl */
#define KITCHEN_TEX_COUNT 9
static uint16_t tex_tpage[KITCHEN_TEX_COUNT];
static uint16_t tex_clut[KITCHEN_TEX_COUNT];

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

/* Load a TIM into VRAM using a caller-supplied scratch buffer, so all kitchen
   textures share ONE allocation instead of mallocing/freeing per file. This
   avoids heap churn/leakage from many transient buffers (which was crashing
   the kitchen load). */
static void load_tim_buf(const char *filename, int slot,
                         uint8_t *buf, int bufcap) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return;
    int sectors = (file.size + 2047) / 2048;
    if (sectors * 2048 > bufcap) return;   /* file too big for scratch buffer */
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        tex_clut[slot] = getClut(tim.crect->x, tim.crect->y);
    }
    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
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

/* Load all kitchen textures into VRAM. MUST be called at startup (before the
   main render loop begins), NOT mid-game: LoadImage is unreliable once the
   per-frame VSync/draw cycle is running — it hard-crashes after a few uploads —
   but is safe at startup, exactly how delivery_area loads its textures. Kitchen
   textures occupy VRAM regions that don't overlap delivery-area or prop
   textures, so all stay resident together. */
void kitchen_load_textures(void) {
    const int TBUF_CAP = 32 * 1024;
    uint8_t *tbuf = malloc(TBUF_CAP);
    if (!tbuf) return;
    load_tim_buf("\\STNSTL.TIM;1",   0, tbuf, TBUF_CAP);
    load_tim_buf("\\KCHNTILE.TIM;1", 1, tbuf, TBUF_CAP);
    load_tim_buf("\\WDFLR.TIM;1",    2, tbuf, TBUF_CAP);
    load_tim_buf("\\REDWLPPR.TIM;1", 3, tbuf, TBUF_CAP);
    load_tim_buf("\\DBLDOOR.TIM;1",  4, tbuf, TBUF_CAP);
    load_tim_buf("\\INRDBLDR.TIM;1", 5, tbuf, TBUF_CAP);
    load_tim_buf("\\REDCRPT.TIM;1",  6, tbuf, TBUF_CAP);
    load_tim_buf("\\STOVE.TIM;1",    7, tbuf, TBUF_CAP);
    load_tim_buf("\\DINCL.TIM;1",    8, tbuf, TBUF_CAP);
    free(tbuf);
}

void kitchen_dining_init(void) {
    kitchen_dining_collision_init(&current_collision_room);
    kitchen_dining_floor_zones_init();

    cam_x   = 381;
    cam_y   = -149;   /* floor at world y=0 → target = 0 - GROUND_FLOOR_Y = -149 */
    cam_vy  = 0;
    cam_z   = 676;
    cam_rot = 3072;   /* facing west (-X) */

    /* Textures are uploaded once at startup via kitchen_load_textures(). Only
       geometry is loaded here (a CD read, no LoadImage), which is safe mid-game. */
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
            /* Distance cull (Manhattan), just past where fog fully saturates
               (fog_end below) so culled polys are already invisible. */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 2600)
                { p += stride; continue; }
            /* Behind-camera cull: dot of (poly-cam) with the camera forward
               vector (isin,icos, scaled by 4096). Skip polys clearly behind the
               view before paying for the GTE transform. The generous margin
               keeps large polys that straddle the view plane from popping. */
            int32_t fwd = dx * isin(cam_rot) + dz * icos(cam_rot);
            if (fwd < -(700 << 12))
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

        if (!pt->nocull) {
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
        /* Reserve OT_LENGTH-1 for the texture-window primitive (set in
           kitchen_dining_draw) so it is processed before every textured poly. */
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 500, fog_end = 2200;
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        uint8_t tex_idx = (i < KITCHEN_DINING_PRIM_COUNT) ? kitchen_dining_tex_map[i] : 0xFF;

        if (is_quad && tex_idx != 0xFF && tex_idx < KITCHEN_TEX_COUNT) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            /* Use the exporter's per-poly UVs straight from the SMD primitive
               (FT4 stride 32: tu0,tv0..tu3,tv3 at offsets 20-27). They are
               already normalized into 0-255 and wrap via the 128 texture
               window set in kitchen_dining_draw, reproducing the Blender UVs. */
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8));
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
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

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. Placed at the top of the OT (OT_LENGTH-1) so the GPU applies it
       before any textured kitchen poly (those are clamped to OT_LENGTH-2).
       Mask is texels>>3, matching the SDK convention (128>>3 = 16). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

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
    /* Player overlays + debug collision view are drawn by the shared
       draw_player_systems step in main (applies to every area). */
}
