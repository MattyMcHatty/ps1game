#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "cull_arena.h"
#include "tim_slots.h"
#include "texmgr.h"
#include "camera.h"
#include "collision.h"
#include "room_data.h"
#include "sharurs_trial.h"
#include "sharurs_trial_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "hell_hath_no_fury.h"  /* the volcanic paths, a narrow uploader      */
#include "catacombs_entry.h"    /* cobble + the inner door, narrow uploaders  */
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"
#include "nessie.h"             /* the north-west lava's monster              */

/* Sharur's Trial - see sharurs_trial.h. The room half is src/hell_hath_no_
   fury.c's, cull keys, radius side cull and all, without its opening drop. */

static SMD  *st_smd  = NULL;
static void *st_buff = NULL;

/* ---- The lava sea ----------------------------------------------------------
   x[-3300,3300] z[-2699,2700] (the collision proxy's bounds), floor at y=0, and
   the sky centred on the middle of it, half way up the ruins. */
#define ST_CX                0
#define ST_CZ                0
#define ST_SKY_Y          (-300)

#define ST_FLOOR_Y           0
#define ST_EYE_Y          (ST_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The ruins and the doors are the tallest things here and reach y=-600. */
#define ST_CEILING_Y      (-600)

/* ---- THE NORTH-EAST DOOR, back to Hell Hath No Fury ------------------------
   The wd_dr polys in "Sharurs Trial.smx" span (2900,2300)-(3100,2100): centre
   (3000,2200), on the 45-degree rim x+z=5200 (collision wall 38, inward normal
   (-0.707,-0.707)), so the player walks up to it from the south-west.

   THE SIGN is on a free yaw (door_draw_string_3d_yaw), facing the way the
   reader faces - 512, north-east - 11 off the door toward the player.

   THE SPAWN is 250 in along the normal (177 on each axis), clear of the 195
   push radius, facing south-west into the room. */
#define ST_NE_X           3000
#define ST_NE_Z           2200
#define ST_NE_TEXT_X      2992
#define ST_NE_TEXT_Z      2192
#define ST_NE_TEXT_YAW     512
#define ST_NE_SPAWN_X     2823
#define ST_NE_SPAWN_Z     2023
#define ST_NE_SPAWN_ROT   2560
#define ST_DOOR_TEXT_Y    (ST_FLOOR_Y - 200)
#define ST_TEXT_RADIUS    1500
#define ST_FADE_NEAR      1000
#define ST_TRIGGER_RADIUS  500

/* Circle edge-detect for the door. Seeded "held" by the arm so a press
   carried in through the transition cannot fire on the arrival frame. */
static int ne_circle_prev = 1;

static void st_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -3300;  floor_zones[0].max_x = 3300;
    floor_zones[0].min_z = -2700;  floor_zones[0].max_z = 2700;
    floor_zones[0].y     = ST_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   SIX mesh textures, ONE owned:
     0  volcanic             Hell Hath No Fury's, its narrow uploader (x384 y256)
     1  lava                 the Descent's, its narrow uploader (x320 y256). Its
                             UVs run past 128 and wrap in the 128 window.
     2  rocks                OWNED. 4bpp x512 y0, CLUT (960,502): a page start
                             tools/VRAM_MAP_UNDERWORLD.txt calls FREE.
     3  cobblestones         the Catacombs Entry's, its narrow uploader
     4  wd_dr                RESIDENT: the fat door's startup upload at x768
                             y256 - which is also DOOR_PANEL_WOOD's panel, the
                             transition out of this room. Nothing here may
                             take that page.
     5  catacomb inner door  the Catacombs Entry's, its narrow uploader
   The sky's texture is src/underworld_sky.c's business. */
#define ST_TEX_COUNT 6

static uint16_t tex_tpage[ST_TEX_COUNT];
static uint16_t tex_clut[ST_TEX_COUNT];
static int      rocks_tex = -1;

void sharurs_trial_load_assets(void) {
    /* BANK: Chapter 4. Derived, not guessed - py tools/check_tex_banks.py walks
       the uploader call graph and fails the build if this is short. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    rocks_tex = texmgr_register("\\TEXUNDW\\ROCKS.TIM;1");
    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, LAVA);
    TIM_SLOT(2, ROCKS);
    TIM_SLOT(3, COBBLE);
    TIM_SLOT(4, WDDR);
    TIM_SLOT(5, CTCMBDR);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void sharurs_trial_upload_textures(void) {
    hell_hath_no_fury_upload_volcanic();
    descent_to_hell_upload_lava();
    if (rocks_tex >= 0) texmgr_upload(rocks_tex);
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   hhnf_build_cull_keys() verbatim (src/hell_hath_no_fury.c, and
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius): this room is
   the same open shape - a fog distance close to the room's own size - only
   larger. Built on the same call that reloads the mesh it describes, and
   nowhere else. */
static int     st_key_count = 0;
static int32_t st_y_lo = 0, st_y_hi = 0;

static void st_build_cull_keys(void) {
    st_key_count = 0;
    if (!st_smd) return;
    uint8_t *p = (uint8_t *)st_smd->p_prims;
    int i, n = st_smd->n_prims;
    if (n > SHARURS_TRIAL_PRIM_COUNT) n = SHARURS_TRIAL_PRIM_COUNT;
    st_y_lo = st_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &st_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &st_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < st_y_lo) st_y_lo = vk->vy;
            if (vk->vy > st_y_hi) st_y_hi = vk->vy;
        }
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        {
            int32_t rad = (hi_x - lo_x) + (hi_z - lo_z);   /* >= any |v - v0| */
            rad = (rad + 3) >> 2;
            cull_keys[i].pad = (uint8_t)(rad >= 255 ? 255 : rad);
        }
        p += pt->len;
    }
    st_key_count = n;
}

