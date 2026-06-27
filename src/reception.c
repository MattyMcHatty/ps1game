#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "reception.h"
#include "collision.h"
#include "reception_mesh_collision.h"
#include "reception_tex_map.h"

/* Placeholder reception room: untextured geometry rendered flat-shaded with the
   same distance fog as the kitchen, so it blends with the dark interior until
   the finished art replaces it. Modelled on kitchen_dining.c but with no
   textures/tex_map (Reception.smx carries no texture references yet). */

static SMD  *reception_smd  = NULL;
static void *reception_buff = NULL;

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

/* Multi-level floor layout (taken from the collision mesh, Reception_mesh.smx):
   ground (y=0) -> ramp A up to a y=-150 platform -> ramp B up to the y=-600
   upper floor. apply_height() picks the first matching zone, skipping flat/upper
   zones that sit ABOVE the player, so elevated zones must be listed BEFORE the
   ground catch-all (which spans the whole room beneath everything). */
static void reception_floor_zones_init(void) {
    int i = 0;

    /* Ramp A — climbs along +X from ground (y=0 @ x=-100) to the platform
       (y=-150 @ x=300). */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = -100; floor_zones[i].max_x = 300;
    floor_zones[i].min_z = -700; floor_zones[i].max_z = -300;
    floor_zones[i].ramp_y_start    = 0;     /* y at x=-100 (ground)   */
    floor_zones[i].ramp_y_end      = -150;  /* y at x=300  (platform) */
    floor_zones[i].ramp_axis_start = -100;
    floor_zones[i].ramp_axis_end   = 300;
    floor_zones[i].ramp_along_x    = 1;
    i++;

    /* Ramp B — climbs along +Z from the platform (y=-150 @ z=-300) to the
       upper floor (y=-600 @ z=500). */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = 300; floor_zones[i].max_x = 700;
    floor_zones[i].min_z = -300; floor_zones[i].max_z = 500;
    floor_zones[i].ramp_y_start    = -150;  /* y at z=-300 (platform)    */
    floor_zones[i].ramp_y_end      = -600;  /* y at z=500  (upper floor) */
    floor_zones[i].ramp_axis_start = -300;
    floor_zones[i].ramp_axis_end   = 500;
    floor_zones[i].ramp_along_x    = 0;
    i++;

    /* Platform between the two ramps (y=-150). */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = 300; floor_zones[i].max_x = 700;
    floor_zones[i].min_z = -700; floor_zones[i].max_z = -300;
    floor_zones[i].y     = -150;
    i++;

    /* Upper floor (y=-600), L-shaped: the +Z/+X area... */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = -900; floor_zones[i].max_x = 1500;
    floor_zones[i].min_z = 500;  floor_zones[i].max_z = 1500;
    floor_zones[i].y     = -600;
    i++;

    /* ...and the west strip. */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = -1500; floor_zones[i].max_x = -900;
    floor_zones[i].min_z = -1500; floor_zones[i].max_z = 1500;
    floor_zones[i].y     = -600;
    i++;

    /* Ground catch-all (whole room) — MUST be last so elevated zones win. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1500; floor_zones[i].max_x = 1500;
    floor_zones[i].min_z = -1500; floor_zones[i].max_z = 1500;
    floor_zones[i].y     = 0;
    i++;

    floor_zone_count = i;
}

/* Load geometry at STARTUP (a CD read only — no LoadImage, so it's safe and
   leaves gameplay CD-read-free, like the kitchen). */
void reception_load_assets(void) {
    reception_buff = load_file_from_cd("\\RECEPT.SMD;1");
    if (reception_buff)
        reception_smd = smdInitData(reception_buff);
}

/* ---- Per-room texture streaming -------------------------------------------
   Uploads reception's nine mesh textures into VRAM during the STATE_LOADING
   transition (NOT at startup). Historically everything loaded at startup because
   LoadImage hard-crashes when issued while the per-frame draw is in flight
   (TEXTURING_NOTES PROBLEM A — it died on the 4th mid-loop upload). The caller
   idles the GPU with DrawSync(0) first and kicks no DrawOTagEnv until the next
   flip_buffers, so these uploads run with the GPU quiescent (validated by the
   original 6-texture spike). Streaming lets rooms share VRAM: reception's new
   textures reuse slots the kitchen also uses, and the kitchen re-streams its own
   textures when re-entered (see kitchen_stream_textures). */
#define RECEPTION_TEX_COUNT 9
static uint16_t tex_tpage[RECEPTION_TEX_COUNT];
static uint16_t tex_clut[RECEPTION_TEX_COUNT];

static void load_tim_buf(const char *filename, int slot, uint8_t *buf, int bufcap) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return;
    int sectors = (file.size + 2047) / 2048;
    if (sectors * 2048 > bufcap) return;
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

void reception_stream_textures(void) {
    /* Static scratch buffer instead of a mid-game malloc: the heap is tighter
       once all rooms' geometry is resident, and a mid-game malloc here returned
       bad memory (LoadImage then crashed). TEMP test of that theory. */
    static uint8_t tbuf[32 * 1024];
    const int TBUF_CAP = sizeof(tbuf);
    /* Slots match reception_tex_map indices. The three NEW textures (strs,
       bnnstr, frnt_dr) stream into page-aligned VRAM that the kitchen also uses
       (stn_stl/kchn_tile/red_crpt slots) — kitchen re-streams its own textures
       when re-entered, so the two rooms share that VRAM. */
    load_tim_buf("\\TEX\\STRS.TIM;1",   0, tbuf, TBUF_CAP);
    load_tim_buf("\\REDWLPPR.TIM;1", 1, tbuf, TBUF_CAP);
    load_tim_buf("\\WDFLR.TIM;1",    2, tbuf, TBUF_CAP);
    load_tim_buf("\\TEX\\BNNSTR.TIM;1", 3, tbuf, TBUF_CAP);
    load_tim_buf("\\DINCL.TIM;1",    4, tbuf, TBUF_CAP);
    load_tim_buf("\\TEX\\FRNTDR.TIM;1", 5, tbuf, TBUF_CAP);
    load_tim_buf("\\WDDR.TIM;1",     6, tbuf, TBUF_CAP);
    load_tim_buf("\\INRDBLDR.TIM;1", 7, tbuf, TBUF_CAP);
    load_tim_buf("\\STNGLS.TIM;1",   8, tbuf, TBUF_CAP);
}

void reception_init(void) {
    reception_collision_init(&current_collision_room);
    reception_floor_zones_init();

    /* Spawn near the room centre, facing +X. Tune once the connecting doorway
       art is finalised. */
    cam_x   = 0;
    cam_y   = -149;   /* floor at world y=0 -> eye target 0 - GROUND_FLOOR_Y */
    cam_vy  = 0;
    cam_z   = 0;
    cam_rot = 1024;   /* facing +X */
}

static void draw_reception_smd(RenderContext *ctx) {
    if (!reception_smd) return;

    uint8_t *p = (uint8_t *)reception_smd->p_prims;
    int i;

    for (i = 0; i < reception_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &reception_smd->p_verts[vi[0]];
        SVECTOR *v1 = &reception_smd->p_verts[vi[1]];
        SVECTOR *v2 = &reception_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan), just past where fog fully saturates
               (fog_end below) so culled polys are already invisible. */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 2300)
                { p += stride; continue; }
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
            SVECTOR *v3 = &reception_smd->p_verts[vi[3]];
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

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 350, fog_end = 2200;
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). Reception
           textures every face (no 0xFF), but keep the guard for safety. UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in reception_draw, reproducing the Blender UVs. */
        uint8_t tex_idx = (i < RECEPTION_PRIM_COUNT) ? reception_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < RECEPTION_TEX_COUNT);
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

        if (is_quad && textured) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
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
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
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

void reception_draw(RenderContext *ctx) {
    /* Dark interior background, same as the kitchen. */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. Sorted at OT_LENGTH-1 so the GPU applies it before any textured poly
       (those are clamped to OT_LENGTH-2). Mask = texels>>3 (128>>3 = 16). All
       reception textures sit at page-top (V 0-127), so one window serves them. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix from the camera (same construction as kitchen_dining_draw). */
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

    draw_reception_smd(ctx);
}
