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
#include "lava_u.h"
#include "lava_u_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "hell_hath_no_fury.h"  /* the volcanic paths, a narrow uploader      */
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "sharurs_trial.h"      /* the rocks, a narrow uploader               */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"
#include "ahriman.h"            /* the south-west corner's floating eye      */

/* Lava U - see lava_u.h. The room half is src/sharurs_trial.c's (itself Hell
   Hath No Fury's): cull keys, radius side cull and all. */

static SMD  *lu_smd  = NULL;
static void *lu_buff = NULL;

/* ---- The U -----------------------------------------------------------------
   x[-6900,2100] z[-5100,300] (the collision proxy's bounds), floor at y=0, and
   the sky centred on the middle of it, half way up the rock island. */
#define LU_CX            (-2400)
#define LU_CZ            (-2400)
#define LU_SKY_Y          (-300)

#define LU_FLOOR_Y           0
#define LU_EYE_Y          (LU_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The rock island is the tallest thing here and reaches y=-617. */
#define LU_CEILING_Y      (-620)

/* ---- THE TWO DOORS, both back to Sharur's Trial ----------------------------
   Both are wd_dr in "Lava U.smx", standing in the north rim at z=300
   (collision wall 4, inward normal (0,-1)), so the player walks up to each
   from the south:
     NORTH-EAST  polys x[-100,88]     -> centre x=-6, at the head of the U's
                 east arm. Leads to Sharur's Trial's SOUTH door (a catacomb
                 inner door) - the user's "south east" door.
     NORTH-WEST  polys x[-4949,-4508] -> centre x=-4730, at the head of the
                 west arm (a warped, wider door). Leads to Sharur's Trial's
                 SOUTH-WEST door.

   THE SIGNS are on a free yaw (door_draw_string_3d_yaw), facing the way the
   reader faces - 0, north - 11 off the wall toward the player.

   THE SPAWNS are 250 in from the wall (clear of the 195 push radius), facing
   south into the room. Both stand on the volcanic path, not the lava. */
#define LU_NE_X             (-6)
#define LU_NW_X          (-4730)
#define LU_DOOR_Z           300
#define LU_TEXT_Z           289
#define LU_TEXT_YAW           0
#define LU_SPAWN_Z           50
#define LU_SPAWN_ROT       2048
#define LU_DOOR_TEXT_Y    (LU_FLOOR_Y - 200)
#define LU_TEXT_RADIUS    1500
#define LU_FADE_NEAR      1000
#define LU_TRIGGER_RADIUS  500

/* Circle edge-detect, one per door. Seeded "held" by the arm so a press
   carried in through the transition cannot fire on the arrival frame. */
static int ne_circle_prev = 1;
static int nw_circle_prev = 1;

static void lu_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -6900;  floor_zones[0].max_x = 2100;
    floor_zones[0].min_z = -5100;  floor_zones[0].max_z = 300;
    floor_zones[0].y     = LU_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   FOUR mesh textures, NONE owned - so no texmgr_set_bank() and no CD access:
     0  volcanic   Hell Hath No Fury's, its narrow uploader (x384 y256)
     1  lava       the Descent's, its narrow uploader (x320 y256). Its UVs run
                   past 128 and wrap in the 128 window.
     2  rocks_128  Sharur's Trial's ROCKS.TIM (4bpp x512 y0): the same 128x128
                   art under its Blender material name, through
                   sharurs_trial_upload_rocks().
     3  wd_dr      RESIDENT: the fat door's startup upload at x768 y256 - which
                   is also DOOR_PANEL_WOOD's panel, the transition out of this
                   room through either door. Nothing here may take that page.
   The sky's texture is src/underworld_sky.c's business. */
#define LU_TEX_COUNT 4

static uint16_t tex_tpage[LU_TEX_COUNT];
static uint16_t tex_clut[LU_TEX_COUNT];

void lava_u_load_assets(void) {
    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, LAVA);
    TIM_SLOT(2, ROCKS);
    TIM_SLOT(3, WDDR);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void lava_u_upload_textures(void) {
    hell_hath_no_fury_upload_volcanic();
    descent_to_hell_upload_lava();
    sharurs_trial_upload_rocks();
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   hhnf_build_cull_keys() verbatim (src/hell_hath_no_fury.c, and
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius): this room is
   the same open shape - a fog distance close to the room's own size. Built
   on the same call that reloads the mesh it describes, and nowhere else. */
static int     lu_key_count = 0;
static int32_t lu_y_lo = 0, lu_y_hi = 0;

static void lu_build_cull_keys(void) {
    lu_key_count = 0;
    if (!lu_smd) return;
    uint8_t *p = (uint8_t *)lu_smd->p_prims;
    int i, n = lu_smd->n_prims;
    if (n > LAVA_U_PRIM_COUNT) n = LAVA_U_PRIM_COUNT;
    lu_y_lo = lu_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &lu_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &lu_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < lu_y_lo) lu_y_lo = vk->vy;
            if (vk->vy > lu_y_hi) lu_y_hi = vk->vy;
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
    lu_key_count = n;
}

