#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "cull_arena.h"
#include "tim_slots.h"
#include "camera.h"
#include "cleaver_l.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "cleaver_l_mesh_collision.h"
#include "cleaver_l_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "the_pit.h"            /* ...and the rusty ironwork, for the blades */
#include "cleaver.h"            /* the four slamming blades                  */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* Cleaver L — see cleaver_l.h for the layout, the blades and the doors. */

static SMD  *cll_smd  = NULL;
static void *cll_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 with +50%/+50% for the lantern — the Cleaver Corridor's numbers, for
   the same 600-wide run. 261 primitives, fewer than the corridor's 321, so
   there is no lag argument for pulling them in. From the door the corner is
   2400 off and in the dark; the blades are walked into rather than seen. */
#define CLL_BASE_FOG_NEAR   450
#define CLL_BASE_FOG_FAR   1600

#define CLL_VIEW_UNIT        256
#define CLL_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define CLL_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define CLL_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t cll_view     = CLL_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t cll_fog_near = CLL_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t cll_fog_far  = CLL_BASE_FOG_FAR;

static int32_t cll_view_target(void) {
    int32_t s = CLL_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += CLL_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += CLL_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void cll_view_resolve(int snap) {
    int32_t target = cll_view_target();
    if (snap) {
        cll_view = target;
    } else if (cll_view < target) {
        cll_view += CLL_VIEW_RATE;
        if (cll_view > target) cll_view = target;
    } else if (cll_view > target) {
        cll_view -= CLL_VIEW_RATE;
        if (cll_view < target) cll_view = target;
    }
    cll_fog_near = (CLL_BASE_FOG_NEAR * cll_view) >> 8;
    cll_fog_far  = (CLL_BASE_FOG_FAR  * cll_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define CLL_FOG_R             7
#define CLL_FOG_G             6
#define CLL_FOG_B             9

/* Wall standoff. The chapter's 195. Both shafts are 600 wide, which leaves a
   210 band down the middle of each — the Cleaver Corridor's. */
#define CLL_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define CLL_FLOOR_Y            0
#define CLL_EYE_Y           (CLL_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   TWO, FLAT, AT y=0, over the proxy's two floor faces exactly — the two shafts
   (see the FLOOR list at the foot of src/cleaver_l_mesh_collision.c). They
   overlap on the corner square and are one plane, so their order is a nicety. */
static void cll_floor_zones_init(void) {
    int i = 0;

    /* The north-south shaft, the door's. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x =     0; floor_zones[i].max_x =   600;
    floor_zones[i].min_z = -2400; floor_zones[i].max_z =   600;
    floor_zones[i].y     = CLL_FLOOR_Y;
    i++;

    /* The east-west shaft, corner included. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -2399; floor_zones[i].max_x =   600;
    floor_zones[i].min_z = -2400; floor_zones[i].max_z = -1800;
    floor_zones[i].y     = CLL_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   TWO, AND THE ROOM OWNS NONE.

     0 cobblestones        the walls and the floor — 257 of the 261 polys
                                                          (x384 y0)
     1 catacomb inner door the two doorways, north and west (x832 y0)

   Both come through src/catacombs_entry.c's narrow uploaders, as every
   Chapter 3 room takes them. The blades' rusty ironwork (x704 y0) is not in
   this table — cleavers_draw() uses the compile-time TIM_*_RUSTY constants —
   but this room still has to put it up, see cleaver_l_upload_textures().

   Both sit at Voff 0, so the one 128 texture window in the draw serves them. */
#define CLEAVER_L_TEX_COUNT 2

static uint16_t tex_tpage[CLEAVER_L_TEX_COUNT];
static uint16_t tex_clut[CLEAVER_L_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/room_of_bones.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int cll_key_count = 0;

static void cll_build_cull_keys(void) {
    cll_key_count = 0;
    if (!cll_smd) return;
    uint8_t *p = (uint8_t *)cll_smd->p_prims;
    int i, n = cll_smd->n_prims;
    if (n > CLEAVER_L_PRIM_COUNT) n = CLEAVER_L_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &cll_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    cll_key_count = n;
}

void cleaver_l_load_geometry(void) {
    cll_buff = room_arena_load("\\TEXCTCMB\\CLEAVERL.SMD;1");
    cll_smd  = cll_buff ? smdInitData(cll_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    cll_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: two compile-time headers and no
   registration at all, so no texmgr_set_bank() either (the Cleaver Corridor's
   case). The bank this room's art is in is decided by the owners'. */
void cleaver_l_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).
   Three pages, none shared with another here, so no ordering rule. */
void cleaver_l_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    /* ...and The Pit's rusty ironwork, which the CLEAVERS are modelled in
       (src/cleaver.h) — x704 y0, a page nothing else in this room draws. */
    the_pit_upload_rusty();
}

/* ---- THE NORTH DOOR --------------------------------------------------------
   z=600, x[200,400], y[-400,0] — in the north end wall of the N-S shaft. Back
   into the Meat Plant, through the door at the back of its south alcove.

   A door in the XY plane at fixed Z, approached from -Z (wall 0 runs z=600 with
   nz=-4096), so TEXT_PLANE_XY with mirror=0, the sign 11 proud of the wall
   along -Z, and the -200 door_draw_string_3d wants on the X argument (the
   reading axis for an XY sign). The Meat Plant's south-alcove door, the far
   side, takes the opposite pair.

   THE WEST DOOR, x=-2400 z[-2200,-2000] at the far end of the E-W shaft, is
   drawn and nothing else: no sign, no trigger. It reads as a sealed door until
   the room behind it exists. */
#define CLL_NORTH_X           300     /* the art spans x[200,400] */
#define CLL_NORTH_Z           600
#define CLL_NORTH_TEXT_Y     (-186)   /* eye level on the y=0 floor */
#define CLL_TEXT_RADIUS      1200
#define CLL_FADE_NEAR         800
#define CLL_TRIGGER_RADIUS    500

/* ---- THE CLEAVERS (src/cleaver.h) ------------------------------------------
   FOUR, TWO TO AN ARM, AND THE LAYOUT IS THE BRIEF'S.

   THE CORNER PAIR hang ON the edges of the corner square x[0,600]
   z[-2400,-1800], where the arms meet:

     NORTH EDGE  (300,-1800)  TURNED, across the N-S shaft, on the line of the
                              E-W shaft's north wall (wall 4, z=-1800)
     WEST EDGE   (0,-2100)    as modelled, across the E-W shaft, on the line of
                              the N-S shaft's west wall (wall 5, x=0)

   Each is 550 wide in a 600 shaft and centred in it, so they stop 25 short of
   the inner corner at (0,-1800) and never meet. Down, both hold the player 150
   off their faces, which leaves the corner square itself — x[165,405] by
   z[-2205,-1965] of standing room — as a pocket out of reach of either: a
   player can cross one blade, wait in the corner and time the other.

   THE OTHER TWO split what is left of each arm in two, the Cleaver Corridor's
   "every gap the same" rule:

     N-S  (300,-600)    TURNED: halfway from the north door (z=600) to the
                        corner blade, 1200 each way
     E-W  (-1200,-2100) halfway from the corner blade to the west end wall
                        (x=-2400, the sealed door's), 1200 each way

   The arrival spawn is at z=380, 980 short of the first blade and 580 outside
   its trigger, so nothing falls on arrival. The span gate on the trigger
   (cleaver.h, THE CYCLE) is what keeps the west-edge blade, 300 west of the
   N-S shaft's centre line, from firing on a player anywhere up that shaft.

   The Cleaver Corridor's lift and slot line: the edge 100 above the authored
   650, and the slot at the walls' tops, y=-800 — above it is the open dark. */
#define CLL_CLEAVER_RAISE     100
#define CLL_CEILING_Y        (-800)

#define CLL_SHAFT_X           300     /* centre line of the N-S shaft   */
#define CLL_ARM_Z           (-2100)   /* centre line of the E-W shaft   */
#define CLL_CORNER_N        (-1800)   /* the corner square's north edge */
#define CLL_CORNER_W            0     /* ...and its west edge           */
#define CLL_WEST_END        (-2400)

static void cll_place_cleavers(void) {
    int32_t lift = cleaver_authored_lift() + CLL_CLEAVER_RAISE;
    cleavers_clear();
    /* N-S shaft: turned, across it. */
    cleaver_place(STATE_CLEAVER_L, CLL_SHAFT_X, -GROUND_FLOOR_Y,
                  (CLL_NORTH_Z + CLL_CORNER_N) / 2, lift, CLL_CEILING_Y, 1);
    cleaver_place(STATE_CLEAVER_L, CLL_SHAFT_X, -GROUND_FLOOR_Y,
                  CLL_CORNER_N, lift, CLL_CEILING_Y, 1);
    /* E-W shaft: as modelled, across it. */
    cleaver_place(STATE_CLEAVER_L, CLL_CORNER_W, -GROUND_FLOOR_Y,
                  CLL_ARM_Z, lift, CLL_CEILING_Y, 0);
    cleaver_place(STATE_CLEAVER_L, (CLL_CORNER_W + CLL_WEST_END) / 2,
                  -GROUND_FLOOR_Y, CLL_ARM_Z, lift, CLL_CEILING_Y, 0);
}

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int north_circle_prev = 1;

void cleaver_l_arm(void) {
    north_circle_prev = interact_tapped();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int cleaver_l_north_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !north_circle_prev;
    int32_t dx, dz, xz;
    north_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - CLL_NORTH_X;
    dz = cam_z - CLL_NORTH_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= CLL_TRIGGER_RADIUS) return 0;
    if (!interact_facing(CLL_NORTH_X, CLL_NORTH_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within CLL_FADE_NEAR, gone by CLL_TEXT_RADIUS. */
static void cll_north_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - CLL_NORTH_X;
    int32_t dz = cam_z - CLL_NORTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= CLL_TEXT_RADIUS) return;

    if (xz > CLL_FADE_NEAR) {
        int range = CLL_TEXT_RADIUS - CLL_FADE_NEAR;
        int prog  = xz - CLL_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        CLL_NORTH_X - 200, CLL_NORTH_TEXT_Y, CLL_NORTH_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

void cleaver_l_spawn_north(void) {
    /* 220 off wall 0 on its walkable -Z side, on the door's centre line (the
       shaft's, 300 clear of both side walls), facing -Z — the direction of
       travel, south down the shaft. The first blade is 980 further on. */
    cam_x   = CLL_NORTH_X;
    cam_y   = CLL_EYE_Y;
    cam_vy  = 0;
    cam_z   = CLL_NORTH_Z - (CLL_WALL_RADIUS + 25);
    cam_rot = 2048;                    /* facing -Z, south down the shaft */
    cleaver_l_arm();
}

void cleaver_l_init(void) {
    cleaver_l_collision_init(&current_collision_room);
    /* The walls' tops, y=-800: there is no vault mesh, the Cleaver Corridor's
       open dark. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(CLL_WALL_RADIUS);

    cll_floor_zones_init();
    cam_pitch = 0;

    cleaver_l_spawn_north();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside the L. The Catacombs Entry's sconce at (595,200) lands inside the
       N-S shaft; sconces gate on area, so it does not bite, and this is the
       cheap guarantee rather than a fix. Safe to clear: catacombs_entry_init()
       re-places all four. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* THE CLEAVERS, cleared and re-placed ARMED on every entry: leaving the
       room is what resets them (src/cleaver.h). */
    cll_place_cleavers();

    cll_view_resolve(1);
}

static void draw_cleaver_l_smd(RenderContext *ctx) {
    if (!cll_smd) return;

    uint8_t *p = (uint8_t *)cll_smd->p_prims;
    int i, n = cll_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = cll_fog_far;   /* resolved by cll_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* THE REJECT PATH READS cull_keys, NOT THE MESH. */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t cd = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (cd > cull)                        { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12)) { p += stride; continue; }
        }

        /* SURVIVED BOTH CULLS: only now is the header read and the vertex array
           addressed. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &cll_smd->p_verts[vi[0]];
        SVECTOR *v1 = &cll_smd->p_verts[vi[1]];
        SVECTOR *v2 = &cll_smd->p_verts[vi[2]];

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

        int nocull = (i < CLEAVER_L_PRIM_COUNT) && cleaver_l_nocull[i];
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
            v3 = &cll_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner (render.h): the floor
           must not be sorted over a fallen blade lying on it. */
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
        int32_t fog = dist < cll_fog_near ? cll_fog_near : (dist > cll_fog_far ? cll_fog_far : dist);
        int32_t fog_factor = ((cll_fog_far - fog) << 8) / (cll_fog_far - cll_fog_near);

        uint8_t tex_idx = (i < CLEAVER_L_PRIM_COUNT) ? cleaver_l_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < CLEAVER_L_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + CLL_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + CLL_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + CLL_FOG_B * (256 - fog_factor)) >> 8);

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

void cleaver_l_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    cll_view_resolve(0);

    g_fog_near = cll_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : cll_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, CLL_FOG_R, CLL_FOG_G, CLL_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       Both textures sit at Voff 0, and the blades' UVs reach 128 and rely on
       it. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    if (exp != DBG_EXP_NO_MESH) draw_cleaver_l_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGN, THE CLEAVERS AND THE ENEMIES. <<< */
    if (exp != DBG_EXP_NO_ENTITIES) {
        cll_north_door_text(ctx);  /* north: XY plane, approached from -Z */
        /* BOTH CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Both sheets sit at Voff 128, so each is handed the window to restore
           after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        /* The blades AFTER the two enemy calls, whose windows are restored for
           this Voff-0 art: their UVs reach 128 (the bars' rule). */
        cleavers_draw(ctx);
    }
}
