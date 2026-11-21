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
#include "ribcage.h"
#include "ribcage_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "hell_hath_no_fury.h"  /* the volcanic ground, a narrow uploader     */
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "sharurs_trial.h"      /* the rocks, a narrow uploader               */
#include "catacombs_entry.h"    /* the catacomb inner door, a narrow uploader */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"
#include "golem.h"              /* the east door's Golem                     */
#include "nessie.h"             /* a Nessie in each lava pool                */

/* Ribcage - see ribcage.h. The room half is src/a_woman_scorned.c's (itself
   Wingscape's, Lava U's and Hell Hath No Fury's): cull keys, radius side cull
   and all. */

static SMD  *rc_smd  = NULL;
static void *rc_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[0,7800] z[-1200,2400] (the collision proxy's bounds), one floor at y=0.
   The sky is centred on the middle of it, half way up the drawn mesh (the
   tallest drawn thing, the east door's peak, reaches y=-965). */
#define RC_CX             3900
#define RC_CZ              600
#define RC_SKY_Y         (-480)

#define RC_FLOOR_Y          0
#define RC_EYE_Y         (RC_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The drawn mesh tops out at y=-965 (the east door's peak); the proxy's walls
   reach -798. */
#define RC_CEILING_Y     (-966)

/* ---- THE WEST DOOR, to Hell Hath No Fury -----------------------------------
   The wd_dr polys stand in the plane x=0, z[400,800] (collision wall 48,
   inward normal +X), so the player walks up to it from the east, down the
   west corridor (z[0,1200]). Like A Woman Scorned's south-west door it is
   DISTORTED: square to y=-400, then its top rises lopsided to a peak of -800
   at z=800. So:
     - the PRESS point is the middle of the door's foot, (0,600): a player
       held 195 off the wall by the push radius is well inside the 500
       trigger;
     - THE SIGN is 11 off the door toward the player, at y=-300 (the middle of
       its square part), on a free yaw facing the way the reader faces - 3072,
       west;
     - THE SPAWN, for an arrival through it, is 250 in along the normal, clear
       of the 195 push radius, facing east down the corridor. */
#define RC_W_X              0
#define RC_W_Z            600
#define RC_W_TEXT_X        11
#define RC_W_TEXT_Z       600
#define RC_W_TEXT_Y      (RC_FLOOR_Y - 300)
#define RC_W_TEXT_YAW    3072
#define RC_W_SPAWN_X      250
#define RC_W_SPAWN_Z      600
#define RC_W_SPAWN_ROT   1024

/* ---- THE EAST DOOR, to Lightnings Edge ------------------------------------
   The double_door polys stand in the plane x=7800, z[200,1000] (collision
   wall 25, inward normal -X), at the end of the east corridor, so the player
   walks up to it from the west. Square to y=-400, its top rising to a peak
   of -965. So the press point is the middle of its foot, (7800,600); the sign
   is 11 off it at y=-300 (the west door's height, the middle of the square
   part) on a free yaw facing east (1024); the spawn is 250 in, facing west.

   >>> THE GOLEM BLOCKS IT. <<< The Golem seeded 500 in front of this door
   (world.c, world_seed_room) is tagged GLM_BLOCKS_RIBCAGE_E_DOOR: while it
   stands, the trigger refuses and the sign is not drawn (src/golem.h,
   BLOCKING). Killing it opens the way, and the death sticks. */
#define RC_E_X           7800
#define RC_E_Z            600
#define RC_E_TEXT_X      7789
#define RC_E_TEXT_Z       600
#define RC_E_TEXT_Y      (RC_FLOOR_Y - 300)
#define RC_E_TEXT_YAW    1024
#define RC_E_SPAWN_X     7550
#define RC_E_SPAWN_Z      600
#define RC_E_SPAWN_ROT   3072

/* ---- THE NORTH DOOR, to the Lava Maze --------------------------------------
   The catacomb inner door polys in the plane z=2400, x[5800,6200] (collision
   wall 69, inward normal -Z), at the head of the north-east corridor
   (x[5400,6600] z[1400,2400]), so the player walks up to it from the south.
   Distorted like the west door - a stepped outline peaking at y=-800 - so
   the press point is the middle of its foot, (6000,2400); the sign is 11 off
   it at y=-200 (Hell Hath No Fury's height for the same door) facing north
   (yaw 0); the spawn is 250 in, clear of the push radius, facing south. */
#define RC_N_X           6000
#define RC_N_Z           2400
#define RC_N_TEXT_X      6000
#define RC_N_TEXT_Z      2389
#define RC_N_TEXT_Y      (RC_FLOOR_Y - 200)
#define RC_N_TEXT_YAW       0
#define RC_N_SPAWN_X     6000
#define RC_N_SPAWN_Z     2150
#define RC_N_SPAWN_ROT   2048

#define RC_TEXT_RADIUS    1500
#define RC_FADE_NEAR      1000
#define RC_TRIGGER_RADIUS  500

/* Circle edge-detect. Seeded "held" by the arm so a press carried in through
   the transition cannot fire on the arrival frame. */
static int w_circle_prev = 1;
static int n_circle_prev = 1;
static int e_circle_prev = 1;

/* ---- THE FLOOR -------------------------------------------------------------
   One storey: the proxy has one floor plane, y=0, wider than the walls, so
   one FLOOR_FLAT zone over the collision bounds. The walls keep the player
   out of the notches in the outline. */
static void rc_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = 0;      floor_zones[0].max_x = 7800;
    floor_zones[0].min_z = -1200;  floor_zones[0].max_z = 2400;
    floor_zones[0].y     = RC_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   SIX mesh textures, ONE owned (the double door's copy, below):
     0  volcanic     Hell Hath No Fury's, its narrow uploader (x384 y256)
     1  lava         the Descent's, its narrow uploader (x320 y256). Its UVs
                     run past 128 and wrap in the 128 window.
     2  rocks_128    Sharur's Trial's ROCKS.TIM (4bpp x512 y0) - the ribs: the
                     same 128x128 art under its Blender material name, through
                     sharurs_trial_upload_rocks().
     3  double_door  OWNED: \TEXUNDW\DBLDRUW.TIM, a byte-identical copy of the
                     Delivery Area's DBLDOOR.TIM moved to x576 y0 (same CLUT,
                     768,480). Its own address, x832 y0, is the catacomb inner
                     door's (slot 5), and this room draws both. x576 y0 is the
                     SEAL's page down here, and only Hell Hath No Fury draws
                     the seal - it re-uploads it on every entry, so taking the
                     page costs no restore.
     4  wd_dr        RESIDENT: the fat door's startup upload at x768 y256, the
                     west door - and DOOR_PANEL_WOOD's panel, the transition out
                     of here. Never take that page.
     5  catacomb inner door  the Catacombs Entry's, its narrow uploader (x832
                     y0), the north door - and DOOR_PANEL_CATACOMB's panel,
                     the transition out of here to the Lava Maze, so it MUST
                     be uploaded here (the panel draws before STATE_LOADING).
   The sky's texture is src/underworld_sky.c's business. */
#define RC_TEX_COUNT 6

static uint16_t tex_tpage[RC_TEX_COUNT];
static uint16_t tex_clut[RC_TEX_COUNT];

/* The double door's copy: the one texture this room owns. */
static int dbl_tex = -1;

void ribcage_load_assets(void) {
    /* BANK: the double door's copy is drawn here alone. Derived, not guessed
       - py tools/check_tex_banks.py. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    dbl_tex = texmgr_register("\\TEXUNDW\\DBLDRUW.TIM;1");

    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, LAVA);
    TIM_SLOT(2, ROCKS);
    TIM_SLOT(3, DBLDRUW);
    TIM_SLOT(4, WDDR);
    TIM_SLOT(5, CTCMBDR);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void ribcage_upload_textures(void) {
    hell_hath_no_fury_upload_volcanic();
    descent_to_hell_upload_lava();
    sharurs_trial_upload_rocks();
    texmgr_upload(dbl_tex);
    catacombs_entry_upload_inner_door();
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   ws_build_cull_keys() verbatim (src/wingscape.c, itself Hell Hath No Fury's;
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius). Built on the
   same call that reloads the mesh it describes, and nowhere else. */
static int     rc_key_count = 0;
static int32_t rc_y_lo = 0, rc_y_hi = 0;

static void rc_build_cull_keys(void) {
    rc_key_count = 0;
    if (!rc_smd) return;
    uint8_t *p = (uint8_t *)rc_smd->p_prims;
    int i, n = rc_smd->n_prims;
    if (n > RIBCAGE_PRIM_COUNT) n = RIBCAGE_PRIM_COUNT;
    rc_y_lo = rc_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &rc_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &rc_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < rc_y_lo) rc_y_lo = vk->vy;
            if (vk->vy > rc_y_hi) rc_y_hi = vk->vy;
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
    rc_key_count = n;
}

void ribcage_load_geometry(void) {
    rc_buff = room_arena_load("\\TEXUNDW\\RIBCAGE.SMD;1");
    rc_smd  = rc_buff ? smdInitData(rc_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (rc_smd && !room_data_bind(rc_smd->n_prims)) rc_smd = NULL;
    rc_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The door --------------------------------------------------------------*/
void ribcage_doors_arm(void) {
    w_circle_prev = interact_tapped();
    n_circle_prev = w_circle_prev;
    e_circle_prev = w_circle_prev;
}

/* The north door, to the Lava Maze: the west door's test on its own point
   and edge state. */
int ribcage_n_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !n_circle_prev;
    int32_t dx, dz;
    n_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - RC_N_X;
    dz = cam_z - RC_N_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= RC_TRIGGER_RADIUS)
        return 0;
    return interact_facing(RC_N_X, RC_N_Z);
}

static void rc_n_text(RenderContext *ctx) {
    int32_t dx = cam_x - RC_N_X;
    int32_t dz = cam_z - RC_N_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= RC_TEXT_RADIUS) return;
    if (xz > RC_FADE_NEAR) {
        int range = RC_TEXT_RADIUS - RC_FADE_NEAR;
        int prog  = xz - RC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            RC_N_TEXT_X, RC_N_TEXT_Y, RC_N_TEXT_Z,
                            50, 255, 50, fade, RC_N_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void ribcage_spawn_n(void) {
    cam_x     = RC_N_SPAWN_X;
    cam_y     = RC_EYE_Y;
    cam_vy    = 0;
    cam_z     = RC_N_SPAWN_Z;
    cam_rot   = RC_N_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    ribcage_doors_arm();
}

/* The east door, to Lightnings Edge: the north door's test on its own point
   and edge state - and REFUSED while the Golem in front of it stands. The
   edge state is still kept current while it is. */
int ribcage_e_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !e_circle_prev;
    int32_t dx, dz;
    e_circle_prev = held;
    if (lock || !just) return 0;
    if (golem_blocks(STATE_RIBCAGE, GLM_BLOCKS_RIBCAGE_E_DOOR)) return 0;
    dx = cam_x - RC_E_X;
    dz = cam_z - RC_E_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= RC_TRIGGER_RADIUS)
        return 0;
    return interact_facing(RC_E_X, RC_E_Z);
}

