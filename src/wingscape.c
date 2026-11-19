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
#include "wingscape.h"
#include "wingscape_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "hell_hath_no_fury.h"  /* the volcanic ground, a narrow uploader     */
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "sharurs_trial.h"      /* the rocks, a narrow uploader               */
#include "catacombs_entry.h"    /* the catacomb inner door, a narrow uploader */
#include "wings_tablet.h"       /* the terrace's tablet                       */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"
#include "golem.h"              /* the rock giant by the west door          */

/* Wingscape - see wingscape.h. The room half is src/lava_u.c's (itself Hell
   Hath No Fury's): cull keys, radius side cull and all. */

static SMD  *ws_smd  = NULL;
static void *ws_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[-3300,3300] z[-2700,3100] (the collision proxy's bounds). The sky is
   centred on the middle of it, half way up the tablet (ground 0, terrace
   -500, tablet top -1100). */
#define WS_CX                0
#define WS_CZ              200
#define WS_SKY_Y          (-550)

#define WS_FLOOR_Y           0
#define WS_TERRACE_Y      (-500)
#define WS_EYE_Y          (WS_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The tallest drawn thing is the tablet, whose top is at y=-1100; the
   proxy's terrace rim reaches -1104. */
#define WS_CEILING_Y      (-1105)

/* ---- THE SOUTH-EAST DOOR, to Hell Hath No Fury -----------------------------
   The catacomb inner door polys span (2700,-2500)-(3100,-2100): centre
   (2900,-2300), on the cut corner x-z=5200 (collision wall 14, inward normal
   (-0.707,+0.707)), so the player walks up to it from the north-west.

   THE SIGN is on a free yaw facing the way the reader faces - 1536, south-
   east - 11 off the door toward the player (8 on each axis), at the
   doorway's mid-height (the door is 400 tall). THE SPAWN, for an arrival
   through it, is 250 in along the normal (177 on each axis), clear of the 195
   push radius, facing north-west into the room, on the volcanic ground (the
   lava stops at x=1500). */
#define WS_SE_X            2900
#define WS_SE_Z          (-2300)
#define WS_SE_TEXT_X       2892
#define WS_SE_TEXT_Z     (-2292)
#define WS_SE_TEXT_Y      (WS_FLOOR_Y - 200)
#define WS_SE_TEXT_YAW     1536
#define WS_SE_SPAWN_X      2723
#define WS_SE_SPAWN_Z    (-2123)
#define WS_SE_SPAWN_ROT    3584
/* ---- THE WEST DOOR, to Hero's Plinth's north-east door ---------------------
   The wd_dr polys stand in the west wall x=-3300, z[-1500,-1300] (leaning in
   to z[-1436,-1374] at the top, y=-600): centre (-3300,-1400), approached
   from the east. Sign facing west (yaw 3072), 11 off the wall toward the
   player, at the doorway's mid-height; spawn 250 east, facing east. */
#define WS_W_X           (-3300)
#define WS_W_Z           (-1400)
#define WS_W_TEXT_X      (-3289)
#define WS_W_TEXT_Z      (-1400)
#define WS_W_TEXT_Y       (WS_FLOOR_Y - 200)
#define WS_W_TEXT_YAW      3072
#define WS_W_SPAWN_X     (-3050)
#define WS_W_SPAWN_Z     (-1400)
#define WS_W_SPAWN_ROT     1024

#define WS_TEXT_RADIUS     1500
#define WS_FADE_NEAR       1000
#define WS_TRIGGER_RADIUS   500

/* Circle edge-detect. Seeded "held" by the arm so a press carried in through
   the transition cannot fire on the arrival frame. */
static int se_circle_prev = 1;
static int w_circle_prev  = 1;

/* ---- THE FLOOR ZONES -------------------------------------------------------
   Two storeys sharing a footprint in places (the ground poly runs on under the
   terrace), so ORDER IS THE MECHANISM (tools/ADDING_A_ROOM.txt STEP 5, the Up
   Down Maze note): apply_height() takes the first zone that is not above the
   player, so the TERRACE rectangles come first, then the two RAMPS, and the
   ground catch-all last.

   The terrace rectangles stop at the TOP of the rock face along its south
   edge, never over the ground below it: a rectangle that overhung the ground
   would hold a player who stepped off the edge up at y=-500 in mid-air. They
   may overhang the void beyond the rim; the rim's walls keep the player out.
   The edge, from the proxy (assets/underworld/Wingscape mesh.smx):
       x[-3300,-2450]  z=700    (the west ramp's head)
       x[-2450,-2400]  700 -> 900 on the diagonal (the ramp's side wall)
       x[-2400,-2100]  z=900
       x[-2100,-1900]  900 -> 1100 on the diagonal
       x[-1900,-1700]  z=1100
       x[-1700,-1500]  1100 -> 1300 on the diagonal
       x[-1500, 1900]  z=1300
       x[ 1900, 2100]  1300 -> 1100 on the diagonal (the east ramp's corner)
       x[ 2100, 3300]  z=1300   (the east ramp's head)
   The diagonal steps are left to the zone below them: a rectangle there
   would overhang the slope by up to 200. The one exception is the west
   landing's 50-wide sliver beside the ramp's side wall: the ground player
   is held a full wall radius east of that wall, so never stands under it. */
static void ws_floor_zones_init(void) {
    int i = 0;

    /* THE TERRACE, y=-500. */
    floor_zones[i].type  = FLOOR_UPPER;          /* the north band          */
    floor_zones[i].min_x = -1700;  floor_zones[i].max_x = 3300;
    floor_zones[i].min_z =  1300;  floor_zones[i].max_z = 3100;
    floor_zones[i].y     = WS_TERRACE_Y;
    i++;
    floor_zones[i].type  = FLOOR_UPPER;          /* the north-west          */
    floor_zones[i].min_x = -3300;  floor_zones[i].max_x = -1700;
    floor_zones[i].min_z =  1100;  floor_zones[i].max_z = 3100;
    floor_zones[i].y     = WS_TERRACE_Y;
    i++;
    floor_zones[i].type  = FLOOR_UPPER;          /* the west, south of that */
    floor_zones[i].min_x = -3300;  floor_zones[i].max_x = -2100;
    floor_zones[i].min_z =   900;  floor_zones[i].max_z = 1100;
    floor_zones[i].y     = WS_TERRACE_Y;
    i++;
    floor_zones[i].type  = FLOOR_UPPER;          /* the west ramp's landing */
    floor_zones[i].min_x = -3300;  floor_zones[i].max_x = -2400;
    floor_zones[i].min_z =   700;  floor_zones[i].max_z = 900;
    floor_zones[i].y     = WS_TERRACE_Y;
    i++;

    /* THE RAMPS, both rising north. */
    floor_zones[i].type            = FLOOR_RAMP; /* east                    */
    floor_zones[i].min_x =  2100;  floor_zones[i].max_x = 3300;
    floor_zones[i].min_z =  -100;  floor_zones[i].max_z = 1300;
    floor_zones[i].ramp_y_start    = WS_FLOOR_Y;     /* at z=-100 */
    floor_zones[i].ramp_y_end      = WS_TERRACE_Y;   /* at z=1300 */
    floor_zones[i].ramp_axis_start = -100;
    floor_zones[i].ramp_axis_end   = 1300;
    floor_zones[i].ramp_along_x    = 0;
    i++;
    floor_zones[i].type            = FLOOR_RAMP; /* west                    */
    floor_zones[i].min_x = -3300;  floor_zones[i].max_x = -2400;
    floor_zones[i].min_z =  -500;  floor_zones[i].max_z = 700;
    floor_zones[i].ramp_y_start    = WS_FLOOR_Y;     /* at z=-500 */
    floor_zones[i].ramp_y_end      = WS_TERRACE_Y;   /* at z=700  */
    floor_zones[i].ramp_axis_start = -500;
    floor_zones[i].ramp_axis_end   = 700;
    floor_zones[i].ramp_along_x    = 0;
    i++;

    /* THE GROUND, y=0: the catch-all, LAST. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -3300;  floor_zones[i].max_x = 3300;
    floor_zones[i].min_z = -2700;  floor_zones[i].max_z = 3100;
    floor_zones[i].y     = WS_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   FIVE mesh textures, NONE owned - so no texmgr_set_bank() and no CD access:
     0  volcanic             Hell Hath No Fury's, its narrow uploader (x384 y256)
     1  lava                 the Descent's, its narrow uploader (x320 y256). Its
                             UVs run past 128 and wrap in the 128 window.
     2  rocks_128            Sharur's Trial's ROCKS.TIM (4bpp x512 y0): the same
                             128x128 art under its Blender material name,
                             through sharurs_trial_upload_rocks().
     3  wd_dr                RESIDENT: the fat door's startup upload at x768
                             y256 - the west wall's decorative door. Nothing
                             here may take that page.
     4  catacomb inner door  the Catacombs Entry's, its narrow uploader (x832
                             y0) - which is also DOOR_PANEL_CATACOMB's panel,
                             the transition out of this room, so it MUST be
                             uploaded here (the panel draws before
                             STATE_LOADING).
   The WINGS TABLET's texture is its module's (x704 y0, Sharur's Tablet's page
   and CLUT, time-shared: neither room draws the other's tablet). The sky's
   texture is src/underworld_sky.c's business. */
#define WS_TEX_COUNT 5

static uint16_t tex_tpage[WS_TEX_COUNT];
static uint16_t tex_clut[WS_TEX_COUNT];

void wingscape_load_assets(void) {
    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, LAVA);
    TIM_SLOT(2, ROCKS);
    TIM_SLOT(3, WDDR);
    TIM_SLOT(4, CTCMBDR);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void wingscape_upload_textures(void) {
    hell_hath_no_fury_upload_volcanic();
    descent_to_hell_upload_lava();
    sharurs_trial_upload_rocks();
    catacombs_entry_upload_inner_door();
    wings_tablet_upload_texture();
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   lu_build_cull_keys() verbatim (src/lava_u.c, itself Hell Hath No Fury's;
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius). Built on the
   same call that reloads the mesh it describes, and nowhere else. */
static int     ws_key_count = 0;
static int32_t ws_y_lo = 0, ws_y_hi = 0;

static void ws_build_cull_keys(void) {
    ws_key_count = 0;
    if (!ws_smd) return;
    uint8_t *p = (uint8_t *)ws_smd->p_prims;
    int i, n = ws_smd->n_prims;
    if (n > WINGSCAPE_PRIM_COUNT) n = WINGSCAPE_PRIM_COUNT;
    ws_y_lo = ws_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &ws_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &ws_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < ws_y_lo) ws_y_lo = vk->vy;
            if (vk->vy > ws_y_hi) ws_y_hi = vk->vy;
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
    ws_key_count = n;
}

void wingscape_load_geometry(void) {
    ws_buff = room_arena_load("\\TEXUNDW\\WINGSCP.SMD;1");
    ws_smd  = ws_buff ? smdInitData(ws_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (ws_smd && !room_data_bind(ws_smd->n_prims)) ws_smd = NULL;
    ws_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The two doors ---------------------------------------------------------*/
void wingscape_doors_arm(void) {
    int held = interact_tapped();
    se_circle_prev = held;
    w_circle_prev  = held;
}

/* The shared body: a fresh Circle within reach of (door_x, door_z) and facing
   it. The edge state is kept up to date even while locked. A door a living
   Golem stands in front of is `blocked` (src/golem.h, BLOCKING): no Circle,
   and no sign, until it dies. */
static int ws_door_triggered(int32_t door_x, int32_t door_z,
                             int *circle_prev, int lock, int blocked) {
    int held = interact_tapped();
    int just = held && !*circle_prev;
    int32_t dx, dz;
    *circle_prev = held;
    if (lock || !just || blocked) return 0;
    dx = cam_x - door_x;
    dz = cam_z - door_z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= WS_TRIGGER_RADIUS)
        return 0;
    return interact_facing(door_x, door_z);
}

int wingscape_se_door_triggered(int lock) {
    return ws_door_triggered(WS_SE_X, WS_SE_Z, &se_circle_prev, lock,
               golem_blocks(STATE_WINGSCAPE, GLM_BLOCKS_WINGSCAPE_SE_DOOR));
}
int wingscape_w_door_triggered(int lock) {
    return ws_door_triggered(WS_W_X, WS_W_Z, &w_circle_prev, lock,
               golem_blocks(STATE_WINGSCAPE, GLM_BLOCKS_WINGSCAPE_W_DOOR));
}

static void ws_door_text(RenderContext *ctx, int32_t door_x, int32_t door_z,
                         int32_t text_x, int32_t text_y, int32_t text_z,
                         int yaw) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - door_z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= WS_TEXT_RADIUS) return;
    if (xz > WS_FADE_NEAR) {
        int range = WS_TEXT_RADIUS - WS_FADE_NEAR;
        int prog  = xz - WS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            text_x, text_y, text_z,
                            50, 255, 50, fade, yaw,
                            DOOR_PIXEL_SIZE);
}

static void ws_spawn_at(int32_t x, int32_t z, int rot) {
    cam_x     = x;
    cam_y     = WS_EYE_Y;
    cam_vy    = 0;
    cam_z     = z;
    cam_rot   = rot;
    cam_pitch = 0;
    camera_look_cancel();
    wingscape_doors_arm();
}

void wingscape_spawn_se(void) {
    ws_spawn_at(WS_SE_SPAWN_X, WS_SE_SPAWN_Z, WS_SE_SPAWN_ROT);
}
void wingscape_spawn_w(void) {
    ws_spawn_at(WS_W_SPAWN_X, WS_W_SPAWN_Z, WS_W_SPAWN_ROT);
}

void wingscape_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(WS_CEILING_Y);
    ws_floor_zones_init();

    /* The default arrival (main.c overrides it per door), and where a debug
       jump lands: the south-east door, from Hell Hath No Fury. */
    wingscape_spawn_se();

    /* The global prop arrays, as Lava U clears them: Hell Hath No Fury's save
       point and tank would otherwise block invisibly inside this room's
       bounds. The clear drops the placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();

    /* The tablet's puzzle: no shot, no shards, and a NEW ANSWER ORDER - the
       brief re-rolls it on every entry. Whether the slab stands at all is
       FLAG_WINGS_TABLET_BROKEN, read live. */
    wings_tablet_arm();
}

