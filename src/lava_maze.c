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
#include "lava_maze.h"
#include "lava_maze_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "catacombs_entry.h"    /* cobble + the inner door, narrow uploaders  */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"
#include "column.h"             /* the middle pool's living column           */

/* Lava Maze - see lava_maze.h. The room half is src/a_woman_scorned.c's
   (itself Wingscape's, Lava U's and Hell Hath No Fury's): cull keys, radius
   side cull and all. */

static SMD  *lm_smd  = NULL;
static void *lm_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[0,7800] z[-2400,4200] (the collision proxy's bounds), one floor at y=0:
   the cobblestone path and the lava share the plane. The sky is centred on
   the middle of it, half way up the rim (the tallest drawn thing, the rim and
   the west door, reaches y=-600). */
#define LM_CX             3900
#define LM_CZ              900
#define LM_SKY_Y         (-300)

#define LM_FLOOR_Y          0
#define LM_EYE_Y         (LM_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The drawn mesh tops out at y=-600; the proxy's rim walls reach -599. */
#define LM_CEILING_Y     (-601)

/* ---- THE WEST DOOR, to Ribcage ---------------------------------------------
   The gaol door polys stand in the plane x=0 (collision wall 40, inward
   normal +X), so the player walks up to it from the east, along the path
   that leaves it (z[0,600] at the west rim). Like A Woman Scorned's
   south-west door it is DISTORTED: it LEANS north as it rises - z[0,710] at
   the floor, z[110,820] at y=-300, z[220,1040] at the top (y=-600). So:
     - the PRESS point is the middle of the door's foot, (0,355): a player
       held 195 off the wall by the push radius is well inside the 500
       trigger;
     - THE SIGN is 11 off the door toward the player at y=-300, the middle of
       its face there (z 465), on a free yaw facing the way the reader faces
       - 3072, west;
     - THE SPAWN, for an arrival through it, is 250 in along the normal, clear
       of the 195 push radius and on the path, facing east. */
#define LM_W_X              0
#define LM_W_Z            355
#define LM_W_TEXT_X        11
#define LM_W_TEXT_Z       465
#define LM_W_TEXT_Y      (LM_FLOOR_Y - 300)
#define LM_W_TEXT_YAW    3072
#define LM_W_SPAWN_X      250
#define LM_W_SPAWN_Z      355
#define LM_W_SPAWN_ROT   1024

/* ---- THE NORTH DOOR, to A Woman Scorned ------------------------------------
   The catacomb inner door polys stand in the plane z=4200, x[2800,3200]
   (collision wall 38, inward normal -Z), square and 300 tall, at the north
   end of the path. So the press point is the middle of its foot, the sign 11
   off it at y=-200 (Hell Hath No Fury's height for the same door) facing
   north (yaw 0), and the spawn 250 in, facing south. */
#define LM_N_X           3000
#define LM_N_Z           4200
#define LM_N_TEXT_X      3000
#define LM_N_TEXT_Z      4189
#define LM_N_TEXT_Y      (LM_FLOOR_Y - 200)
#define LM_N_TEXT_YAW       0
#define LM_N_SPAWN_X     3000
#define LM_N_SPAWN_Z     3950
#define LM_N_SPAWN_ROT   2048

/* ---- THE EAST DOOR ---------------------------------------------------------
   The wd_dr polys round the corner at (7800,-1800), in the planes z=-1800
   and x=7800. It leads nowhere yet: no prompt and no trigger until its room
   exists. */

#define LM_TEXT_RADIUS    1500
#define LM_FADE_NEAR      1000
#define LM_TRIGGER_RADIUS  500

/* Circle edge-detect, one per door. Seeded "held" by the arm so a press
   carried in through the transition cannot fire on the arrival frame. */
static int w_circle_prev = 1;
static int n_circle_prev = 1;

/* ---- THE FLOOR -------------------------------------------------------------
   One storey: the proxy has one floor plane, y=0, wider than the walls, so
   one FLOOR_FLAT zone over the collision bounds. The maze is not walls: it is
   the lava either side of the path, which burns (lava_update, its ROOMS line
   in tools/gen_lava_floors.py). */
static void lm_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = 0;      floor_zones[0].max_x = 7800;
    floor_zones[0].min_z = -2400;  floor_zones[0].max_z = 4200;
    floor_zones[0].y     = LM_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   FIVE mesh textures, ONE owned (the gaol door's copy, below):
     0  cobblestones the Catacombs Entry's, its narrow uploader (x384 y0)
     1  lava         the Descent's, its narrow uploader (x320 y256). Its UVs
                     run past 128 and wrap in the 128 window.
     2  wd_dr        RESIDENT: the fat door's startup upload at x768 y256, the
                     east door. Never take that page.
     3  catacomb inner door  the Catacombs Entry's, its narrow uploader (x832
                     y0), the north door - and DOOR_PANEL_CATACOMB's panel,
                     the transition out of here to A Woman Scorned, so it MUST
                     be uploaded here (the panel draws before STATE_LOADING).
     4  gaol door    OWNED: \TEXUNDW\GAOLDRUW.TIM, a byte-identical copy of the
                     Gaol Entry's GAOLDOOR.TIM moved to x576 y0 with the seal's
                     CLUT (640,502). The original's page, x640 y0, AND its CLUT
                     line, (576,502), are the sky's hell bg in every Chapter 4
                     room. x576 y0 is the SEAL's page down here (and Ribcage's
                     double door copy's); Hell Hath No Fury and Ribcage
                     re-upload theirs on every entry, so taking it costs no
                     restore. It is also DOOR_PANEL_GAOL_UW's panel, the
                     transition out of here to Ribcage - uploaded here for the
                     same reason as the inner door.
   The sky's texture is src/underworld_sky.c's business. */
#define LM_TEX_COUNT 5

static uint16_t tex_tpage[LM_TEX_COUNT];
static uint16_t tex_clut[LM_TEX_COUNT];

/* The gaol door's copy: the one texture this room owns. */
static int gaol_tex = -1;

void lava_maze_load_assets(void) {
    /* BANK: the gaol door's copy is drawn here alone (and by the panel out of
       here). Derived, not guessed - py tools/check_tex_banks.py. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    gaol_tex = texmgr_register("\\TEXUNDW\\GAOLDRUW.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, LAVA);
    TIM_SLOT(2, WDDR);
    TIM_SLOT(3, CTCMBDR);
    TIM_SLOT(4, GAOLDRUW);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void lava_maze_upload_textures(void) {
    catacombs_entry_upload_cobble();
    descent_to_hell_upload_lava();
    catacombs_entry_upload_inner_door();
    texmgr_upload(gaol_tex);
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   aws_build_cull_keys() verbatim (src/a_woman_scorned.c, itself Hell Hath No Fury's;
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius). Built on the
   same call that reloads the mesh it describes, and nowhere else. */
static int     lm_key_count = 0;
static int32_t lm_y_lo = 0, lm_y_hi = 0;

static void lm_build_cull_keys(void) {
    lm_key_count = 0;
    if (!lm_smd) return;
    uint8_t *p = (uint8_t *)lm_smd->p_prims;
    int i, n = lm_smd->n_prims;
    if (n > LAVA_MAZE_PRIM_COUNT) n = LAVA_MAZE_PRIM_COUNT;
    lm_y_lo = lm_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &lm_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &lm_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < lm_y_lo) lm_y_lo = vk->vy;
            if (vk->vy > lm_y_hi) lm_y_hi = vk->vy;
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
    lm_key_count = n;
}

void lava_maze_load_geometry(void) {
    lm_buff = room_arena_load("\\TEXUNDW\\LAVAMAZE.SMD;1");
    lm_smd  = lm_buff ? smdInitData(lm_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (lm_smd && !room_data_bind(lm_smd->n_prims)) lm_smd = NULL;
    lm_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The doors -------------------------------------------------------------*/
void lava_maze_doors_arm(void) {
    int held = interact_tapped();
    w_circle_prev = held;
    n_circle_prev = held;
}

/* The shared body: a fresh Circle within reach of (door_x, door_z) and facing
   it. The edge state is kept up to date even while locked. */
static int lm_door_triggered(int lock, int32_t door_x, int32_t door_z,
                             int *circle_prev) {
    int held = interact_tapped();
    int just = held && !*circle_prev;
    int32_t dx, dz;
    *circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - door_x;
    dz = cam_z - door_z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= LM_TRIGGER_RADIUS)
        return 0;
    return interact_facing(door_x, door_z);
}

int lava_maze_w_door_triggered(int lock) {
    return lm_door_triggered(lock, LM_W_X, LM_W_Z, &w_circle_prev);
}

int lava_maze_n_door_triggered(int lock) {
    return lm_door_triggered(lock, LM_N_X, LM_N_Z, &n_circle_prev);
}

static void lm_door_text(RenderContext *ctx, int32_t door_x, int32_t door_z,
                         int32_t tx, int32_t ty, int32_t tz, int yaw) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - door_z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= LM_TEXT_RADIUS) return;
    if (xz > LM_FADE_NEAR) {
        int range = LM_TEXT_RADIUS - LM_FADE_NEAR;
        int prog  = xz - LM_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            tx, ty, tz, 50, 255, 50, fade, yaw,
                            DOOR_PIXEL_SIZE);
}