void sharurs_trial_load_geometry(void) {
    st_buff = room_arena_load("\\TEXUNDW\\SHARUR.SMD;1");
    st_smd  = st_buff ? smdInitData(st_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (st_smd && !room_data_bind(st_smd->n_prims)) st_smd = NULL;
    st_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The north-east door ---------------------------------------------------*/
void sharurs_trial_doors_arm(void) {
    ne_circle_prev = interact_tapped();
}

int sharurs_trial_ne_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !ne_circle_prev;
    int32_t dx, dz;
    ne_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - ST_NE_X;
    dz = cam_z - ST_NE_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= ST_TRIGGER_RADIUS)
        return 0;
    return interact_facing(ST_NE_X, ST_NE_Z);
}

static void st_ne_text(RenderContext *ctx) {
    int32_t dx = cam_x - ST_NE_X;
    int32_t dz = cam_z - ST_NE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= ST_TEXT_RADIUS) return;
    if (xz > ST_FADE_NEAR) {
        int range = ST_TEXT_RADIUS - ST_FADE_NEAR;
        int prog  = xz - ST_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            ST_NE_TEXT_X, ST_DOOR_TEXT_Y, ST_NE_TEXT_Z,
                            50, 255, 50, fade, ST_NE_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void sharurs_trial_spawn_ne(void) {
    cam_x     = ST_NE_SPAWN_X;
    cam_y     = ST_EYE_Y;
    cam_vy    = 0;
    cam_z     = ST_NE_SPAWN_Z;
    cam_rot   = ST_NE_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    sharurs_trial_doors_arm();
}

void sharurs_trial_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(ST_CEILING_Y);
    st_floor_zones_init();

    /* The one way in today, and where a debug jump lands. */
    sharurs_trial_spawn_ne();

    /* The global prop arrays, as Hell Hath No Fury clears them: its save point
       and tank would otherwise block invisibly inside this room's bounds. The
       clear drops the placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

/* ---- The mesh --------------------------------------------------------------
   draw_hhnf_smd() without the opening drop: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_st_smd(RenderContext *ctx) {
    if (!st_smd) return;

    uint8_t *p = (uint8_t *)st_smd->p_prims;
    int i, n = st_key_count;

    /* HOISTED OUT OF THE LOOP (STEP 3C fix 1): none of these can change while
       a frame is being queued. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = UW_FOG_FAR;
    int32_t fog_start = UW_FOG_NEAR < cull ? UW_FOG_NEAR : cull / 2;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* ---- THE SIDE-PLANE CULL'S PER-FRAME TERMS ------------------------------
       Hell Hath No Fury's, unchanged: see draw_hhnf_smd for the derivation
       (the pitch bound, the 10*R radius term and the arithmetic). */
#define ST_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = st_y_lo - cam_y, dyhi = st_y_hi - cam_y;
        if (ps   < 0) ps   = -ps;
        if (dylo < 0) dylo = -dylo;
        if (dyhi < 0) dyhi = -dyhi;
        pslack = ps * (dylo > dyhi ? dylo : dyhi) + 512;
    }

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > cull)
                { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12))
                { p += stride; continue; }
        }
        /* The side planes, off the key point and its radius. */
        if (!no_frustum && cull_keys[i].pad != 255) {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t f  = dx * sn + dz * cs;
            int32_t sd = dx * cs - dz * sn;
            int32_t r  = ((int32_t)cull_keys[i].pad * 40) << 12;   /* >= 10R */
            if (pitched) f = (((f >> 8) * pc) >> 4) + pslack;
            f = f * 5 + ST_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &st_smd->p_verts[vi[0]];
        SVECTOR *v1 = &st_smd->p_verts[vi[1]];
        SVECTOR *v2 = &st_smd->p_verts[vi[2]];

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

        int nocull = (i < SHARURS_TRIAL_PRIM_COUNT) && room_nocull(i);
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
            v3 = &st_smd->p_verts[vi[3]];
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
        if (otz >= UW_SKY_OT) otz = UW_SKY_OT - 1;   /* in front of the sky */

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog = dist < fog_start ? fog_start : (dist > cull ? cull : dist);
        int32_t fog_factor = ((cull - fog) << 8) / (cull - fog_start);

        uint8_t tex_idx = (i < SHARURS_TRIAL_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < ST_TEX_COUNT);
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + UW_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + UW_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + UW_FOG_B * (256 - fog_factor)) >> 8);

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

void sharurs_trial_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All six room textures sit at Voff 0, and so does the sky's. Nessie's
       sheet does NOT (Voff 128), so she brackets herself and is told what to
       restore. */
    RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
    nessies_set_texwindow(&tw);
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
            setTexWindow(twin, &tw);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    MATRIX view;
    camera_build_view(&view);
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    if (exp != DBG_EXP_NO_MESH) {
        underworld_sky_draw(ctx, ST_CX, ST_SKY_Y, ST_CZ);
        draw_st_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE DOOR SIGN, ANY PICKUP a later seed puts down, AND
       NESSIE with her fireballs (area-gated inside; src/nessie.h). No props. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        item_pickups_draw(ctx);
        draw_nessies(ctx);
        st_ne_text(ctx);
    }
}