/* ---- The mesh --------------------------------------------------------------
   draw_lu_smd() (src/lava_u.c) verbatim: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_ws_smd(RenderContext *ctx) {
    if (!ws_smd) return;

    uint8_t *p = (uint8_t *)ws_smd->p_prims;
    int i, n = ws_key_count;

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
#define WS_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = ws_y_lo - cam_y, dyhi = ws_y_hi - cam_y;
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
            f = f * 5 + WS_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &ws_smd->p_verts[vi[0]];
        SVECTOR *v1 = &ws_smd->p_verts[vi[1]];
        SVECTOR *v2 = &ws_smd->p_verts[vi[2]];

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

        int nocull = (i < WINGSCAPE_PRIM_COUNT) && room_nocull(i);
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
            v3 = &ws_smd->p_verts[vi[3]];
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

        uint8_t tex_idx = (i < WINGSCAPE_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < WS_TEX_COUNT);
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

void wingscape_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All five room textures, the tablet's and the sky's sit at Voff 0. The
       GOLEM's sheet does NOT (Voff 128), so it brackets itself and is told
       what to restore. */
    {
        RECT gtw = { 0, 0, 128 >> 3, 128 >> 3 };
        golems_set_texwindow(&gtw);
    }
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
        underworld_sky_draw(ctx, WS_CX, WS_SKY_Y, WS_CZ);
        draw_ws_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE WINGS TABLET (its shards and its sign), NINURTA'S
       WINGS once it has shattered, the TWO DOOR SIGNS, and the GOLEMS - the
       door's and the tablet's guard (area-gated inside; src/golem.h). The
       tablet reloads the plain view itself, so the Golem after it still
       projects through the camera. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        wings_tablet_draw(ctx);
        item_pickups_draw(ctx);
        draw_golems(ctx);
        if (!golem_blocks(STATE_WINGSCAPE, GLM_BLOCKS_WINGSCAPE_SE_DOOR))
            ws_door_text(ctx, WS_SE_X, WS_SE_Z, WS_SE_TEXT_X, WS_SE_TEXT_Y,
                         WS_SE_TEXT_Z, WS_SE_TEXT_YAW);
        if (!golem_blocks(STATE_WINGSCAPE, GLM_BLOCKS_WINGSCAPE_W_DOOR))
            ws_door_text(ctx, WS_W_X, WS_W_Z, WS_W_TEXT_X, WS_W_TEXT_Y,
                         WS_W_TEXT_Z, WS_W_TEXT_YAW);
    }

    /* The tablet's question, answers and reticule, over the puzzle's shot.
       Screen space and outside the entity gate: it is the puzzle. */
    wings_tablet_draw_overlay(ctx);
}
