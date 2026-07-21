#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "conservatory.h"
#include "collision.h"
#include "conservatory_mesh_collision.h"
#include "conservatory_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "concrete_props.h"
#include "copper_pot.h"
#include "fatdoor.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Conservatory: a long flat-floored glasshouse off reception's west wall,
   rendered the same way as the piano room (per-poly tex map + 128 texture
   window + fog). */

static SMD  *conservatory_smd  = NULL;
static void *conservatory_buff = NULL;

/* Single flat floor at y=0 across the walkable bounds (all detected floors in
   conservatory_mesh_collision.c sit at y=0 — the upstairs is scenery). */
static void conservatory_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -2601; floor_zones[0].max_x = 0;
    floor_zones[0].min_z = -311;  floor_zones[0].max_z = 1702;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Nine mesh textures. Three are resident from startup (wd_flr/din_cl with the
   kitchen, wd_dr with the fat door). SIX are streamed on every conservatory
   entry:
     - prpl_wlppr and strs time-share the stove / stn_stl slots (also streamed
       by the piano room and reception respectively, but a title-screen load
       straight into this room guarantees nothing, so we upload our own copies);
     - the four conservatory-only textures (con_tile/trees/grss/upstairs)
       time-share the DELIVERY-only slots (double_door/gravel/rusty_fence/
       brick_wall), restored by delivery_restore_textures() on delivery entry
       (double_door also by the kitchen's restore list). */
#define CONSERVATORY_TEX_COUNT 9

/* Streamed slots: engine slot -> texmgr id. */
#define CONSERVATORY_NEW_TEX 6
static int new_tex_id[CONSERVATORY_NEW_TEX];
static const struct { const char *file; int slot; } new_tex[CONSERVATORY_NEW_TEX] = {
    { "\\TEX\\PRPLWLP.TIM;1",  1 },
    { "\\TEX\\STRS.TIM;1",     3 },
    { "\\TEX\\CONTILE.TIM;1",  4 },
    { "\\TEX\\TREES.TIM;1",    5 },
    { "\\TEX\\GRSS.TIM;1",     6 },
    { "\\TEX\\UPSTAIRS.TIM;1", 8 },
};

static uint16_t tex_tpage[CONSERVATORY_TEX_COUNT];
static uint16_t tex_clut[CONSERVATORY_TEX_COUNT];

/* Read a whole TIM into a freshly malloc'd buffer (caller owns it). NULL on fail. */
static uint8_t *read_tim(const char *filename) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    uint8_t *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

static void *load_file_from_cd(const char *filename) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buff = malloc(sectors * 2048);
    if (!buff) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buff;
}

/* Capture tpage/clut for a texture that is ALREADY resident in VRAM (shared
   with another room): read its header only, no LoadImage. */
static void capture_tpage(const char *filename, int slot) {
    uint8_t *buf = read_tim(filename);
    if (!buf) return;
    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    if (tim.mode & 0x8) tex_clut[slot] = getClut(tim.crect->x, tim.crect->y);
    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    free(buf);
}

/* Load geometry AND register/capture textures at STARTUP (the only time CD
   access is safe — see tools/TEXTURING_NOTES.txt). */
void conservatory_load_assets(void) {
    conservatory_buff = load_file_from_cd("\\TEX\\CONSRV.SMD;1");
    if (conservatory_buff)
        conservatory_smd = smdInitData(conservatory_buff);

    /* Streamed slots: RAM-resident via the texture manager, uploaded on entry. */
    for (int i = 0; i < CONSERVATORY_NEW_TEX; i++) {
        int slot = new_tex[i].slot;
        new_tex_id[i]   = texmgr_register(new_tex[i].file);
        tex_tpage[slot] = texmgr_tpage(new_tex_id[i]);
        tex_clut[slot]  = texmgr_clut(new_tex_id[i]);
    }

    /* Resident from startup (kitchen + fatdoor); just capture tpage/clut. */
    capture_tpage("\\WDFLR.TIM;1", 0);
    capture_tpage("\\DINCL.TIM;1", 2);
    capture_tpage("\\WDDR.TIM;1",  7);
}

/* Upload the six streamed textures from their resident RAM copies. Pure
   LoadImage — no CD access — safe during the room transition (the caller
   DrawSyncs first, as main's STATE_LOADING does). */
