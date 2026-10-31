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
#include "hell_hath_no_fury.h"
#include "hell_hath_no_fury_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "catacombs_entry.h"    /* cobble + the inner door, narrow uploaders  */
#include "seal.h"               /* the Seal's floor art                       */
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"               /* the tank's and the south-west door's signs */

/* Hell Hath No Fury - see hell_hath_no_fury.h. The room half is
   src/master_bedroom.c's, which has been through DIAGNOSING_FRAME_RATE.txt,
   with the chapter's fog and sky (src/underworld.h) in place of its own. */

static SMD  *hhnf_smd  = NULL;
static void *hhnf_buff = NULL;

/* ---- The platform ----------------------------------------------------------
   x[0,4200) z[0,4200) (the collision proxy's bounds), floor at y=0, and the
   sky centred on the middle of it, half way up the ruin. */
#define HHNF_CX           2100
#define HHNF_CZ           2100
#define HHNF_SKY_Y       (-300)

#define HHNF_FLOOR_Y         0
#define HHNF_EYE_Y        (HHNF_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The ruin is the tallest thing here and its proxy walls reach y=-600. */
#define HHNF_CEILING_Y    (-600)

/* THE CENTRE, on the seal, facing north (+Z): out across the platform to the
   two north doors with the ruin behind the player's right shoulder. */
#define HHNF_SPAWN_ROT       0

/* ---- THE SAVE POINT --------------------------------------------------------
   At the NORTHERN TIP: the middle of the north rim (wall 1, z=4200, which runs
   x[600,3600]), 200 in from it - the Neck's corner standoff. The model's own
   footprint is 70 plus the 55 standoff save_points_collide is called with, so
   the player stops 125 short of it. y is the floor (0) less 300 and rot/scale
   are reception's, as everywhere. */
#define HHNF_SAVE_X          2100
#define HHNF_SAVE_Y          (HHNF_FLOOR_Y - 300)
#define HHNF_SAVE_Z          4000

/* ---- THE OIL DISPENSER -----------------------------------------------------
   OD_TANK_HHNF, half full to start (src/oil_dispenser.c's od_tank_start[]).
   In the INSIDE CORNER of the ruin's L, where its east arm's west face (wall
   10, x=3199) meets its south arm's north face (wall 13, z=1000): a corner
   whose walls are +X and -Z, i.e. exactly the Neck's south-east corner, so it
   takes that tank's terms - origin 2 in from both, rot 0, which points the
   spout back up the (-X,+Z) diagonal at the seal. The ruin is 600 tall, so the
   tank (170 to 280 up) hangs well inside its face.

   THE SIGN is the Neck's, moved here: 78 out along each axis at half size, its
   glyph top 150 above the floor, under the tank.

   ITS PRESS goes after the save point's in main.c, vetoed by it. The save
   point is 3195 north of it, so they never share a circle; the order is what
   guarantees it. */
#define HHNF_OD_X            3197
#define HHNF_OD_Z            1002
#define HHNF_OD_TEXT_X       3119     /* 78 out along the (-X,+Z) diagonal */
#define HHNF_OD_TEXT_Z       1080
#define HHNF_OD_TEXT_Y       (HHNF_FLOOR_Y - 150)  /* glyph TOP, under the tank */
#define HHNF_OD_TEXT_YAW     1536     /* 135 deg: face back north-west     */
#define HHNF_OD_TEXT_PIXEL      2     /* half DOOR_PIXEL_SIZE, the Neck's  */
#define HHNF_TEXT_RADIUS     1200
#define HHNF_FADE_NEAR        800
#define HHNF_TRIGGER_RADIUS   500

/* Circle edge-detect for the tank. Seeded "held" by the init so a press
   carried in through the transition cannot fire on the arrival frame. */
static int oil_circle_prev = 1;

/* ---- THE SOUTH-WEST DOOR, to Sharur's Trial --------------------------------
   The catacomb inner door polys span (400,200)-(200,400): centre (300,300), on
   the cut corner x+z=600 (collision wall 4, inward normal (+0.707,+0.707)), so
   the player walks up to it from the north-east. The other three doors still
   lead nowhere and have no prompt.

   THE SIGN is on a free yaw facing the way the reader faces - 2560, south-west
   - 11 off the door toward the player, at the doorway's mid-height (the door
   is 400 tall here). THE SPAWN, for an arrival back through it, is 250 in
   along the normal (177 on each axis), clear of the 195 push radius, facing
   north-east out across the platform. */