/* ...and its sign, which the caller does not draw while the Golem stands. */
static void rc_e_text(RenderContext *ctx) {
    int32_t dx = cam_x - RC_E_X;
    int32_t dz = cam_z - RC_E_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= RC_TEXT_RADIUS) return;
    if (xz > RC_FADE_NEAR) {
        int range = RC_TEXT_RADIUS - RC_FADE_NEAR;
        int prog  = xz - RC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            RC_E_TEXT_X, RC_E_TEXT_Y, RC_E_TEXT_Z,
                            50, 255, 50, fade, RC_E_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void ribcage_spawn_e(void) {
    cam_x     = RC_E_SPAWN_X;
    cam_y     = RC_EYE_Y;
    cam_vy    = 0;
    cam_z     = RC_E_SPAWN_Z;
    cam_rot   = RC_E_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    ribcage_doors_arm();
}

/* A fresh Circle within reach of the west door and facing it. The edge
   state is kept up to date even while locked. */
int ribcage_w_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !w_circle_prev;
    int32_t dx, dz;
    w_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - RC_W_X;
    dz = cam_z - RC_W_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= RC_TRIGGER_RADIUS)
        return 0;
    return interact_facing(RC_W_X, RC_W_Z);
}

static void rc_w_text(RenderContext *ctx) {
    int32_t dx = cam_x - RC_W_X;
    int32_t dz = cam_z - RC_W_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= RC_TEXT_RADIUS) return;
    if (xz > RC_FADE_NEAR) {
        int range = RC_TEXT_RADIUS - RC_FADE_NEAR;
        int prog  = xz - RC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            RC_W_TEXT_X, RC_W_TEXT_Y, RC_W_TEXT_Z,
                            50, 255, 50, fade, RC_W_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void ribcage_spawn_w(void) {
    cam_x     = RC_W_SPAWN_X;
    cam_y     = RC_EYE_Y;
    cam_vy    = 0;
    cam_z     = RC_W_SPAWN_Z;
    cam_rot   = RC_W_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    ribcage_doors_arm();
}

void ribcage_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(RC_CEILING_Y);
    rc_floor_zones_init();

    /* The default arrival, and where a debug jump lands: the west door, from
       Hell Hath No Fury. main.c moves it to the north door (from the Lava
       Maze) or the east door (from Lightnings Edge). */
    ribcage_spawn_w();

    /* The global prop arrays, as Wingscape clears them: Hell Hath No Fury's
       save point and tank would otherwise block invisibly inside this room's
       bounds. The clear drops the placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

/* ---- The mesh --------------------------------------------------------------
   draw_aws_smd() (src/a_woman_scorned.c) verbatim: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_rc_smd(RenderContext *ctx) {
    if (!rc_smd) return;

    uint8_t *p = (uint8_t *)rc_smd->p_prims;
    int i, n = rc_key_count;

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
#define RC_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = rc_y_lo - cam_y, dyhi = rc_y_hi - cam_y;
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
            f = f * 5 + RC_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &rc_smd->p_verts[vi[0]];
        SVECTOR *v1 = &rc_smd->p_verts[vi[1]];
        SVECTOR *v2 = &rc_smd->p_verts[vi[2]];

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

        int nocull = (i < RIBCAGE_PRIM_COUNT) && room_nocull(i);
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
            v3 = &rc_smd->p_verts[vi[3]];
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

        uint8_t tex_idx = (i < RIBCAGE_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < RC_TEX_COUNT);
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

void ribcage_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All six room textures and the sky's sit at Voff 0. The two enemy
       sheets do NOT (Voff 128), so each brackets itself and is told what to
       restore. */
    RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
    golems_set_texwindow(&tw);
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
        underworld_sky_draw(ctx, RC_CX, RC_SKY_Y, RC_CZ);
        draw_rc_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE ENEMIES - the east door's Golem and four Nessies
       with their fireballs (both area-gated inside) - the THREE DOORS' SIGNS
       (the east one only once its Golem is dead) and any pickup a later seed
       puts down. No props. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        item_pickups_draw(ctx);
        draw_golems(ctx);
        draw_nessies(ctx);
        rc_w_text(ctx);
        rc_n_text(ctx);
        if (!golem_blocks(STATE_RIBCAGE, GLM_BLOCKS_RIBCAGE_E_DOOR))
            rc_e_text(ctx);
    }
}