void conservatory_upload_textures(void) {
    for (int i = 0; i < CONSERVATORY_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);
    concrete_props_upload_textures();   /* cncrte -> kchn_tile slot (unused here) */
    copper_pot_upload_texture();        /* copper pot -> key slot (world sprite) */
}

/* ---- Door back to reception -----------------------------------------------
   The single wooden door on the east wall (x=0), centred on z~10 (from the
   wd_dr polys in Conservatory.smx). Same pattern as the piano room's. */
#define CONDOOR_X                   0
#define CONDOOR_Z                  10
#define CONDOOR_TEXT_Y          (-186)
#define CONDOOR_TEXT_RADIUS      1500
#define CONDOOR_FADE_NEAR        1000
#define CONDOOR_TRIGGER_RADIUS    500

/* Circle edge-detect, seeded by condoor_arm(). Starts "held" so a press
   carried in from the reception-side transition doesn't immediately bounce
   back. */
static int condoor_circle_prev = 1;

void condoor_arm(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    condoor_circle_prev = held;
}

int condoor_triggered(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int just = held && !condoor_circle_prev;
    condoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - CONDOOR_X;
    int32_t dz = cam_z - CONDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < CONDOOR_TRIGGER_RADIUS;
}

/* Floating "Press " BTN_CIRCLE " to enter" sign on the east door, in the YZ
   plane (faces along X). The player approaches from the -X (room) side, so
   mirror=1. */
static void condoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - CONDOOR_X;
    int32_t dz = cam_z - CONDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= CONDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > CONDOOR_FADE_NEAR) {
        int range = CONDOOR_TEXT_RADIUS - CONDOOR_FADE_NEAR;
        int prog  = xz - CONDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        CONDOOR_X, CONDOOR_TEXT_Y, CONDOOR_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

void conservatory_init(void) {
    conservatory_collision_init(&current_collision_room);
    conservatory_floor_zones_init();

    /* Spawn just inside the east door (from reception), facing west (-X) into
       the room. Flat floor y=0, standing eye height as reception. */
    cam_x   = -250;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = 10;
    cam_rot = 3072;   /* facing west (-X) */

    condoor_arm();   /* don't re-trigger on a held Circle from the entry */

    concrete_props_place();   /* 1 concrete block + 3 concrete chairs */
}

static void draw_conservatory_smd(RenderContext *ctx) {
    if (!conservatory_smd) return;

    uint8_t *p = (uint8_t *)conservatory_smd->p_prims;
    int i;

    for (i = 0; i < conservatory_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &conservatory_smd->p_verts[vi[0]];
        SVECTOR *v1 = &conservatory_smd->p_verts[vi[1]];
        SVECTOR *v2 = &conservatory_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible (same budget as the other rooms). */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 1500)
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

        /* Backface cull, except degenerate (triangle-shaped) quads flagged at
           build time in conservatory_nocull — same scheme as reception. */
        int nocull = (i < CONSERVATORY_PRIM_COUNT) && conservatory_nocull[i];
        if (!pt->nocull && !nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
        if (is_quad) {
            v3 = &conservatory_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner, not their average,
           so floors stay behind whatever stands on them (see render.h). */
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 350, fog_end = 1500;   /* fog saturates at the cull distance */
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in conservatory_draw. */
        uint8_t tex_idx = (i < CONSERVATORY_PRIM_COUNT) ? conservatory_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < CONSERVATORY_TEX_COUNT);
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

void conservatory_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = 350; g_fog_far = 1500;

    /* Dark interior background, same as the other rooms. */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. All nine conservatory textures sit at page-top (V 0-127), so one
       window serves them (see tools/TEXTURING_NOTES.txt). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix from the camera (same construction as the other rooms). */
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

    draw_conservatory_smd(ctx);
    /* Static concrete props (block + chairs). Their texture sits at page-top
       (V 0-127) so the room's 128 texture window serves it; restores the view
       matrix before returning. */
    concrete_props_draw(ctx);
    /* Copper pot collectible sprite (brackets its own texture window; the pot's
       VRAM sits at Voff 128 so it must disable the room's 128 window). */
    copper_pot_draw(ctx);
    /* Breakable doors filling the two north-wall openings. Draw with the room's
       active 128 window (their wd_dr UVs are 0-127, so wrapping is a no-op);
       restores the view matrix before returning. */
    fatdoors_draw(ctx);
    condoor_text(ctx);
}