#define HHNF_SW_X            300
#define HHNF_SW_Z            300
#define HHNF_SW_TEXT_X       308
#define HHNF_SW_TEXT_Z       308
#define HHNF_SW_TEXT_Y       (HHNF_FLOOR_Y - 200)
#define HHNF_SW_TEXT_YAW     2560
#define HHNF_SW_SPAWN_X      477
#define HHNF_SW_SPAWN_Z      477
#define HHNF_SW_SPAWN_ROT    512

static int sw_circle_prev = 1;

static void hhnf_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = 0;  floor_zones[0].max_x = 4200;
    floor_zones[0].min_z = 0;  floor_zones[0].max_z = 4200;
    floor_zones[0].y     = HHNF_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   FIVE mesh textures, ONE owned:
     0  volcanic             OWNED. 4bpp x384 y256, CLUT (928,502): the start
                             of a page the Catacombs map calls FREE, i.e. every
                             tenant is re-uploaded by its own room on entry.
     1  cobblestones         the Catacombs Entry's, its narrow uploader
     2  catacomb inner door  the Catacombs Entry's, its narrow uploader
     3  wd_dr                RESIDENT: the fat door's startup upload at x768
                             y256, which nothing in Chapter 3 or 4 overwrites
                             (VRAM_MAP_CATACOMBS.txt: "needs a way back first").
                             A future Underworld room that takes that page owes
                             this room a restore.
     4  seal                 the Seal's, its uploader (pixels and its CLUT line,
                             Lamashtu's, at 640,502)
     5  lava                 the Descent's, its narrow uploader: x320 y256, CLUT
                             (944,502). Its UVs run past 128 and wrap in the
                             room's 128 window, so it must stay a page start.
   The sky's texture is src/underworld_sky.c's business. */
#define HHNF_TEX_COUNT 6

static uint16_t tex_tpage[HHNF_TEX_COUNT];
static uint16_t tex_clut[HHNF_TEX_COUNT];
static int      volcanic_tex = -1;

void hell_hath_no_fury_load_assets(void) {
    /* BANK: Chapter 4. Derived, not guessed - py tools/check_tex_banks.py walks
       the uploader call graph and fails the build if this is short. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    volcanic_tex = texmgr_register("\\TEXUNDW\\VOLCANIC.TIM;1");
    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, COBBLE);
    TIM_SLOT(2, CTCMBDR);
    TIM_SLOT(3, WDDR);
    TIM_SLOT(4, SEAL);
    TIM_SLOT(5, LAVA);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void hell_hath_no_fury_upload_textures(void) {
    hell_hath_no_fury_upload_volcanic();
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    seal_upload_textures();
    descent_to_hell_upload_lava();
    underworld_sky_upload_textures();
    /* The tank in the ruin: x896 y256, CLUT (672,504), over stn_gls exactly
       as in Chapter 3, which the player has already passed through. */
    oil_dispenser_upload_texture();
}

/* The volcanic rock alone, for the chapter's other rooms (Sharur's Trial's
   paths) - a second registration would hold a second RAM copy of it. */
void hell_hath_no_fury_upload_volcanic(void) {
    if (volcanic_tex >= 0) texmgr_upload(volcanic_tex);
}

/* ---- The cull key, and its RADIUS (tools/DIAGNOSING_FRAME_RATE.txt STEP 3I) -
   Built on the same call that reloads the mesh it describes, and nowhere else.

   >>> THIS ROOM HAS A SIDE-PLANE CULL, BECAUSE A METER ASKED, AND IT IS NOT
   THE GREENHOUSE'S. <<< Measured (Oct 2026, force-booted, 12 poses): facing
   the seal from anywhere on the rim D read 257-273 and the room ran at 30fps,
   ~460 primitives a frame through the GTE and ~180 of them on screen - the
   distance cull passes nearly the whole platform (3500 against 4200) and the
   behind test was the only heading test the loop had.

   THE GREENHOUSE'S BOX TEST WAS TRIED FIRST AND MEASURED A LOSS: it cut the GTE
   load 480 -> 187 and the mesh's time went 179 -> 194. Four corners at four
   CPU multiplies each cost more than the transforms they saved - the GTE is
   cheap, the R3000's multiplier is not. What went in instead is ONE point and
   ONE radius: the key's own vertex, whose forward term the behind test has
   already paid for, plus two multiplies for its side term, and the radius in
   the key's spare byte (pad = ceil(R/4), 255 = too big, never side-culled),
   so nothing extra is read. It culls less than the box (55% of what reaches
   it against 69%) and is the one that is faster: mesh 186-206 -> 152-169,
   every measured pose back to 60fps. No cull_boxes.

   R is the box's half-perimeter-plus: (max_x-min_x) + (max_z-min_z), which is
   at least the distance from v0 to any vertex. hhnf_y_lo/hi are the mesh's
   height range, for the pitch term in the test (see draw_hhnf_smd). */