static void lm_spawn_at(int32_t x, int32_t z, int rot) {
    cam_x     = x;
    cam_y     = LM_EYE_Y;
    cam_vy    = 0;
    cam_z     = z;
    cam_rot   = rot;
    cam_pitch = 0;
    camera_look_cancel();
    lava_maze_doors_arm();
}

void lava_maze_spawn_w(void) {
    lm_spawn_at(LM_W_SPAWN_X, LM_W_SPAWN_Z, LM_W_SPAWN_ROT);
}

void lava_maze_spawn_n(void) {
    lm_spawn_at(LM_N_SPAWN_X, LM_N_SPAWN_Z, LM_N_SPAWN_ROT);
}

void lava_maze_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(LM_CEILING_Y);
    lm_floor_zones_init();

    /* The default arrival, and where a debug jump lands: the west door, from
       Ribcage. main.c moves it to the north door for an arrival from A Woman
       Scorned. */
    lava_maze_spawn_w();

    /* The global prop arrays, as Ribcage clears them: Hell Hath No Fury's
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
static void draw_lm_smd(RenderContext *ctx) {
    if (!lm_smd) return;

    uint8_t *p = (uint8_t *)lm_smd->p_prims;
    int i, n = lm_key_count;

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
#define LM_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = lm_y_lo - cam_y, dyhi = lm_y_hi - cam_y;
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
            f = f * 5 + LM_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &lm_smd->p_verts[vi[0]];
        SVECTOR *v1 = &lm_smd->p_verts[vi[1]];
        SVECTOR *v2 = &lm_smd->p_verts[vi[2]];

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

        int nocull = (i < LAVA_MAZE_PRIM_COUNT) && room_nocull(i);
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
            v3 = &lm_smd->p_verts[vi[3]];
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

        uint8_t tex_idx = (i < LAVA_MAZE_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < LM_TEX_COUNT);
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

void lava_maze_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All five room textures and the sky's sit at Voff 0. The Column's sheet
       does NOT (Voff 128), so each of its quads brackets itself and is told
       what to restore. */
    RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
    columns_set_texwindow(&tw);
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
        underworld_sky_draw(ctx, LM_CX, LM_SKY_Y, LM_CZ);
        draw_lm_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE COLUMN <<< (with its orbs and shards; area-gated
       inside), THE TWO DOOR SIGNS - no props - and any pickup a later seed
       puts down. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        item_pickups_draw(ctx);
        draw_columns(ctx);
        lm_door_text(ctx, LM_W_X, LM_W_Z, LM_W_TEXT_X, LM_W_TEXT_Y,
                     LM_W_TEXT_Z, LM_W_TEXT_YAW);
        lm_door_text(ctx, LM_N_X, LM_N_Z, LM_N_TEXT_X, LM_N_TEXT_Y,
                     LM_N_TEXT_Z, LM_N_TEXT_YAW);
    }
}