void lava_u_load_geometry(void) {
    lu_buff = room_arena_load("\\TEXUNDW\\LAVAU.SMD;1");
    lu_smd  = lu_buff ? smdInitData(lu_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (lu_smd && !room_data_bind(lu_smd->n_prims)) lu_smd = NULL;
    lu_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The two doors ---------------------------------------------------------*/
void lava_u_doors_arm(void) {
    int held = interact_tapped();
    ne_circle_prev = held;
    nw_circle_prev = held;
}

/* The shared body: a fresh Circle within reach of (door_x, LU_DOOR_Z) and
   facing it. The edge state is kept up to date even while locked. */
static int lu_door_triggered(int32_t door_x, int *circle_prev, int lock) {
    int held = interact_tapped();
    int just = held && !*circle_prev;
    int32_t dx, dz;
    *circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - door_x;
    dz = cam_z - LU_DOOR_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= LU_TRIGGER_RADIUS)
        return 0;
    return interact_facing(door_x, LU_DOOR_Z);
}

int lava_u_ne_door_triggered(int lock) {
    return lu_door_triggered(LU_NE_X, &ne_circle_prev, lock);
}

int lava_u_nw_door_triggered(int lock) {
    return lu_door_triggered(LU_NW_X, &nw_circle_prev, lock);
}

static void lu_door_text(RenderContext *ctx, int32_t door_x) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - LU_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= LU_TEXT_RADIUS) return;
    if (xz > LU_FADE_NEAR) {
        int range = LU_TEXT_RADIUS - LU_FADE_NEAR;
        int prog  = xz - LU_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            door_x, LU_DOOR_TEXT_Y, LU_TEXT_Z,
                            50, 255, 50, fade, LU_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

static void lu_spawn_at(int32_t door_x) {
    cam_x     = door_x;
    cam_y     = LU_EYE_Y;
    cam_vy    = 0;
    cam_z     = LU_SPAWN_Z;
    cam_rot   = LU_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    lava_u_doors_arm();
}

void lava_u_spawn_ne(void) { lu_spawn_at(LU_NE_X); }
void lava_u_spawn_nw(void) { lu_spawn_at(LU_NW_X); }

void lava_u_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(LU_CEILING_Y);
    lu_floor_zones_init();

    /* The default arrival (main.c overrides it per door), and where a debug
       jump lands. */
    lava_u_spawn_ne();

    /* The global prop arrays, as Sharur's Trial clears them: Hell Hath No
       Fury's save point and tank would otherwise block invisibly inside this
       room's bounds. The clear drops the placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

/* ---- The mesh --------------------------------------------------------------
   draw_st_smd() (src/sharurs_trial.c) verbatim: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_lu_smd(RenderContext *ctx) {
    if (!lu_smd) return;

    uint8_t *p = (uint8_t *)lu_smd->p_prims;
    int i, n = lu_key_count;

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
#define LU_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = lu_y_lo - cam_y, dyhi = lu_y_hi - cam_y;
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
            f = f * 5 + LU_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &lu_smd->p_verts[vi[0]];
        SVECTOR *v1 = &lu_smd->p_verts[vi[1]];
        SVECTOR *v2 = &lu_smd->p_verts[vi[2]];

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

        int nocull = (i < LAVA_U_PRIM_COUNT) && room_nocull(i);
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
            v3 = &lu_smd->p_verts[vi[3]];
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

        uint8_t tex_idx = (i < LAVA_U_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < LU_TEX_COUNT);
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

void lava_u_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All four room textures sit at Voff 0, and so does the sky's. The
       Ahriman's sheet does NOT (Voff 128), so it brackets itself and is told
       what to restore. */
    RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
    ahrimans_set_texwindow(&tw);
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
        underworld_sky_draw(ctx, LU_CX, LU_SKY_Y, LU_CZ);
        draw_lu_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE TWO DOOR SIGNS, any pickup a later seed puts down,
       and the AHRIMANS (area-gated inside; src/ahriman.h). */
    if (exp != DBG_EXP_NO_ENTITIES) {
        item_pickups_draw(ctx);
        draw_ahrimans(ctx);
        lu_door_text(ctx, LU_NE_X);
        lu_door_text(ctx, LU_NW_X);
    }
}