static int     hhnf_key_count = 0;
static int32_t hhnf_y_lo = 0, hhnf_y_hi = 0;

static void hhnf_build_cull_keys(void) {
    hhnf_key_count = 0;
    if (!hhnf_smd) return;
    uint8_t *p = (uint8_t *)hhnf_smd->p_prims;
    int i, n = hhnf_smd->n_prims;
    if (n > HELL_HATH_NO_FURY_PRIM_COUNT) n = HELL_HATH_NO_FURY_PRIM_COUNT;
    hhnf_y_lo = hhnf_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &hhnf_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &hhnf_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < hhnf_y_lo) hhnf_y_lo = vk->vy;
            if (vk->vy > hhnf_y_hi) hhnf_y_hi = vk->vy;
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
    hhnf_key_count = n;
}

void hell_hath_no_fury_load_geometry(void) {
    hhnf_buff = room_arena_load("\\TEXUNDW\\HHNF.SMD;1");
    hhnf_smd  = hhnf_buff ? smdInitData(hhnf_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (hhnf_smd && !room_data_bind(hhnf_smd->n_prims)) hhnf_smd = NULL;
    hhnf_build_cull_keys();
    /* The sky: its own small model, kept across Chapter 4's rooms and handed
       back by load_area_geometry on the way into any other. */
    underworld_sky_load();
}

/* ---- The arrival out of white ----------------------------------------------
   The Descent's white held, the room built under it, and then 2 s up out of
   it with the player rooted: turn, but no walking, and no HUD. */
#define HHNF_T_FADE   120

static int32_t fade_t = HHNF_T_FADE;   /* HHNF_T_FADE = not fading */

/* ---- The opening drop ------------------------------------------------------
   Under the fade, and on long after it: the camera hangs straight over the
   centre of the seal, looking straight down (the Descent's pitch, yaw north),
   and comes down onto it, slowing to nothing as it reaches the player's eye.

   THE HEIGHT. The fog is measured in Manhattan XZ, so an overhead camera reads
   as close however high it is. For the drop only, draw_hhnf_smd adds the
   camera's rise above the eye to that distance - the chapter's 800/3500 are
   untouched, only where they are measured from changes (src/underworld.h) -
   and the drop starts 3300 up: the seal's centre is then 200 inside the cull
   line, a few percent out of the fog, and the floor round it further in.

   THE SPEED. 9 s, by request (an even deceleration from the Descent's speed
   took 13.7). It starts at the Descent's one steady speed (5950 in 740
   frames, 8.04 a frame) and slows to zero, LATE rather than evenly: speed =
   v0 * (1 - tau^3), tau = t / T, which averages 3/4 of v0, so the 3300 takes
   T = 3300 / (0.75 * v0) = 540 frames at v0 = 8.15 - the Descent's to 1.4%.
   Integrated: rise = R * (1 - (4 tau - tau^4) / 3).

   THE TURN. In the last HHNF_T_TURN frames - from 200 above the eye, just
   short of the player's height - the pitch eases from straight down to level
   (smoothstep), the yaw held north throughout, so the drop lands on the
   ground facing north and hands the player control on that frame.

   >>> NO BEHIND-THE-YAW CULL DURING THE DROP. <<< draw_hhnf_smd's 700-unit
   test ignores pitch; looking straight down from 3300 the screen reaches
   ~1550 south of the camera, so it would cut a straight edge across the seal
   (memory: overhead-camera-behind-cull). The whole platform is cheap. */
#define HHNF_DROP_RISE   3300
#define HHNF_T_DROP       540
#define HHNF_T_TURN       100
#define HHNF_PITCH_DOWN  1024   /* the Descent's DH_PITCH_DOWN */

static int32_t drop_t = HHNF_T_DROP;   /* HHNF_T_DROP = not dropping */

/* The ends are early returns, not clamps (memory: clamp-before-divide). */
static void hhnf_place_camera(void) {
    if (drop_t >= HHNF_T_DROP) return;
    int32_t s = HHNF_T_DROP - drop_t;              /* frames left, 1..T */
    int32_t u  = (drop_t << 12) / HHNF_T_DROP;     /* tau, 0..4096 */
    int32_t u2 = (u * u) >> 12;
    int32_t u4 = (u2 * u2) >> 12;
    int32_t r  = HHNF_DROP_RISE -
                 ((HHNF_DROP_RISE * ((4 * u - u4) / 3)) >> 12);
    cam_x   = HHNF_CX;
    cam_z   = HHNF_CZ;
    cam_y   = HHNF_EYE_Y - r;
    cam_vy  = 0;
    cam_rot = HHNF_SPAWN_ROT;
    if (s >= HHNF_T_TURN) {
        cam_pitch = HHNF_PITCH_DOWN;
    } else {
        int32_t w = (s << 8) / HHNF_T_TURN;        /* 256 -> 0 */
        cam_pitch = (w * w * (768 - 2 * w)) >> 14; /* 1024 * smoothstep */
    }
}

/* On the ground, facing north: the spawn, and the player's from here. */
static void hhnf_land(void) {
    cam_x     = HHNF_CX;
    cam_z     = HHNF_CZ;
    cam_y     = HHNF_EYE_Y;
    cam_vy    = 0;
    cam_rot   = HHNF_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
}

void hell_hath_no_fury_from_white(void) {
    fade_t = 0;
    drop_t = 0;
    hhnf_place_camera();   /* the first frame drawn is the top of the drop */
}

int hell_hath_no_fury_fading(void) { return fade_t < HHNF_T_FADE; }

int hell_hath_no_fury_intro(void) { return drop_t < HHNF_T_DROP; }

void hell_hath_no_fury_reset(void) {
    fade_t = HHNF_T_FADE;
    drop_t = HHNF_T_DROP;
}

void hell_hath_no_fury_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(HHNF_CEILING_Y);
    hhnf_floor_zones_init();

    cam_x     = HHNF_CX;
    cam_y     = HHNF_EYE_Y;
    cam_vy    = 0;
    cam_z     = HHNF_CZ;
    cam_rot   = HHNF_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();

    /* No fade or drop unless main.c asks for one (the Descent's hand-off). */
    fade_t = HHNF_T_FADE;
    drop_t = HHNF_T_DROP;

    /* The global prop arrays, as the Seal clears them: an instance left from
       another room would block invisibly inside x[0,4200) z[0,4200). THEN
       this room's own save point and tank (see above); the clear drops the
       placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    save_point_add(HHNF_SAVE_X, HHNF_SAVE_Y, HHNF_SAVE_Z, 512, 2048);
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    oil_dispenser_place(STATE_HELL_HATH_NO_FURY, OD_TANK_HHNF,
                        HHNF_OD_X, HHNF_FLOOR_Y - GROUND_FLOOR_Y, HHNF_OD_Z, 0);
    bars_clear();
    reset_particles();

    oil_circle_prev = interact_tapped();
    sw_circle_prev  = oil_circle_prev;
    save_point_arm();
}

/* Back in through the south-west door, out of Sharur's Trial. */
void hell_hath_no_fury_spawn_sw(void) {
    cam_x     = HHNF_SW_SPAWN_X;
    cam_y     = HHNF_EYE_Y;
    cam_vy    = 0;
    cam_z     = HHNF_SW_SPAWN_Z;
    cam_rot   = HHNF_SW_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    sw_circle_prev = interact_tapped();
}

/* The south-west door's press: a fresh Circle within reach and facing it. The
   edge state is kept up to date while locked, as the tank's is. */
int hell_hath_no_fury_sw_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !sw_circle_prev;
    int32_t dx, dz;
    sw_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - HHNF_SW_X;
    dz = cam_z - HHNF_SW_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= HHNF_TRIGGER_RADIUS)
        return 0;
    return interact_facing(HHNF_SW_X, HHNF_SW_Z);
}

