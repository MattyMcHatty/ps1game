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
#include "h_corridor.h"
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "h_corridor_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "north_chamber.h"      /* ...and the ladder's                         */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons, game flags */
#include "title.h"              /* STATE_H_CORRIDOR, the maggots' area tag    */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The H Corridor — see h_corridor.h for the layout and the doors. */

static SMD  *hc_smd  = NULL;
static void *hc_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY, copied from src/zig_zag_tomb.c with its numbers:
   cull and fog-far are equal, the base is scaled by what the player is
   carrying, and the scale eases rather than jumping. 366/1300 at rest and two
   equal ~46% bonuses for the lantern, so the sight lines are 1300 / 1900 /
   2500. The Tomb's reach, so walking through the door between them does not
   change how far the player can see. */
#define HC_BASE_FOG_NEAR   366
#define HC_BASE_FOG_FAR   1300

#define HC_VIEW_UNIT        256
#define HC_VIEW_HELL_BONUS  118   /* ~+46% while the lantern is in hand   */
#define HC_VIEW_BURN_BONUS  118   /* ~+46% more while it is actually lit  */
#define HC_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t hc_view     = HC_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t hc_fog_near = HC_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t hc_fog_far  = HC_BASE_FOG_FAR;

static int32_t hc_view_target(void) {
    int32_t s = HC_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += HC_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += HC_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void hc_view_resolve(int snap) {
    int32_t target = hc_view_target();
    if (snap) {
        hc_view = target;
    } else if (hc_view < target) {
        hc_view += HC_VIEW_RATE;
        if (hc_view > target) hc_view = target;
    } else if (hc_view > target) {
        hc_view -= HC_VIEW_RATE;
        if (hc_view < target) hc_view = target;
    }
    hc_fog_near = (HC_BASE_FOG_NEAR * hc_view) >> 8;
    hc_fog_far  = (HC_BASE_FOG_FAR  * hc_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define HC_FOG_R             7
#define HC_FOG_G             6
#define HC_FOG_B             9

/* Wall standoff. The chapter's 195. Every corridor is 600 wide, which leaves a
   210 band down the middle of each. */
#define HC_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define HC_FLOOR_Y            0
#define HC_EYE_Y           (HC_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zone ------------------------------------------------------------
   ONE, FLAT, AT y=0, over the bounding box of the proxy's three floor faces
   (the west leg, the crossbar and the east leg — see the FLOOR list at the foot
   of src/h_corridor_mesh_collision.c). All three are the one plane, and the
   walls keep the player out of the two empty corners of the box, so one zone
   serves the "h" exactly as one served the Tomb's blocks. */
static void hc_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x =    0; floor_zones[0].max_x = 2400;
    floor_zones[0].min_z =    0; floor_zones[0].max_z = 5400;
    floor_zones[0].y     = HC_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   THREE, AND THE ROOM OWNS NONE — the Cleaver Corridor's set.

     0 cobblestones        the walls, the floor and the shaft — 407 of the
                           417 polys                              (x384 y0)
     1 catacomb inner door the two doorways                       (x832 y0)
     2 ladder              up the shaft's south wall, 5 polys
                           (x704 y256, the North Chamber's, on the
                           incinerator's page)

   The one untextured poly is the black cap over the shaft at y=-1600; the tex
   map gives it 0xFF and it draws flat.

   Slots 0 and 1 come through src/catacombs_entry.c's narrow uploaders, as every
   Chapter 3 room takes them, and slot 2 through north_chamber_upload_ladder().

   All three sit at Voff 0, so the one 128 texture window in the draw serves
   them. */
#define H_CORRIDOR_TEX_COUNT 3

static uint16_t tex_tpage[H_CORRIDOR_TEX_COUNT];
static uint16_t tex_clut[H_CORRIDOR_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/zig_zag_tomb.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int hc_key_count = 0;

static void hc_build_cull_keys(void) {
    hc_key_count = 0;
    if (!hc_smd) return;
    uint8_t *p = (uint8_t *)hc_smd->p_prims;
    int i, n = hc_smd->n_prims;
    if (n > H_CORRIDOR_PRIM_COUNT) n = H_CORRIDOR_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &hc_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    hc_key_count = n;
}

void h_corridor_load_geometry(void) {
    hc_buff = room_arena_load("\\TEXCTCMB\\HCORRDR.SMD;1");
    hc_smd  = hc_buff ? smdInitData(hc_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (hc_smd && !room_data_bind(hc_smd->n_prims)) hc_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    hc_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: three compile-time headers and no
   registration at all, so no texmgr_set_bank() either. The bank this room's art
   is in is decided by its owners'. */
void h_corridor_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
    TIM_SLOT(2, LADDER);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> THE LADDER LINE OVERWRITES THE INCINERATOR, as it does in the North
   Chamber and the Cleaver Corridor. <<< Nothing else in this room draws x704
   y256, and the Incinerator Room's own uploader puts the machine back on the
   way in there, so there is no ordering rule between the three calls. */
void h_corridor_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    north_chamber_upload_ladder();
}

/* ---- THE NORTH DOOR --------------------------------------------------------
   z=5400, x[200,400], y[-400,0] — in the north wall at the head of the west
   leg. Back into the Zig Zag Tomb, through the south door in its south-west
   corner.

   A door in the XY plane at fixed Z, approached from -Z (wall 9 runs z=5400
   with nz=-4096), so TEXT_PLANE_XY with mirror=0, the sign 11 proud of the wall
   along -Z, and the -200 door_draw_string_3d wants on the X argument (the
   reading axis for an XY sign). The Tomb's south door, the far side, takes the
   opposite pair.

   ---- THE SOUTH DOOR ----
   z=0, x[2000,2200], y[-400,0] — in the south wall at the foot of the east leg.
   Out to THE ROOM OF TORSOS, through the one door in its east face.

   In the XY plane at fixed Z, approached from +Z (wall 5 runs z=0 over
   x[1800,2400] with nz=+4095, so the walkable side is +Z), so TEXT_PLANE_XY
   with mirror=1, the sign 11 proud of the wall along +Z, and the -200 on the X
   argument. The Room of Torsos' east door, the far side, is a YZ door and takes
   its own pair.

   THE LADDER, z=0 x[200,400], is drawn and nothing else: no sign, no trigger.
   It reads as sealed until the room above it exists. It is 1700 from the south
   door, so the two can never share a trigger. */
#define HC_NORTH_X           300     /* the art spans x[200,400] */
#define HC_NORTH_Z          5400
#define HC_NORTH_TEXT_Y     (-186)   /* eye level on the y=0 floor */
#define HC_SOUTH_X          2100     /* the art spans x[2000,2200] */
#define HC_SOUTH_Z             0
#define HC_SOUTH_TEXT_Y     (-186)   /* eye level on the y=0 floor */

#define HC_TEXT_RADIUS      1200
#define HC_FADE_NEAR         800
#define HC_TRIGGER_RADIUS    500

/* Circle edge-detect, one per door. Seeded "held" by the arm below so a press
   carried in through the transition cannot fire on the arrival frame. */
static int north_circle_prev = 1;
static int south_circle_prev = 1;

void h_corridor_arm(void) {
    north_circle_prev = interact_tapped();
    south_circle_prev = north_circle_prev;
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int h_corridor_north_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !north_circle_prev;
    int32_t dx, dz, xz;
    north_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - HC_NORTH_X;
    dz = cam_z - HC_NORTH_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= HC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(HC_NORTH_X, HC_NORTH_Z)) return 0;
    return 1;
}

/* The north door's floating sign. Same shape as every door sign in the game:
   opaque within HC_FADE_NEAR, gone by HC_TEXT_RADIUS. */
static void hc_north_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - HC_NORTH_X;
    int32_t dz = cam_z - HC_NORTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= HC_TEXT_RADIUS) return;

    if (xz > HC_FADE_NEAR) {
        int range = HC_TEXT_RADIUS - HC_FADE_NEAR;
        int prog  = xz - HC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        HC_NORTH_X - 200, HC_NORTH_TEXT_Y, HC_NORTH_Z - 11,
                        50, 255, 50, fade,
                        0, TEXT_PLANE_XY,   /* mirror=0: XY door approached from -Z */
                        DOOR_PIXEL_SIZE);
}

/* The south door's Circle test. The north door's shape; the two are 7200
   apart in Manhattan terms, so they can never both be in reach. */
int h_corridor_south_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !south_circle_prev;
    int32_t dx, dz, xz;
    south_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - HC_SOUTH_X;
    dz = cam_z - HC_SOUTH_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= HC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(HC_SOUTH_X, HC_SOUTH_Z)) return 0;
    return 1;
}

static void hc_south_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - HC_SOUTH_X;
    int32_t dz = cam_z - HC_SOUTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= HC_TEXT_RADIUS) return;

    if (xz > HC_FADE_NEAR) {
        int range = HC_TEXT_RADIUS - HC_FADE_NEAR;
        int prog  = xz - HC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        HC_SOUTH_X - 200, HC_SOUTH_TEXT_Y, HC_SOUTH_Z + 11,
                        50, 255, 50, fade,
                        1, TEXT_PLANE_XY,   /* mirror=1: XY door approached from +Z */
                        DOOR_PIXEL_SIZE);
}

