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
#include "room_of_legs.h"
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "room_of_legs_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "crib.h"              /* the cot in front of the pile's spine */
#include "creep.h"             /* ...and what it pours out when the axe wakes it */
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Room of Legs — see room_of_legs.h for the layout and the door. */

static SMD  *rol_smd  = NULL;
static void *rol_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 with +50%/+50% for the lantern — the Room of Heads' numbers, for the
   same octagon. 283 primitives, smaller still than that room's 295, so there is
   no lag argument for pulling them in. */
#define ROL_BASE_FOG_NEAR   450
#define ROL_BASE_FOG_FAR   1600

#define ROL_VIEW_UNIT        256
#define ROL_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define ROL_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define ROL_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t rol_view     = ROL_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t rol_fog_near = ROL_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t rol_fog_far  = ROL_BASE_FOG_FAR;

static int32_t rol_view_target(void) {
    int32_t s = ROL_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += ROL_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += ROL_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void rol_view_resolve(int snap) {
    int32_t target = rol_view_target();
    if (snap) {
        rol_view = target;
    } else if (rol_view < target) {
        rol_view += ROL_VIEW_RATE;
        if (rol_view > target) rol_view = target;
    } else if (rol_view > target) {
        rol_view -= ROL_VIEW_RATE;
        if (rol_view < target) rol_view = target;
    }
    rol_fog_near = (ROL_BASE_FOG_NEAR * rol_view) >> 8;
    rol_fog_far  = (ROL_BASE_FOG_FAR  * rol_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define ROL_FOG_R             7
#define ROL_FOG_G             6
#define ROL_FOG_B             9

/* Wall standoff. The chapter's 195. The corridor between the pile's two arms is
   z[-304,304], which leaves a ~220-wide band down its middle — enough, and the
   spawn is on its centre line. */
#define ROL_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define ROL_FLOOR_Y            0
#define ROL_EYE_Y           (ROL_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- THE CRIB --------------------------------------------------------------
   The chapter's THIRD crib encounter, the Room of Arms' cot and state machine
   unchanged: a crucifaxe swing wakes it, it rocks, lights, and pours ten Creeps.
   Its solved flag is THIS room's bit in the save — crib_room_solved(
   STATE_ROOM_OF_LEGS) — separate from the Room of Arms' and the Room of Heads'
   (src/crib.h).

   WHERE: in front of the pile's SPINE, at the far (west) end of the corridor
   the player walks down from the door. The spine's inner face is proxy wall 22,
   x=-724 over z[-304,304]. rot_y 1024 turns the cot's 350 long axis onto Z, so
   it stands side-on to the player, back to the legs, spanning x[-700,-500]
   z[-175,175]: 24 clear of the spine, far too little for the player to get
   behind it (195 off the wall plus 75 off the cot), so it closes the corridor's
   west end and is struck from its east face. The corridor's side walls are at
   z=+-304, 129 past the cot's ends — also closed to the player, who is held 195
   off them.

   It is NOT between the pile's two ENDS, even though that is where the pile
   faces the door: the corridor there is z[-304,304], and even laid lengthways a
   cot 200 deep in the middle of it leaves ~200 a side against the 270 the
   player needs (195 off the pile plus 75 off the cot), so it would seal the
   player in at the door.

   THE CREEPS COME FROM THREE PLACES, and only the first is the Room of Arms':
     0  the cot's own centre, at its rim            (crib.c, every room)
     1  the NORTH end of the pile  (725,-200, 507)  crib_set_outer_spawns()
     2  the SOUTH end of the pile  (725,-200,-507)
   The end points are the tip of each arm's ridge in the VISUAL mesh — the vertex
   where the ridge line (536,+-696)-(725,+-507) meets the end cap that slopes down
   to the floor at (914,+-516). y=-200 is the drawn legs' height and not the
   proxy's -599. Both lie north and south of the spawn (1073,0) as the player
   comes in, one either side of the corridor ahead of them. */
#define ROL_CRIB_X          (-600)
#define ROL_CRIB_Z             0
#define ROL_CRIB_ROT        1024     /* long axis along Z, back to the spine */

#define ROL_PILE_END_X        725
#define ROL_PILE_END_Y      (-200)   /* world (mesh) y of the drawn ridge tips */
#define ROL_PILE_END_Z        507    /* +north, -south */

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's floor. Its two floor faces are two slabs
   of one plane (see the FLOOR list at the foot of
   src/room_of_legs_mesh_collision.c) — x[-990,990] and the door's trapezoid out
   to x=1293, both z[-838,838] — so this is the Room of Heads' case. The rect
   also covers the inside of the pile, which the walls make unreachable. */
static void rol_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -990;  floor_zones[i].max_x = 1293;
    floor_zones[i].min_z = -838;  floor_zones[i].max_z = 838;
    floor_zones[i].y     = ROL_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, AND THE ROOM OWNS ONE.

     0 cobblestones        the outer walls, the floor and the vault
                           — 219 of the 283 polys         (x384 y0)
     1 legs                the pile, 62 polys             (x640 y0)  OWNED HERE
     2 catacomb inner door the one doorway, east          (x832 y0)

   Slots 0 and 2 come through src/catacombs_entry.c's narrow uploaders, as every
   Chapter 3 room takes them.

   >>> SLOT 1 TIME-SHARES THE ROOM OF ARMS' PAGE AND PALETTE. <<< x640 y0 and
   the CLUT at (672,501) are arms.tim's, and legs.tim is built to the same two
   addresses — as heads.tim already is, so the page now has three owners taking
   turns. None of the three rooms is ever drawn with another, and each one's
   uploader puts its own art back on every entry, so none owes the others a
   restore. The page's other occupants were already displaced by the arms and
   have their own restores (tools/vram_map.py lists them beside the pairs).

   128x128 STRETCHED from a 64x64 source (textures/catacombs/legs_128.png), the
   heads' treatment: the mesh puts ~330 world units on one UV tile (the heads'
   ~340, the cobble's ~510), so one copy per tile is what the artist drew. The
   UVs run to 255 and wrap under the 128 window, which is what repeats the legs
   across the pile.

   All three sit at Voff 0, so the one 128 texture window in the draw serves
   them. */
#define ROOM_OF_LEGS_TEX_COUNT 3

static uint16_t tex_tpage[ROOM_OF_LEGS_TEX_COUNT];
static uint16_t tex_clut[ROOM_OF_LEGS_TEX_COUNT];

/* The one texmgr entry this room owns (slot 1). -1 until registration, which is
   also what a registration past TEXMGR_MAX leaves — texmgr_upload on -1 is a
   no-op, and the legs draw as whatever was last on x640 y0 (arms or heads). */
static int legs_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/north_chamber.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int rol_key_count = 0;

static void rol_build_cull_keys(void) {
    rol_key_count = 0;
    if (!rol_smd) return;
    uint8_t *p = (uint8_t *)rol_smd->p_prims;
    int i, n = rol_smd->n_prims;
    if (n > ROOM_OF_LEGS_PRIM_COUNT) n = ROOM_OF_LEGS_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &rol_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    rol_key_count = n;
}

void room_of_legs_load_geometry(void) {
    rol_buff = room_arena_load("\\TEXCTCMB\\LEGROOM.SMD;1");
    rol_smd  = rol_buff ? smdInitData(rol_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (rol_smd && !room_data_bind(rol_smd->n_prims)) rol_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    rol_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: three compile-time headers and ONE
   deferred registration. The bank mask is derived, not guessed — py
   tools/check_tex_banks.py walks the uploader graph and fails if CATACOMBS is not
   in it. */
void room_of_legs_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    legs_tex = texmgr_register("\\TEXCTCMB\\LEGS.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, LEGS);
    TIM_SLOT(2, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> THE LEGS LINE IS WHAT OVERWRITES THE ARMS (OR THE HEADS). <<< Nothing else
   in this room draws x640 y0, and the Room of Arms' and the Room of Heads'
   uploaders each put their own art back on the way in there, so there is no
   ordering rule between the calls. */
/* LEGS.TIM alone, for the Meat Plant, which draws the legs but none of this
   room's other art: the full uploader below would stamp the crib and the Creep
   as well. */
void room_of_legs_upload_legs(void) {
    texmgr_upload(legs_tex);
}

void room_of_legs_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    room_of_legs_upload_legs();
    /* ...and the crib's and the Creep's, the two prop/enemy modules' narrow
       uploaders, exactly as the Room of Arms calls them. Their pages (x576 y0
       and x[960,1024) y0) are neither of this room's, so no ordering rule. */
    crib_upload_texture();
    creeps_upload_texture();
}

/* ---- THE EAST DOOR ---------------------------------------------------------
   x=1293, z[-107,107], y[-400,0] — in the octagon's east face, and the ONLY door
   this room has. Back into the Sliding Bars Room, through the NORTH of the two
   doors in its east wall.

   A door in the YZ plane at fixed X, approached from -X (wall 1 runs x=1293
   with nx=-4096), so TEXT_PLANE_YZ with mirror=1 and the sign 11 proud of the
   wall along -X — the Room of Heads' east door exactly. The reading axis for a YZ
   sign is Z, so the -200 door_draw_string_3d wants goes on the Z argument. */
#define ROL_EAST_X           1293
#define ROL_EAST_Z              0     /* the art spans z[-107,107] */
#define ROL_EAST_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define ROL_TEXT_RADIUS      1200
#define ROL_FADE_NEAR         800
#define ROL_TRIGGER_RADIUS    500

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int east_circle_prev = 1;

void room_of_legs_arm(void) {
    east_circle_prev = interact_tapped();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int room_of_legs_east_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !east_circle_prev;
    int32_t dx, dz, xz;
    east_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - ROL_EAST_X;
    dz = cam_z - ROL_EAST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= ROL_TRIGGER_RADIUS) return 0;
    if (!interact_facing(ROL_EAST_X, ROL_EAST_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within ROL_FADE_NEAR, gone by ROL_TEXT_RADIUS. */
static void rol_east_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - ROL_EAST_X;
    int32_t dz = cam_z - ROL_EAST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= ROL_TEXT_RADIUS) return;

    if (xz > ROL_FADE_NEAR) {
        int range = ROL_TEXT_RADIUS - ROL_FADE_NEAR;
        int prog  = xz - ROL_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        ROL_EAST_X - 11, ROL_EAST_TEXT_Y, ROL_EAST_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void room_of_legs_spawn_east(void) {
    /* 220 off wall 1 on its walkable -X side, on the corridor's centre line,
       facing -X — the direction of travel, down the corridor between the pile's
       two arms to the crib. (1073,0) is over 300 from every pile and funnel
       wall — the nearest is the pile corner (724,+-304), ~460 off — so the
       195 push is quiet. */
    cam_x   = ROL_EAST_X - (ROL_WALL_RADIUS + 25);
    cam_y   = ROL_EYE_Y;
    cam_vy  = 0;
    cam_z   = ROL_EAST_Z;
    cam_rot = 3072;                    /* facing -X, west into the room */
    room_of_legs_arm();
}

void room_of_legs_init(void) {
    room_data_collision(&current_collision_room);
    /* THE PILE IS SHOT OVER. Its proxy walls (and the two funnel walls off its
       ends) are 599 tall against 800 for the outer walls, but the DRAWN legs
       are only 200 high, so a shot has nothing to hit up there: anything 600 or
       under is shoot-over. The player still walks round the pile, but a shot
       (and an enemy's sightline) passes above — which is what lets the player
       shoot a Creep rising off one of the pile's ends. Set by HEIGHT, not by
       index, because the wall list is generated and reorders on re-export. */
    collision_shoot_over_short_walls(600);
    /* The vault, read off the VISUAL mesh: y=-800 over the whole octagon, where
       the proxy's outer walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(ROL_WALL_RADIUS);

    rol_floor_zones_init();
    cam_pitch = 0;

    room_of_legs_spawn_east();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside x/z[-1293,1293]. The Catacombs Entry's sconces at (+-595,200) land
       inside it, as they land inside the Room of Heads; sconces gate on area, so
       they do not bite, and this is the cheap guarantee rather than a fix. Safe
       to clear: catacombs_entry_init() re-places all four. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* THE CRIB. Cleared and re-placed on every entry, like the other two
       (crib.h: only the solved bit outlives a door, and crib_place reads it). */
    cribs_clear();
    crib_place(STATE_ROOM_OF_LEGS, ROL_CRIB_X, -GROUND_FLOOR_Y,
               ROL_CRIB_Z, ROL_CRIB_ROT);
    crib_set_outer_spawns(STATE_ROOM_OF_LEGS,
                          ROL_PILE_END_X, ROL_PILE_END_Y,  ROL_PILE_END_Z,
                          ROL_PILE_END_X, ROL_PILE_END_Y, -ROL_PILE_END_Z);

    rol_view_resolve(1);
}

static void draw_room_of_legs_smd(RenderContext *ctx) {
    if (!rol_smd) return;

    uint8_t *p = (uint8_t *)rol_smd->p_prims;
    int i, n = rol_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = rol_fog_far;   /* resolved by rol_view_resolve() this frame */
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
        SVECTOR *v0 = &rol_smd->p_verts[vi[0]];
        SVECTOR *v1 = &rol_smd->p_verts[vi[1]];
        SVECTOR *v2 = &rol_smd->p_verts[vi[2]];

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

        int nocull = (i < ROOM_OF_LEGS_PRIM_COUNT) && room_nocull(i);
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
            v3 = &rol_smd->p_verts[vi[3]];
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
           must not be sorted over the legs lying on it. */
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
        int32_t fog = dist < rol_fog_near ? rol_fog_near : (dist > rol_fog_far ? rol_fog_far : dist);
        int32_t fog_factor = ((rol_fog_far - fog) << 8) / (rol_fog_far - rol_fog_near);

        uint8_t tex_idx = (i < ROOM_OF_LEGS_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < ROOM_OF_LEGS_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + ROL_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + ROL_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + ROL_FOG_B * (256 - fog_factor)) >> 8);

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

void room_of_legs_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    rol_view_resolve(0);

    g_fog_near = rol_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : rol_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, ROL_FOG_R, ROL_FOG_G, ROL_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All three textures sit at Voff 0. The legs' UVs run past 127 and rely on
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

    if (exp != DBG_EXP_NO_MESH) draw_room_of_legs_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGN, THE CRIB AND THE ENEMIES. <<< */
    if (exp != DBG_EXP_NO_ENTITIES) {
        rol_east_door_text(ctx);   /* east: YZ plane, approached from -X */
        /* BOTH CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Both sheets sit at Voff 128, so each is handed the window to restore
           after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
            maggots_set_texwindow(&tw);
            creeps_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        draw_maggots(ctx);   /* area-tagged: free where none is placed */
        /* The crib, then what it pours — the Room of Arms' order and reasons:
           AFTER the two enemy calls, whose windows are restored for this
           Voff-0 art, and the creeps after the cot so a body in front of it
           wins the depth tie. */
        cribs_draw(ctx);
        draw_creeps(ctx);
    }
}
