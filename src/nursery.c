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
#include "nursery.h"
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "nursery_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "crib.h"               /* the ring of six mirror cots */
#include "gula_tablet.h"        /* the slab across the west door */
#include "player.h"             /* current_weapon, player_weapons, show_pickup_msg_raw */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Nursery — see nursery.h for the layout, the door and the cots. */

static SMD  *nur_smd  = NULL;
static void *nur_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 — the Rooms of Arms', Heads', Legs', Bones' and Torsos' base, for
   the same octagon.

   >>> BUT THE LANTERN'S BONUS IS CAPPED HERE: ~+19%/+19%, NOT +50%/+50%. <<<
   Those rooms reach 3200 with the Helluminator lit, which in a 2586-wide
   octagon is the whole room — and here the whole room is six lit cots. At 3200
   every beam was drawn from anywhere, on top of the lantern's own additive
   flame, and the room lagged (October 2026). 1600 x (256+48+48)/256 = 2200
   still shows the far side of the ring from the middle, while a cot across the
   room from a player standing at the wall falls past the cull and its beam,
   fading with the fog (src/crib.c), goes with it. So the lantern still visibly
   widens the view in here, just by less than anywhere else in the chapter. */
#define NUR_BASE_FOG_NEAR   450
#define NUR_BASE_FOG_FAR   1600

#define NUR_VIEW_UNIT        256
#define NUR_VIEW_HELL_BONUS   48   /* ~+19% while the lantern is in hand  */
#define NUR_VIEW_BURN_BONUS   48   /* ~+19% more while it is actually lit */
#define NUR_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t nur_view     = NUR_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t nur_fog_near = NUR_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t nur_fog_far  = NUR_BASE_FOG_FAR;

