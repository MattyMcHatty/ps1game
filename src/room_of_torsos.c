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
#include "room_of_torsos.h"
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "room_of_torsos_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "crib.h"              /* the cot in the north-west corner */
#include "creep.h"             /* ...and what it pours out when the axe wakes it */
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Room of Torsos — see room_of_torsos.h for the layout and the door. */

static SMD  *rto_smd  = NULL;
static void *rto_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 with +50%/+50% for the lantern — the Rooms of Heads', Legs' and
   Bones' numbers, for the same octagon. 283 primitives, the Room of Legs' count
   exactly, so there is no lag argument for pulling them in. */
#define RTO_BASE_FOG_NEAR   450
#define RTO_BASE_FOG_FAR   1600

#define RTO_VIEW_UNIT        256
#define RTO_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define RTO_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define RTO_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t rto_view     = RTO_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t rto_fog_near = RTO_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t rto_fog_far  = RTO_BASE_FOG_FAR;

static int32_t rto_view_target(void) {
    int32_t s = RTO_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += RTO_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += RTO_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void rto_view_resolve(int snap) {
    int32_t target = rto_view_target();
    if (snap) {
        rto_view = target;
    } else if (rto_view < target) {
        rto_view += RTO_VIEW_RATE;
        if (rto_view > target) rto_view = target;
    } else if (rto_view > target) {
        rto_view -= RTO_VIEW_RATE;
        if (rto_view < target) rto_view = target;
    }
    rto_fog_near = (RTO_BASE_FOG_NEAR * rto_view) >> 8;
    rto_fog_far  = (RTO_BASE_FOG_FAR  * rto_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define RTO_FOG_R             7
#define RTO_FOG_G             6
#define RTO_FOG_B             9

/* Wall standoff. The chapter's 195. The tightest place is between the
   south-west pile's north face (z=-99) and the north pile's south face (z=298),
   ~400 across and so ~0 to spare at 195 a side — the player goes round the
   north pile's east end instead, where there is room. Nothing here argues for a
   smaller standoff. */
#define RTO_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define RTO_FLOOR_Y            0
#define RTO_EYE_Y           (RTO_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- THE CRIB --------------------------------------------------------------
   The chapter's FIFTH crib encounter, the Room of Arms' cot and state machine
   unchanged: a crucifaxe swing wakes it, it rocks, lights, and pours ten Creeps.
   Its solved flag is THIS room's bit in the save — crib_room_solved(
   STATE_ROOM_OF_TORSOS) — separate from the other four rooms' (src/crib.h).

   WHERE: IN THE CORNER OPPOSITE THE DOOR, against the wall between two of the
   piles. The door is the middle of the east face, so straight across the room
   is the west face, and the two corners at its ends are (-1293,+-535). The
   south one is behind the south-west pile; the NORTH-WEST one is open floor,
   and the wall that meets it — the north-west chamfer, wall 17, (-535,1293) to
   (-1293,535) — is the stretch of the octagon between the south-west pile and
   the north pile. The cot stands against that chamfer, slid down it into the
   corner:

     rot_y 3584 (-45 degrees) lays the 350 long axis ALONG the chamfer, which
     runs (1,1) in plan. Its back face is 30 off the wall (the chamfer is
     z - x = 1828; both back corners, (-945,841) and (-1193,593), are at 1786),
     and its south-west end is 100 off the west face (x=-1193 against -1293).
     Neither gap is one the player can use: they are held 195 off the walls and
     75 off the cot, so the cot is struck from the room side only.

   Its baked AABB is the 45-degree one, x[-1192,-804] z[452,840] (the Room of
   Arms' 194 half-extent); the corner of it that pokes past the chamfer is
   outside the room and costs nothing. The nearest pile proxy, the north pile's
   west face (x=-107), is ~700 off.

   THE CREEPS COME FROM FOUR PLACES, the brief's: the cot, and the TIP of each
   pile of torsos.
     0  the cot's own centre, at its rim             (crib.c, every room)
     1  the SOUTH-WEST pile's tip (-725,-300,-304)   crib_set_outer_spawns()
     2  the SOUTH-EAST pile's tip ( 536,-300,-497)
     3  the NORTH pile's tip      ( 107,-300, 497)   crib_add_outer_spawn()
   Each tip is the single apex vertex of its pile in the VISUAL mesh — every
   other vertex of every pile is on the floor — so y=-300 is the drawn height,
   and the proxy's 299 agrees. Creeps do not collide (src/crib.c,
   crib_release), so a body starting at a tip simply floats down off it to its
   hover height. Four points, one in four each. */
#define RTO_CRIB_X          (-998)
#define RTO_CRIB_Z            646
#define RTO_CRIB_ROT         3584    /* -45 degrees: long axis along the chamfer */

#define RTO_TIP_Y           (-300)   /* world (mesh) y of all three drawn tips */
#define RTO_SW_TIP_X        (-725)
#define RTO_SW_TIP_Z        (-304)
#define RTO_SE_TIP_X          536
#define RTO_SE_TIP_Z        (-497)
#define RTO_N_TIP_X           107
#define RTO_N_TIP_Z           497

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's floor. Its three floor faces are three
   slabs of one plane (see the FLOOR list at the foot of
   src/room_of_torsos_mesh_collision.c) — x[-1293,-535], x[-535,535] and
   x[535,1293], all z[-1293,1293] — so this is the Room of Bones' case. The rect
   also covers the inside of the piles, which the walls make unreachable. */
static void rto_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1293;  floor_zones[i].max_x = 1293;
    floor_zones[i].min_z = -1293;  floor_zones[i].max_z = 1293;
    floor_zones[i].y     = RTO_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, AND THE ROOM OWNS ONE.

     0 cobblestones        the outer walls, the floor and the vault
                           — 250 of the 283 polys         (x384 y0)
     1 torsos              the three piles, 31 polys      (x640 y0)  OWNED HERE
     2 catacomb inner door the one doorway, east          (x832 y0)

   Slots 0 and 2 come through src/catacombs_entry.c's narrow uploaders, as every
   Chapter 3 room takes them.

   >>> SLOT 1 TIME-SHARES THE ROOM OF ARMS' PAGE AND PALETTE. <<< x640 y0 and
   the CLUT at (672,501) are arms.tim's, and torsos.tim is built to the same two
   addresses — as heads.tim, legs.tim and bones.tim already are, so the page now
   has five owners taking turns. None of the five rooms is ever drawn with
   another (the Meat Plant draws the legs, and stamps them itself on entry), and
   each one's uploader puts its own art back on every entry, so none owes the
   others a restore.

   128x128 AS AUTHORED (textures/catacombs/torsos.png, 16 colours), so unlike
   the legs and the bones there is no stretch: the source already fills the
   tile. The UVs run to 255 and wrap under the 128 window, which is what repeats
   the torsos across each pile.

   All three sit at Voff 0, so the one 128 texture window in the draw serves
   them. */
#define ROOM_OF_TORSOS_TEX_COUNT 3

static uint16_t tex_tpage[ROOM_OF_TORSOS_TEX_COUNT];
static uint16_t tex_clut[ROOM_OF_TORSOS_TEX_COUNT];

/* The one texmgr entry this room owns (slot 1). -1 until registration, which is
   also what a registration past TEXMGR_MAX leaves — texmgr_upload on -1 is a
   no-op, and the torsos draw as whatever was last on x640 y0 (arms, heads, legs
   or bones). It is the 79th registration of TEXMGR_MAX's 80. */
static int torsos_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/north_chamber.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int rto_key_count = 0;

static void rto_build_cull_keys(void) {
    rto_key_count = 0;
    if (!rto_smd) return;
    uint8_t *p = (uint8_t *)rto_smd->p_prims;
    int i, n = rto_smd->n_prims;
    if (n > ROOM_OF_TORSOS_PRIM_COUNT) n = ROOM_OF_TORSOS_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &rto_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    rto_key_count = n;
}

void room_of_torsos_load_geometry(void) {
    rto_buff = room_arena_load("\\TEXCTCMB\\TRSOROOM.SMD;1");
    rto_smd  = rto_buff ? smdInitData(rto_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (rto_smd && !room_data_bind(rto_smd->n_prims)) rto_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    rto_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: three compile-time headers and ONE
   deferred registration. The bank mask is derived, not guessed — py
   tools/check_tex_banks.py walks the uploader graph and fails if CATACOMBS is not
   in it. */
void room_of_torsos_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    torsos_tex = texmgr_register("\\TEXCTCMB\\TORSOS.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, TORSOS);
    TIM_SLOT(2, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> THE TORSOS LINE IS WHAT OVERWRITES THE ARMS (OR THE HEADS, THE LEGS, THE
   BONES). <<< Nothing else in this room draws x640 y0, and each of those rooms'
   uploaders puts its own art back on the way in there, so there is no ordering
   rule between the calls. */
void room_of_torsos_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    texmgr_upload(torsos_tex);
    /* ...and the crib's and the Creep's, the two prop/enemy modules' narrow
       uploaders, exactly as the Room of Arms calls them. Their pages (x576 y0
       and x[640,672) y[320,384)) are neither of this room's, so no ordering
       rule. */
    crib_upload_texture();
    creeps_upload_texture();
}

/* ---- THE EAST DOOR ---------------------------------------------------------
   x=1293, z[-107,107], y[-400,0] — in the octagon's east face, and the ONLY door
   this room has. Back into the H Corridor, through the south door at the foot
   of its east leg.

   A door in the YZ plane at fixed X, approached from -X (wall 10 runs x=1293
   with nx=-4096), so TEXT_PLANE_YZ with mirror=1 and the sign 11 proud of the
   wall along -X — the Room of Legs' east door exactly. The reading axis for a YZ
   sign is Z, so the -200 door_draw_string_3d wants goes on the Z argument. */
#define RTO_EAST_X           1293
#define RTO_EAST_Z              0     /* the art spans z[-107,107] */
#define RTO_EAST_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define RTO_TEXT_RADIUS      1200
#define RTO_FADE_NEAR         800
#define RTO_TRIGGER_RADIUS    500

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int east_circle_prev = 1;

void room_of_torsos_arm(void) {
    east_circle_prev = interact_tapped();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int room_of_torsos_east_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !east_circle_prev;
    int32_t dx, dz, xz;
    east_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - RTO_EAST_X;
    dz = cam_z - RTO_EAST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RTO_TRIGGER_RADIUS) return 0;
    if (!interact_facing(RTO_EAST_X, RTO_EAST_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within RTO_FADE_NEAR, gone by RTO_TEXT_RADIUS. */
static void rto_east_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - RTO_EAST_X;
    int32_t dz = cam_z - RTO_EAST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= RTO_TEXT_RADIUS) return;

    if (xz > RTO_FADE_NEAR) {
        int range = RTO_TEXT_RADIUS - RTO_FADE_NEAR;
        int prog  = xz - RTO_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        RTO_EAST_X - 11, RTO_EAST_TEXT_Y, RTO_EAST_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void room_of_torsos_spawn_east(void) {
    /* 220 off wall 10 on its walkable -X side, on the door's centre line,
       facing -X — the direction of travel, across the room to the crib.
       (1073,0) is ~460 from the nearest pile corner, the south-east pile's
       (724,-304), and ~530 from either eastern chamfer, so the 195 push is
       quiet. */
    cam_x   = RTO_EAST_X - (RTO_WALL_RADIUS + 25);
    cam_y   = RTO_EYE_Y;
    cam_vy  = 0;
    cam_z   = RTO_EAST_Z;
    cam_rot = 3072;                    /* facing -X, west into the room */
    room_of_torsos_arm();
}

void room_of_torsos_init(void) {
    room_data_collision(&current_collision_room);
    /* THE PILES ARE SHOT OVER. Their proxy walls are 299 tall against 800 for
       the outer walls, and the DRAWN piles are pointed: only the one apex of
       each reaches -300, and their slopes are well under eye height (-189) over
       most of their footprint. So anything 400 or under is shoot-over — the
       player still walks round the piles, but a shot (and an enemy's
       sightline) passes above, which is what lets the player shoot a Creep
       rising off a pile's tip. Set by HEIGHT, not by index, because the wall
       list is generated and reorders on re-export. */
    collision_shoot_over_short_walls(400);
    /* The vault, read off the VISUAL mesh: y=-800 over the whole octagon, where
       the proxy's outer walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(RTO_WALL_RADIUS);

    rto_floor_zones_init();
    cam_pitch = 0;

    room_of_torsos_spawn_east();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside x/z[-1293,1293]. The Catacombs Entry's sconces at (+-595,200) land
       inside it, as they land inside the Rooms of Heads, Legs and Bones; sconces
       gate on area, so they do not bite, and this is the cheap guarantee rather
       than a fix. Safe to clear: catacombs_entry_init() re-places all four. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* THE CRIB. Cleared and re-placed on every entry, like the other four
       (crib.h: only the solved bit outlives a door, and crib_place reads it).
       Its three outer points are the three pile tips: two through
       crib_set_outer_spawns(), which resets the list, and the third appended. */
    cribs_clear();
    crib_place(STATE_ROOM_OF_TORSOS, RTO_CRIB_X, -GROUND_FLOOR_Y,
               RTO_CRIB_Z, RTO_CRIB_ROT);
    crib_set_outer_spawns(STATE_ROOM_OF_TORSOS,
                          RTO_SW_TIP_X, RTO_TIP_Y, RTO_SW_TIP_Z,
                          RTO_SE_TIP_X, RTO_TIP_Y, RTO_SE_TIP_Z);
    crib_add_outer_spawn(STATE_ROOM_OF_TORSOS,
                         RTO_N_TIP_X, RTO_TIP_Y, RTO_N_TIP_Z);

    rto_view_resolve(1);
}

static void draw_room_of_torsos_smd(RenderContext *ctx) {
    if (!rto_smd) return;

    uint8_t *p = (uint8_t *)rto_smd->p_prims;
    int i, n = rto_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = rto_fog_far;   /* resolved by rto_view_resolve() this frame */
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
        SVECTOR *v0 = &rto_smd->p_verts[vi[0]];
        SVECTOR *v1 = &rto_smd->p_verts[vi[1]];
        SVECTOR *v2 = &rto_smd->p_verts[vi[2]];

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

        int nocull = (i < ROOM_OF_TORSOS_PRIM_COUNT) && room_nocull(i);
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
            v3 = &rto_smd->p_verts[vi[3]];
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
           must not be sorted over the torsos piled on it. */
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
        int32_t fog = dist < rto_fog_near ? rto_fog_near : (dist > rto_fog_far ? rto_fog_far : dist);
        int32_t fog_factor = ((rto_fog_far - fog) << 8) / (rto_fog_far - rto_fog_near);

        uint8_t tex_idx = (i < ROOM_OF_TORSOS_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < ROOM_OF_TORSOS_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + RTO_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + RTO_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + RTO_FOG_B * (256 - fog_factor)) >> 8);

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

void room_of_torsos_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    rto_view_resolve(0);

    g_fog_near = rto_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : rto_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, RTO_FOG_R, RTO_FOG_G, RTO_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All three textures sit at Voff 0. The torsos' UVs run past 127 and rely
       on it. */
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

    if (exp != DBG_EXP_NO_MESH) draw_room_of_torsos_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGN, THE CRIB AND THE ENEMIES. <<< */
    if (exp != DBG_EXP_NO_ENTITIES) {
        rto_east_door_text(ctx);   /* east: YZ plane, approached from -X */
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