/* ---- THE MAGGOT DROP (h_corridor.h) -----------------------------------------
   Two spots inside the ladder shaft (x[0,600] z[0,600], open to y=-1600), one
   either side of the ladder's x[200,400] and 150 off the south wall it climbs,
   so a body appears in the shaft rather than in the ladder. Taken in turn,
   west spot first.

   SPAWNED HIGH, ON PURPOSE. maggot_spawn() takes a floor ANCHOR, and
   apply_ddog_height lets an anchor that starts ABOVE the floor fall under
   GRAVITY to it (it only refuses to lift one that starts below). An anchor of
   -1249 puts the body about y=-1270, inside the shaft and under its y=-1600
   cap, and it falls the 1100 to the standing anchor in a little over a second
   — a maggot dropping out of the dark above the ladder rather than appearing
   from nothing on the floor. It hunts from its first frame (no emergence), so
   it drifts out of the shaft as it falls.

   THE TIMING: the first falls HC_MAGGOT_FIRST frames after arrival, the rest
   every HC_MAGGOT_GAP. The arrival is 5000 down the west leg, and at MGT_SPEED
   (11) that is ~7.5 s of flight, so all five are in the air before the first
   one arrives: a string of them coming up the leg, not a clump. */
#define HC_MAGGOT_COUNT        5
#define HC_MAGGOT_FIRST       60    /* 1 s after the door                       */
#define HC_MAGGOT_GAP        120    /* 2 s between drops                        */
#define HC_MAGGOT_SPOT_Z     150
#define HC_MAGGOT_DROP_Y    (-1100) /* above the standing anchor, in the shaft  */