static void hhnf_sw_text(RenderContext *ctx) {
    int32_t dx = cam_x - HHNF_SW_X;
    int32_t dz = cam_z - HHNF_SW_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= HHNF_TEXT_RADIUS) return;
    if (xz > HHNF_FADE_NEAR) {
        int range = HHNF_TEXT_RADIUS - HHNF_FADE_NEAR;
        int prog  = xz - HHNF_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            HHNF_SW_TEXT_X, HHNF_SW_TEXT_Y, HHNF_SW_TEXT_Z,
                            50, 255, 50, fade, HHNF_SW_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

/* The tank's tap: the Neck's edge/range/facing test about the corner, then
   the pour and its log line (src/oil_dispenser.c owns both). THE EDGE STATE
   IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a menu
   closing does not read as a fresh press when the lock lifts. */
int hell_hath_no_fury_oil_update(int lock) {
    int held = interact_tapped();
    int just = held && !oil_circle_prev;
    int32_t dx, dz;
    oil_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - HHNF_OD_X;
    dz = cam_z - HHNF_OD_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= HHNF_TRIGGER_RADIUS)
        return 0;
    if (!interact_facing(HHNF_OD_X, HHNF_OD_Z)) return 0;
    oil_dispenser_press(OD_TANK_HHNF);
    return 1;
}