static int32_t nur_view_target(void) {
    int32_t s = NUR_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += NUR_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += NUR_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void nur_view_resolve(int snap) {
    int32_t target = nur_view_target();
    if (snap) {
        nur_view = target;
    } else if (nur_view < target) {
        nur_view += NUR_VIEW_RATE;
        if (nur_view > target) nur_view = target;
    } else if (nur_view > target) {
        nur_view -= NUR_VIEW_RATE;
        if (nur_view < target) nur_view = target;
    }
    nur_fog_near = (NUR_BASE_FOG_NEAR * nur_view) >> 8;
    nur_fog_far  = (NUR_BASE_FOG_FAR  * nur_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define NUR_FOG_R             7
#define NUR_FOG_G             6
#define NUR_FOG_B             9

/* Wall standoff. The chapter's 195. The octagon is empty but for the ring of
   cots, which are props at the crib's 75, so nothing argues for anything else. */
#define NUR_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define NUR_FLOOR_Y            0
#define NUR_EYE_Y           (NUR_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- THE SIX COTS ----------------------------------------------------------
   ONE ON EACH OF THE SIX GULA-TABLET FLOOR TILES — the six polys of
   Nursery.smx textured `gula tablet`, all at y=0. Each (x, z) below is that
   tile's centre in plan, read off the export, and each rot_y lays the cot's
   350 LONG AXIS along the line from the tile to the room's centre, so the six
   stand lengthways toward the middle like the spokes of a wheel. rot_y is
   atan2(z, -x) in PS1 units: crib_place() maps model +X to world
   (cos, -sin), and that has to point at (-x, -z).

       tile         centre       radius   rot_y   lit by
       north        (   0,  796)   796    1024    the Room of Arms' crib
       north-east   ( 630,  597)   868    1554    the Room of Heads' crib
       south-east   ( 630, -597)   868    2542    the Room of Legs' crib
       south        (   0, -796)   796    3072    the Room of Bones' crib
       south-west   (-630, -597)   868    3602    the Room of Torsos' crib
       north-west   (-630,  597)   868     494    the Room of Guts' crib

   THESE ARE NOT ENCOUNTERS. Every one is a crib_place_mirror() (src/crib.h,
   THE NURSERY'S MIRRORS): it carries the encounter's beam at full, held still,
   for as long as its paired room's crib_room_solved() bit is set — read live,
   so it is lit on entry if that crib was beaten earlier — and is dark
   otherwise. A crucifaxe swing rocks it once and does nothing else: no Creeps,
   no loop, no change to its light.

   THE NORTH-WEST COT WAS THE LAST TO BE PAIRED: the sixth crib is the Room of
   Guts' (src/room_of_guts.c), behind the Throat.

   >>> ALL SIX LIT RAISES THE GULA TABLET. <<< When every cot's paired room is
   solved, nursery_init() lifts the slab NUR_TABLET_RISE off the west door, so
   the door behind it shows on the NEXT entry after the last crib is beaten —
   which is necessarily a later entry, since the last crib is in another room.
   Nothing new is saved: it is the six crib bits, read on entry (nur_all_lit).

   CLEARANCES. The cots' outer ends sit at radius ~970 (north/south) and ~1040
   (the four diagonals) against an apothem of 1293, so the nearest wall is
   ~250-320 off. The east door's arrival (1073,0) is ~250 clear of the nearest
   box, the north-east cot's x[435,827] z[404,790], and the straight walk in
   from the door to the centre passes between the two eastern cots.

   SIX BEAMS AT ONCE LAGGED, with the Helluminator lit (October 2026). Three
   things came off it together: each beam now draws only the walls facing the
   camera and is a third shorter (src/crib.h — at most 12 additive quads a cot,
   from 36), and this room caps the lantern's view bonus (View distance, above).
   If it is still slow, read tools/DIAGNOSING_FRAME_RATE.txt and measure U/D/G
   before taking anything else off. */
#define NUR_CRIB_N_Z           796
#define NUR_CRIB_DIAG_X        630
#define NUR_CRIB_DIAG_Z        597

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's floor. Its three floor faces are three
   slabs of one plane (see the FLOOR list at the foot of
   src/nursery_mesh_collision.c) — x[-1293,-535], x[-535,535] and
   x[535,1293], all z[-1293,1293] — so this is the Room of Torsos' case. */
static void nur_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1293;  floor_zones[i].max_x = 1293;
    floor_zones[i].min_z = -1293;  floor_zones[i].max_z = 1293;
    floor_zones[i].y     = NUR_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, AND THE ROOM OWNS NONE OF THEM.

     0 cobblestones        the walls and the floor
                           — 301 of the 311 polys          (x384 y0)
     1 gula tablet         the six floor tiles under the
                           cots, 4bpp                      (x640 y0)
     2 catacomb inner door the two doorways, east and west (x832 y0)

   Slots 0 and 2 come through src/catacombs_entry.c's narrow uploaders, as every
   Chapter 3 room takes them. Slot 1 is the GULA TABLET PROP's texture, owned
   by src/gula_tablet.c and put up by its narrow uploader — the room's tiles and
   the prop draw the same \TEXCTCMB\GULATBLT.TIM, so it is registered once.

   >>> SLOT 1 TIME-SHARES THE ROOM OF ARMS' PAGE, NOT ITS PALETTE. <<< 4bpp,
   so it covers x[640,672) only, the gaol door's terms exactly: the arms,
   heads, legs, bones, torsos, the Shelf's panel and the gaol door all take
   turns on x640 y0, each room puts its own art back on entry, and none of them
   is drawn here. Its CLUT is its own, at (592,502) beside the gaol door's.

   All three sit at Voff 0, so the one 128 texture window in the draw serves
   them — and the cots (x576 y0) and the tablet prop with them. */
#define NURSERY_TEX_COUNT 3

static uint16_t tex_tpage[NURSERY_TEX_COUNT];
static uint16_t tex_clut[NURSERY_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/room_of_torsos.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int nur_key_count = 0;

static void nur_build_cull_keys(void) {
    nur_key_count = 0;
    if (!nur_smd) return;
    uint8_t *p = (uint8_t *)nur_smd->p_prims;
    int i, n = nur_smd->n_prims;
    if (n > NURSERY_PRIM_COUNT) n = NURSERY_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &nur_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    nur_key_count = n;
}

void nursery_load_geometry(void) {
    nur_buff = room_arena_load("\\TEXCTCMB\\NURSERY.SMD;1");
    nur_smd  = nur_buff ? smdInitData(nur_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (nur_smd && !room_data_bind(nur_smd->n_prims)) nur_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    nur_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: three compile-time headers and no
   registration at all — every page is somebody else's. It still declares the
   bank, which py tools/check_tex_banks.py checks against the uploader graph. */
void nursery_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, GULATBLT);
    TIM_SLOT(2, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> THE GULA TABLET LINE IS WHAT OVERWRITES THE ARMS' PAGE (OR WHICHEVER OF
   ITS SEVEN OWNERS WAS LAST). <<< Nothing else in this room draws x640 y0, and
   each of those rooms' uploaders puts its own art back on the way in there. */
void nursery_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    gula_tablet_upload_texture();
    /* ...and the cots'. Their page (x576 y0, CLUT (256,484)) is the one the
       Gaol Cells' mud borrows, so arriving from the cells this line is also
       what puts the crib back. */
    crib_upload_texture();
}

/* ---- THE EAST DOOR ---------------------------------------------------------
   x=1293, z[-107,107], y[-400,0] — in the octagon's east face, and the only
   door the player can use. Back into the Gaol Cells, through the catacomb door
   in the south-east corner of their south wall.

   A door in the YZ plane at fixed X, approached from -X (wall 6 runs x=1293
   with nx=-4095), so TEXT_PLANE_YZ with mirror=1 and the sign 11 proud of the
   wall along -X — the Room of Torsos' east door exactly. The reading axis for a
   YZ sign is Z, so the -200 door_draw_string_3d wants goes on the Z argument.

   THE WEST DOOR (x=-1293, the same span) is drawn and SEALED: the Gula Tablet
   stands across it (src/gula_tablet.h). No door sign, no door trigger — but
   the tablet itself reads: "Press O to read" in front of its east face, and a
   Circle press posts its inscription to the log (nursery_update).

   WITH ALL SIX COTS LIT the tablet stands lifted off the door and the door is
   SHOWN, but it is still not wired: no sign, no trigger, nowhere to go yet —
   and the proxy's west wall still runs across it. Wiring it is a door block
   here, a pending_area in main.c, and a gap in the collision proxy. The read
   prompt is withdrawn while it is lifted (nur_tablet_raised). */
#define NUR_EAST_X           1293
#define NUR_EAST_Z              0     /* the art spans z[-107,107] */
#define NUR_EAST_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define NUR_TEXT_RADIUS      1200
#define NUR_FADE_NEAR         800
#define NUR_TRIGGER_RADIUS    500

/* THE GULA TABLET'S FACE: its east side, x=-1242 as authored, z[-300,300].
   The sign stands 40 proud of it along +X (11 sat it inside the slab's
   ordering table depth and it blended into the face) — the Room of Baby
   Names' west door's terms (YZ plane, approached from +X, mirror=0) — at the doors' eye
   height, so it reads as a prompt in front of the slab. */
#define NUR_TABLET_X        (-1242)
#define NUR_TABLET_Z            0
#define NUR_TABLET_TEXT_Y    (-186)

static const char NUR_TABLET_READ[] = "Release my children from this earthly burden";

/* THE REVEAL: how far the slab lifts once all six cots are lit. The west
   doorway is y[-400,0] and the slab is 800 tall, so 400 puts its base exactly
   on the lintel and its top at y=-1200, the top of the octagon's stone (see
   nursery_init's ceiling) — the door is uncovered and the slab stays inside the
   wall it is set into. Lifted, its base is far above the player's head, so it
   stops blocking (gula_tablets_collide). */
#define NUR_TABLET_RISE       400

/* Set by nursery_init(): every one of the six cots' rooms solved. While it is,
   the tablet stands raised and its "Press O to read" prompt — which would hang
   in the open doorway under the lifted slab — is not offered. */
static int nur_tablet_raised = 0;

/* The six crib bits the ring mirrors, in THE SIX COTS' table order. */
static int nur_all_lit(void) {
    return crib_room_solved(STATE_ROOM_OF_ARMS)   &&
           crib_room_solved(STATE_ROOM_OF_HEADS)  &&
           crib_room_solved(STATE_ROOM_OF_LEGS)   &&
           crib_room_solved(STATE_ROOM_OF_BONES)  &&
           crib_room_solved(STATE_ROOM_OF_TORSOS) &&
           crib_room_solved(STATE_ROOM_OF_GUTS);
}

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int east_circle_prev   = 1;
static int tablet_circle_prev = 1;

void nursery_arm(void) {
    east_circle_prev   = interact_tapped();
    tablet_circle_prev = interact_tapped();
}

int nursery_update(int lock) {
    int held = interact_tapped();
    int just = held && !tablet_circle_prev;
    int32_t dx, dz, xz;
    tablet_circle_prev = held;
    if (lock || !just || nur_tablet_raised) return 0;
    dx = cam_x - NUR_TABLET_X;
    dz = cam_z - NUR_TABLET_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= NUR_TRIGGER_RADIUS) return 0;
    if (!interact_facing(NUR_TABLET_X, NUR_TABLET_Z)) return 0;
    show_pickup_msg_raw(NUR_TABLET_READ);
    return 1;
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int nursery_east_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !east_circle_prev;
    int32_t dx, dz, xz;
    east_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - NUR_EAST_X;
    dz = cam_z - NUR_EAST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= NUR_TRIGGER_RADIUS) return 0;
    if (!interact_facing(NUR_EAST_X, NUR_EAST_Z)) return 0;
    return 1;
}

/* The tablet's floating sign, the door's fade below on the tablet's face. */
static void nur_tablet_text(RenderContext *ctx) {
    int32_t dx = cam_x - NUR_TABLET_X;
    int32_t dz = cam_z - NUR_TABLET_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= NUR_TEXT_RADIUS || nur_tablet_raised) return;

    if (xz > NUR_FADE_NEAR) {
        int range = NUR_TEXT_RADIUS - NUR_FADE_NEAR;
        int prog  = xz - NUR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to read",
                        NUR_TABLET_X + 40, NUR_TABLET_TEXT_Y, NUR_TABLET_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within NUR_FADE_NEAR, gone by NUR_TEXT_RADIUS. */
static void nur_east_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - NUR_EAST_X;
    int32_t dz = cam_z - NUR_EAST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= NUR_TEXT_RADIUS) return;

    if (xz > NUR_FADE_NEAR) {
        int range = NUR_TEXT_RADIUS - NUR_FADE_NEAR;
        int prog  = xz - NUR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        NUR_EAST_X - 11, NUR_EAST_TEXT_Y, NUR_EAST_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void nursery_spawn_east(void) {
    /* 220 off wall 6 on its walkable -X side, on the door's centre line,
       facing -X — the direction of travel, into the ring of cots. (1073,0) is
       ~250 off the nearest cot's box (the north-east one, x[435,827]
       z[404,790]) and ~530 from either eastern chamfer, so the 195 push and
       the cots' 75 are both quiet. */
    cam_x   = NUR_EAST_X - (NUR_WALL_RADIUS + 25);
    cam_y   = NUR_EYE_Y;
    cam_vy  = 0;
    cam_z   = NUR_EAST_Z;
    cam_rot = 3072;                    /* facing -X, west into the room */
    nursery_arm();
}

void nursery_init(void) {
    room_data_collision(&current_collision_room);
    /* THE WALLS, read off the VISUAL mesh: the octagon's stone rises to
       y=-1200 with no drawn ceiling over it, where the proxy's walls stop too.
       Nothing in the room is low enough to shoot over. */
    collision_set_ceiling_y(-1200);
    collision_set_wall_radius(NUR_WALL_RADIUS);

    nur_floor_zones_init();
    cam_pitch = 0;

    nursery_spawn_east();

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

    /* THE SIX COTS, each a MIRROR of one crib elsewhere, lit while that room's
       solved bit is set (THE SIX COTS above has the table; src/crib.h the
       mechanism). Cleared and re-placed on every entry like every crib;
       nothing about them is saved here — the light is the other rooms' bits,
       read live. */
    cribs_clear();
    crib_place_mirror(STATE_NURSERY,  0,               -GROUND_FLOOR_Y,
                       NUR_CRIB_N_Z,    1024, 1, STATE_ROOM_OF_ARMS);
    crib_place_mirror(STATE_NURSERY,  NUR_CRIB_DIAG_X, -GROUND_FLOOR_Y,
                       NUR_CRIB_DIAG_Z, 1554, 1, STATE_ROOM_OF_HEADS);
    crib_place_mirror(STATE_NURSERY,  NUR_CRIB_DIAG_X, -GROUND_FLOOR_Y,
                      -NUR_CRIB_DIAG_Z, 2542, 1, STATE_ROOM_OF_LEGS);
    crib_place_mirror(STATE_NURSERY,  0,               -GROUND_FLOOR_Y,
                      -NUR_CRIB_N_Z,    3072, 1, STATE_ROOM_OF_BONES);
    crib_place_mirror(STATE_NURSERY, -NUR_CRIB_DIAG_X, -GROUND_FLOOR_Y,
                      -NUR_CRIB_DIAG_Z, 3602, 1, STATE_ROOM_OF_TORSOS);
    crib_place_mirror(STATE_NURSERY, -NUR_CRIB_DIAG_X, -GROUND_FLOOR_Y,
                       NUR_CRIB_DIAG_Z,  494, 1, STATE_ROOM_OF_GUTS);

    /* THE GULA TABLET, across the west door, where its export stands it —
       or, with all six cots lit, lifted clear of the door (NUR_TABLET_RISE). */
    gula_tablets_clear();
    gula_tablet_place(STATE_NURSERY);
    nur_tablet_raised = nur_all_lit();
    if (nur_tablet_raised) gula_tablet_set_rise(NUR_TABLET_RISE);

    nur_view_resolve(1);
}

static void draw_nursery_smd(RenderContext *ctx) {
    if (!nur_smd) return;

    uint8_t *p = (uint8_t *)nur_smd->p_prims;
    int i, n = nur_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = nur_fog_far;   /* resolved by nur_view_resolve() this frame */
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
        SVECTOR *v0 = &nur_smd->p_verts[vi[0]];
        SVECTOR *v1 = &nur_smd->p_verts[vi[1]];
        SVECTOR *v2 = &nur_smd->p_verts[vi[2]];

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

        int nocull = (i < NURSERY_PRIM_COUNT) && room_nocull(i);
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
            v3 = &nur_smd->p_verts[vi[3]];
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
           must not be sorted over the cots standing on it. */
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
        int32_t fog = dist < nur_fog_near ? nur_fog_near : (dist > nur_fog_far ? nur_fog_far : dist);
        int32_t fog_factor = ((nur_fog_far - fog) << 8) / (nur_fog_far - nur_fog_near);

        uint8_t tex_idx = (i < NURSERY_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < NURSERY_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + NUR_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + NUR_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + NUR_FOG_B * (256 - fog_factor)) >> 8);

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

void nursery_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    nur_view_resolve(0);

    g_fog_near = nur_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : nur_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, NUR_FOG_R, NUR_FOG_G, NUR_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All three textures sit at Voff 0, and so do the cots and the tablet. */
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

    if (exp != DBG_EXP_NO_MESH) draw_nursery_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGN, THE COTS, THE TABLET AND THE ENEMIES. <<<
       Six lit cots are six beams of up to 12 additive quads: if this room is slow,
       this is the level that says whether they are why. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        nur_east_door_text(ctx);   /* east: YZ plane, approached from -X */
        nur_tablet_text(ctx);      /* the tablet: YZ plane, approached from +X */
        /* THE CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Their sheets sit at Voff 128, so each is handed the window to restore
           after drawing unmasked. No Creeps: a mirror cot never pours one. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
            maggots_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        draw_maggots(ctx);   /* area-tagged: free where none is placed */
        /* The tablet and the cots AFTER the enemy calls, whose windows are
           restored for this Voff-0 art — the Room of Arms' order. */
        gula_tablets_draw(ctx);
        cribs_draw(ctx);
    }
}