static const int16_t hc_maggot_spot_x[2] = { 100, 500 };

static int hc_from_tomb      = 0;   /* latch: set by the Tomb's south door      */
static int hc_maggots_left   = 0;   /* still to fall on this visit              */
static int hc_maggot_timer   = 0;   /* frames to the next drop                  */
static int hc_maggot_next    = 0;   /* which spot the next one falls from       */

void h_corridor_note_tomb_arrival(void) {
    hc_from_tomb = 1;
}

/* Called by h_corridor_init(): consume the latch, and spring the drop if this
   is the arrival from the Tomb and it has never run. */
static void hc_maggots_arm(void) {
    int from_tomb = hc_from_tomb;
    hc_from_tomb    = 0;
    hc_maggots_left = 0;
    if (!from_tomb || game_flag(FLAG_H_CORRIDOR_MAGGOTS)) return;
    game_flag_set(FLAG_H_CORRIDOR_MAGGOTS);
    hc_maggots_left = HC_MAGGOT_COUNT;
    hc_maggot_timer = HC_MAGGOT_FIRST;
    hc_maggot_next  = 0;
}

void h_corridor_update(void) {
    if (hc_maggots_left <= 0 || game_over) return;
    if (--hc_maggot_timer > 0) return;
    /* A full pool places nothing; that one is lost rather than retried, which
       only a pool already holding the Meat Plant's swarm could cause — and
       maggots_reset() empties it on every room change. */
    maggot_spawn(hc_maggot_spot_x[hc_maggot_next], HC_MAGGOT_SPOT_Z,
                 HC_FLOOR_Y - GROUND_FLOOR_Y + HC_MAGGOT_DROP_Y,
                 STATE_H_CORRIDOR);
    hc_maggot_next ^= 1;
    hc_maggots_left--;
    hc_maggot_timer = HC_MAGGOT_GAP;
}