/* The tank's sign, on a free yaw: the range and fade are measured from the
   tank and the glyphs laid out about the sign point - the Neck's
   nk_oil_text(). */
static void hhnf_oil_text(RenderContext *ctx) {
    int32_t dx = cam_x - HHNF_OD_X;
    int32_t dz = cam_z - HHNF_OD_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= HHNF_TEXT_RADIUS) return;
    if (xz > HHNF_FADE_NEAR) {
        int range = HHNF_TEXT_RADIUS - HHNF_FADE_NEAR;
        int prog  = xz - HHNF_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to refill",
                            HHNF_OD_TEXT_X, HHNF_OD_TEXT_Y, HHNF_OD_TEXT_Z,
                            50, 255, 50, fade, HHNF_OD_TEXT_YAW,
                            HHNF_OD_TEXT_PIXEL);
}

void hell_hath_no_fury_update(int lock) {
    (void)lock;
    if (fade_t < HHNF_T_FADE) fade_t++;
    if (drop_t < HHNF_T_DROP) {
        drop_t++;
        if (drop_t >= HHNF_T_DROP) hhnf_land();
        else                       hhnf_place_camera();
    }
}

/* ---- The mesh --------------------------------------------------------------
   src/master_bedroom.c's loop, with the chapter's fog: one distance, set in
   src/underworld.h and changed by nothing. Far primitives clamp one OT slot in
   front of the sky so none is ever drawn under it. */
static void draw_hhnf_smd(RenderContext *ctx) {
    if (!hhnf_smd) return;

    uint8_t *p = (uint8_t *)hhnf_smd->p_prims;
    int i, n = hhnf_key_count;

    /* HOISTED OUT OF THE LOOP (STEP 3C fix 1): none of these can change while
       a frame is being queued. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = UW_FOG_FAR;
    int32_t fog_start = UW_FOG_NEAR < cull ? UW_FOG_NEAR : cull / 2;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    /* THE OPENING DROP: its height counts toward the fog and the cull, and
       the behind test is off (see "The opening drop"). 0 / on otherwise. */
    int     dropping = hell_hath_no_fury_intro();
    int32_t rise     = dropping ? HHNF_EYE_Y - cam_y : 0;
    if (rise < 0) rise = 0;

    /* ---- THE SIDE-PLANE CULL'S PER-FRAME TERMS ------------------------------
       The side planes of the Greenhouse's test (src/greenhouse.c, "SIDE-PLANE
       FRUSTUM CULL") - 8*side > 5*fwd is outside the right plane on a 320-wide
       screen at gte_SetGeomScreen(256) - taken from one point and a radius
       rather than a box (see hhnf_build_cull_keys for why), and with ONE
       addition: THE CAMERA HERE IS NOT ALWAYS LEVEL. The look button tilts it
       up to LOOK_MAX_ANG and the opening drop points it straight down, and the
       Greenhouse's planes are vertical only at pitch 0.

       Pitch turns about the view's own X axis, so a point's view X (sd below) is
       unchanged and only its DEPTH moves: depth = f*cos(p) + dy*sin(p), dy its
       height below the eye. A point is outside the right plane when 8*sd >
       5*depth. Bounding depth from ABOVE keeps that a proof: dy*sin(p) <=
       |sin(p)| * max|dy| over the mesh's height range, a per-frame constant.
       Sign-agnostic, so it holds whichever way pitch is measured.

       THE RADIUS. What is left, 8*sd - 5*cos(p)*f, changes by at most
       sqrt(64 + 25) < 10 per world unit moved in (x,z), so every vertex within
       R of the key point is outside the plane if the key point is by more than
       10*R. pad*4 >= R, hence pad*40.

       ARITHMETIC. f and sd are <<12. f*cos(p) is formed as ((f>>8)*pc)>>4 to
       stay inside an int32; both shifts floor, i.e. UNDER-estimate depth, by
       under 272 in the <<12 domain, so +512 restores the upper bound. The
       |sin| term is exact. Worst case: f ~27M, the slack ~15M (the drop's 3500
       of dy), x5 -> ~215M, plus the radius term's ~42M, inside an int32.
       cos(p) >= 0 for every pitch the game uses (-228..1024); a negative one
       switches the test off rather than inverting it. LEVEL 5 switches it off
       for the A/B.

       HHNF_SIDE_MARGIN is one world unit of view X. Swept offline over the
       platform at 32 headings and seven pitch/height pairs (the drop's
       included): without it, two vertices at pitch 120 sat 0.003 px inside the
       screen edge against exact trig - isin/icos's rounding, not the bound -
       and with it, zero holes. */
#define HHNF_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = hhnf_y_lo - cam_y, dyhi = hhnf_y_hi - cam_y;
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
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) + rise > cull)
                { p += stride; continue; }
            if (!dropping && dx * sn + dz * cs < -(700 << 12))
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
            f = f * 5 + HHNF_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &hhnf_smd->p_verts[vi[0]];
        SVECTOR *v1 = &hhnf_smd->p_verts[vi[1]];
        SVECTOR *v2 = &hhnf_smd->p_verts[vi[2]];

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

        int nocull = (i < HELL_HATH_NO_FURY_PRIM_COUNT) && room_nocull(i);
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
            v3 = &hhnf_smd->p_verts[vi[3]];
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
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) + rise;
        int32_t fog = dist < fog_start ? fog_start : (dist > cull ? cull : dist);
        int32_t fog_factor = ((cull - fog) << 8) / (cull - fog_start);

        uint8_t tex_idx = (i < HELL_HATH_NO_FURY_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < HHNF_TEX_COUNT);
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