void h_corridor_spawn_north(void) {
    /* In from the Zig Zag Tomb. 220 off wall 9 on its walkable -Z side, on the
       door's centre line (the west leg's, 300 clear of both its walls), facing
       -Z — the direction of travel, south down the leg. */
    cam_x   = HC_NORTH_X;
    cam_y   = HC_EYE_Y;
    cam_vy  = 0;
    cam_z   = HC_NORTH_Z - (HC_WALL_RADIUS + 25);
    cam_rot = 2048;                    /* facing -Z, south */
    h_corridor_arm();
}

void h_corridor_spawn_south(void) {
    /* Back from the Room of Torsos. 220 off wall 5 on its walkable +Z side, on
       the door's centre line (the east leg's, 300 clear of both its walls),
       facing +Z — the direction of travel, north up the leg. */
    cam_x   = HC_SOUTH_X;
    cam_y   = HC_EYE_Y;
    cam_vy  = 0;
    cam_z   = HC_SOUTH_Z + (HC_WALL_RADIUS + 25);
    cam_rot = 0;                       /* facing +Z, north */
    h_corridor_arm();
}

void h_corridor_init(void) {
    room_data_collision(&current_collision_room);
    /* The walls' tops, y=-800: there is no vault mesh, the Tomb's open dark.
       The ladder shaft rises above it, but nothing walks up there. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(HC_WALL_RADIUS);

    hc_floor_zones_init();
    cam_pitch = 0;

    h_corridor_spawn_north();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside the corridor. Safe to clear: every room that has one re-places its
       own on entry. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    hc_maggots_arm();   /* the drop, if this is the Tomb arrival and the first */

    hc_view_resolve(1);
}

static void draw_h_corridor_smd(RenderContext *ctx) {
    if (!hc_smd) return;

    uint8_t *p = (uint8_t *)hc_smd->p_prims;
    int i, n = hc_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = hc_fog_far;   /* resolved by hc_view_resolve() this frame */
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
        SVECTOR *v0 = &hc_smd->p_verts[vi[0]];
        SVECTOR *v1 = &hc_smd->p_verts[vi[1]];
        SVECTOR *v2 = &hc_smd->p_verts[vi[2]];

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

        int nocull = (i < H_CORRIDOR_PRIM_COUNT) && room_nocull(i);
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
            v3 = &hc_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner (render.h), so the
           floor stays behind whatever stands on it. */
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
        int32_t fog = dist < hc_fog_near ? hc_fog_near : (dist > hc_fog_far ? hc_fog_far : dist);
        int32_t fog_factor = ((hc_fog_far - fog) << 8) / (hc_fog_far - hc_fog_near);

        uint8_t tex_idx = (i < H_CORRIDOR_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < H_CORRIDOR_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + HC_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + HC_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + HC_FOG_B * (256 - fog_factor)) >> 8);

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

void h_corridor_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    hc_view_resolve(0);

    g_fog_near = hc_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : hc_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, HC_FOG_R, HC_FOG_G, HC_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All three textures sit at Voff 0. */
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

    if (exp != DBG_EXP_NO_MESH) draw_h_corridor_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGNS AND THE ENEMIES. <<< Nothing else stands
       in this room. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        hc_north_door_text(ctx);   /* north: XY plane, approached from -Z */
        hc_south_door_text(ctx);   /* south: XY plane, approached from +Z */
        /* THE CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Their sheets sit at Voff 128, so each is handed the window to restore
           after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
            maggots_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        draw_maggots(ctx);   /* area-tagged: free where none is placed */
    }
}