/* ---- The white -------------------------------------------------------------
   src/descent_to_hell.c's draw_white: an ADDITIVE full-screen tile in front of
   the scene, 255 on the first frame (the Descent's white, unbroken) down to 0. */
#define HHNF_OT_WHITE  2

static void draw_white(RenderContext *ctx) {
    if (fade_t >= HHNF_T_FADE) return;
    int32_t k = 255 - (fade_t * 255) / HHNF_T_FADE;
    if (k <= 0) return;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) + sizeof(DR_TPAGE) > buf_end) return;

    TILE *tl = (TILE *)ctx->next_packet;
    setTile(tl);
    setSemiTrans(tl, 1);
    setRGB0(tl, (uint8_t)k, (uint8_t)k, (uint8_t)k);
    setXY0(tl, 0, 0);
    setWH(tl, SCREEN_XRES, SCREEN_YRES);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[HHNF_OT_WHITE], tl);
    ctx->next_packet += sizeof(TILE);

    /* After the tile in the same slot: LIFO, so the blend mode is set first. */
    DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(dp, 0, 0, getTPage(0, 1 /* ABR=1: B + F */, 0, 0));
    addPrim(&ctx->buffers[ctx->active_buffer].ot[HHNF_OT_WHITE], dp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

void hell_hath_no_fury_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All six room textures sit at Voff 0, and so does the sky's, which draws
       under this same window. */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
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
        underworld_sky_draw(ctx, HHNF_CX, HHNF_SKY_Y, HHNF_CZ);
        draw_hhnf_smd(ctx);
    }

    /* The tank, a fixture of the ruin as the Neck's is of its hall, so not
       under the LEVEL 8 switch. It NEEDS the 128 window set above: its UVs run
       past one tile and only land on the art because the window wraps them. */
    oil_dispensers_draw(ctx);

    /* >>> LEVEL 8 IS THE SAVE POINT, THE TANK'S SIGN AND BAR, THE SOUTH-WEST
       DOOR'S SIGN AND ANY PICKUP a later seed puts down. No enemies. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        save_points_draw(ctx);
        hhnf_oil_text(ctx);
        /* The tank's level, on the sign's own fade curve. */
        oil_dispensers_draw_bar(ctx, HHNF_TEXT_RADIUS, HHNF_FADE_NEAR);
        item_pickups_draw(ctx);
        /* Not while the white is up or the drop runs: the arrival is the
           Descent's, not a door's. */
        if (!hell_hath_no_fury_intro() && !hell_hath_no_fury_fading())
            hhnf_sw_text(ctx);
    }

    draw_white(ctx);
}
