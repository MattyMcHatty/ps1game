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
#include "north_chamber.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "north_chamber_mesh_collision.h"
#include "north_chamber_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "bars.h"               /* ...and the bars prop's, for the cage       */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The North Chamber — see north_chamber.h for the layout, the two storeys and
   the door list. */

static SMD  *nc_smd  = NULL;
static void *nc_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   >>> THE NUMBERS ARE THE UP DOWN MAZE'S, NOT THE PIT'S, AND THAT WAS A LAG
   REPORT. <<< The room first shipped on the chapter's 450/1600 with +50%/+50%
   for the lantern (1600 / 2400 / 3200), and raising the Helluminator lagged:
   1251 primitives, the second biggest mesh in the chapter, and at 2400-3200 the
   ring cull keeps nearly all of a 3600-square room from the south door. The Up
   Down Maze — the one bigger mesh — had already pulled its view in for the same
   reason, so this room takes its figures whole: 366/1300 base, +118/256 per
   lantern step, which resolves to 1300 / 1899 / 2498. The near distance keeps
   the same ratio to the far one as before, so the fog ramp reads the same.

   What it costs: from the south door the north wall and the ramp are in the
   dark even with the lantern burning; the far half of the room is walked into
   rather than seen from the door. If it still lags, measure U/D/G from the south
   door with the lantern burning (STEP 3J of tools/DIAGNOSING_FRAME_RATE.txt)
   before lowering these further. */
#define NC_BASE_FOG_NEAR    366
#define NC_BASE_FOG_FAR    1300

#define NC_VIEW_UNIT        256
#define NC_VIEW_HELL_BONUS  118   /* ~+46% while the lantern is in hand   */
#define NC_VIEW_BURN_BONUS  118   /* ~+46% more while it is actually lit  */
#define NC_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t nc_view     = NC_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t nc_fog_near = NC_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t nc_fog_far  = NC_BASE_FOG_FAR;

static int32_t nc_view_target(void) {
    int32_t s = NC_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += NC_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += NC_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void nc_view_resolve(int snap) {
    int32_t target = nc_view_target();
    if (snap) {
        nc_view = target;
    } else if (nc_view < target) {
        nc_view += NC_VIEW_RATE;
        if (nc_view > target) nc_view = target;
    } else if (nc_view > target) {
        nc_view -= NC_VIEW_RATE;
        if (nc_view < target) nc_view = target;
    }
    nc_fog_near = (NC_BASE_FOG_NEAR * nc_view) >> 8;
    nc_fog_far  = (NC_BASE_FOG_FAR  * nc_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). The six untextured polys of the shaft over the ladder
   are authored 0,0,0 and fog toward this, so they read as a hole into darkness
   rather than a seam. */
#define NC_FOG_R              7
#define NC_FOG_G              6
#define NC_FOG_B              9

/* Wall standoff. The chapter's 195. The gallery's arms are 600 wide, which
   leaves a 210 band between the outer wall and the standoff line — the same
   210 as The Pit's ledge — but here the inner edge has no wall at all, so the
   band is only a guide: step past it and the player drops to the ground. */
#define NC_WALL_RADIUS      195

/* The two floor heights, as world Y. -Y is up. */
#define NC_GALLERY_Y      (-1000)
#define NC_GROUND_Y            0

/* Standing eye on the ground floor: less GROUND_FLOOR_Y and the 40-unit standoff
   apply_height applies. */
#define NC_GROUND_EYE_Y   (NC_GROUND_Y - GROUND_FLOOR_Y - 40)

/* Halfway between the two floors. A cam_y below this line (numerically greater)
   is standing on the ground; the ramp crosses it, and a player halfway up the
   ramp is on neither storey as far as the south door is concerned, which is
   right — the ramp is 3000 from the door. */
#define NC_STOREY_SPLIT_Y ((NC_GALLERY_Y + NC_GROUND_Y) / 2)

/* ---- Floor zones -----------------------------------------------------------
   THE UP DOWN MAZE'S SHAPE: the gallery stands directly over the ground floor,
   so apply_height()'s "first zone that is not above the player" is what decides
   which storey the player is on, and the ORDER below is the whole mechanism.
   Gallery first, then the ramp, then the ground as one catch-all. Reverse them
   and the gallery is unstandable — silently, because the player simply stands on
   the ground with the walkway drawn around their head.

   THE GALLERY RECTS ARE THE PROXY'S FLOOR FACES EXACTLY, AND THAT IS THE
   OPPOSITE OF THE PIT'S RULE, ON PURPOSE. The Pit widens its gallery zones
   because a missed XZ there is a silent 1000-unit drop onto a floor with no way
   back up. Here stepping off the gallery is MEANT to drop the player — there is
   no railing, and the ramp is the way back up — so a zone widened past the edge
   would leave them standing on air instead. The outside edges need no margin:
   the walls hold the player 195 inside them.

   THE RAMP rises along +X, y 0 at x=-200 to y -1000 at x=1800, over the proxy's
   face exactly. apply_height skips a ramp surface more than 150 above the eye,
   which is what lets the player stand in the pocket under the gallery's east
   arm (x[1800,2400] z[3000,3600], ground level) at the ramp's head without
   being lifted onto it.

   THE GROUND is one FLOOR_FLAT over the box and both alcoves,
   x[-1800,3000] z[0,3600]. It also covers the cage and the void past the
   alcoves, which the walls make unreachable. */
static void north_chamber_floor_zones_init(void) {
    int i = 0;

    /* ---- THE GALLERY, y=-1000. First; see the note above. ---------------- */
    floor_zones[i].type  = FLOOR_UPPER;          /* south gallery          */
    floor_zones[i].min_x = -1200; floor_zones[i].max_x =  2400;
    floor_zones[i].min_z =     0; floor_zones[i].max_z =   600;
    floor_zones[i].y     = NC_GALLERY_Y;
    i++;

    floor_zones[i].type  = FLOOR_UPPER;          /* west arm               */
    floor_zones[i].min_x = -1200; floor_zones[i].max_x =  -600;
    floor_zones[i].min_z =   600; floor_zones[i].max_z =  3600;
    floor_zones[i].y     = NC_GALLERY_Y;
    i++;

    floor_zones[i].type  = FLOOR_UPPER;          /* east arm               */
    floor_zones[i].min_x =  1800; floor_zones[i].max_x =  2400;
    floor_zones[i].min_z =   600; floor_zones[i].max_z =  3600;
    floor_zones[i].y     = NC_GALLERY_Y;
    i++;

    floor_zones[i].type  = FLOOR_UPPER;          /* ladder alcove          */
    floor_zones[i].min_x =  2400; floor_zones[i].max_x =  2800;
    floor_zones[i].min_z =  1400; floor_zones[i].max_z =  2000;
    floor_zones[i].y     = NC_GALLERY_Y;
    i++;

    floor_zones[i].type  = FLOOR_UPPER;          /* bridge to the platform */
    floor_zones[i].min_x =   400; floor_zones[i].max_x =   800;
    floor_zones[i].min_z =   600; floor_zones[i].max_z =  1200;
    floor_zones[i].y     = NC_GALLERY_Y;
    i++;

    floor_zones[i].type  = FLOOR_UPPER;          /* platform over the cage */
    floor_zones[i].min_x =     0; floor_zones[i].max_x =  1200;
    floor_zones[i].min_z =  1200; floor_zones[i].max_z =  2400;
    floor_zones[i].y     = NC_GALLERY_Y;
    i++;

    /* ---- THE RAMP, along the north wall. -------------------------------- */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = -200; floor_zones[i].max_x = 1800;
    floor_zones[i].min_z = 3000; floor_zones[i].max_z = 3600;
    floor_zones[i].ramp_y_start    = NC_GROUND_Y;    /* y at x=-200 (the foot) */
    floor_zones[i].ramp_y_end      = NC_GALLERY_Y;   /* y at x=1800 (the head) */
    floor_zones[i].ramp_axis_start = -200;
    floor_zones[i].ramp_axis_end   = 1800;
    floor_zones[i].ramp_along_x    = 1;
    i++;

    /* ---- THE GROUND, y=0. Last: the catch-all. -------------------------- */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1800; floor_zones[i].max_x =  3000;
    floor_zones[i].min_z =     0; floor_zones[i].max_z =  3600;
    floor_zones[i].y     = NC_GROUND_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   FOUR, AND THE ROOM OWNS ONE.

     0 cobblestones        floors, walls, galleries, ramp and vault — 1144 of
                           the 1251 polys                (x384 y0)
     1 catacomb inner door the two doorways              (x832 y0)
     2 bars                the cage under the platform, 94 polys
                           (x512 y256, 4bpp, see-through where the PNG is)
     3 ladder              the ladder up the east alcove, 3 polys
                           (x704 y256)                   OWNED HERE

   Slots 0 and 1 come through src/catacombs_entry.c's narrow uploaders and slot 2
   through src/bars.c's, exactly as The Pit takes all three.

   >>> SLOT 3 TIME-SHARES THE INCINERATOR'S PAGE AND PALETTE. <<< There is no
   whole mesh-art page left in this bank (src/the_pit.c took the last one), so
   the ladder takes a page INSIDE the chapter instead: x704 y256, the Incinerator
   machine's, and the machine's CLUT row at (304,511) with it. The two are never
   drawn in the same room, and incinerator_room_upload_textures() puts the
   machine's pixels and palette back on every entry to the Incinerator Room —
   which is also the only room that draws it. The page's other occupants, Rabisu
   tex and vines, are Chapter 1/2 art the incinerator already displaces, with
   their own restores. The pairs are registered in tools/vram_map.py.

   128x128 STRETCHED from a 64x64 source (textures/catacombs/ladder_128.png).
   The ladder's UVs run u 0..128 across its whole 200-unit width, so one copy per
   tile is what the artist drew, and tiling 2x2 would put two ladders side by
   side. The V range runs past 127 and wraps under the 128 window, which is what
   repeats the rungs up the 800-unit climb.

   All four sit at Voff 0, so the one 128 texture window in the draw serves
   them. */
#define NORTH_CHAMBER_TEX_COUNT 4

static uint16_t tex_tpage[NORTH_CHAMBER_TEX_COUNT];
static uint16_t tex_clut[NORTH_CHAMBER_TEX_COUNT];

/* The one texmgr entry this room owns (slot 3). -1 until registration, which is
   also what a registration past TEXMGR_MAX leaves — texmgr_upload on -1 is a
   no-op, and the ladder draws as the incinerator. */
static int ladder_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/the_pit.c. The keys go in the SHARED arena (src/cull_arena.h)
   and are built on the same call that reloads the mesh they describe. No box key
   and no side-plane cull, for the reason every room since the Master Bedroom
   records: nothing here would feed one, and "the room is open so it would cull
   a lot" is wrong turn #2. */
static int nc_key_count = 0;

static void nc_build_cull_keys(void) {
    nc_key_count = 0;
    if (!nc_smd) return;
    uint8_t *p = (uint8_t *)nc_smd->p_prims;
    int i, n = nc_smd->n_prims;
    if (n > NORTH_CHAMBER_PRIM_COUNT) n = NORTH_CHAMBER_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &nc_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    nc_key_count = n;
}

void north_chamber_load_geometry(void) {
    nc_buff = room_arena_load("\\TEXCTCMB\\NRTHCHMB.SMD;1");
    nc_smd  = nc_buff ? smdInitData(nc_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    nc_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: four compile-time headers and ONE
   deferred registration. The bank mask is derived, not guessed — py
   tools/check_tex_banks.py walks the uploader graph and fails if CATACOMBS is not
   in it. */
void north_chamber_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    ladder_tex = texmgr_register("\\TEXCTCMB\\LADDER.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
    TIM_SLOT(2, BARS);
    TIM_SLOT(3, LADDER);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> THE LADDER LINE IS WHAT OVERWRITES THE INCINERATOR. <<< Nothing else in
   this room draws x704 y256, and the Incinerator Room's own uploader puts the
   machine back on the way in there, so there is no ordering rule between the
   four calls. */
void north_chamber_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    bars_upload_texture();
    north_chamber_upload_ladder();
}

/* The ladder alone, for the Cleaver Corridor at its top (see the header). */
void north_chamber_upload_ladder(void) {
    texmgr_upload(ladder_tex);
}

/* ---- THE SOUTH DOOR --------------------------------------------------------
   z=0, x[1000,1200], y[-400,0] — in the ground floor's south wall, straight
   under the south gallery. Back into THE PIT, through the door in its north
   alcove.

   A door in the XY plane at fixed Z, approached from +Z (wall 0 runs z=0 with
   nz=+4096), so TEXT_PLANE_XY with mirror=1, the sign 11 proud of the wall along
   +Z, and the -200 door_draw_string_3d wants on the X argument (the reading
   axis for an XY sign).

   ITS Y IS THE GROUND FLOOR'S, -186, the chapter's usual eye-level sign. */
#define NC_SOUTH_X            1100     /* the art spans x[1000,1200] */
#define NC_SOUTH_Z               0
#define NC_SOUTH_TEXT_Y       (-186)
#define NC_TEXT_RADIUS        1200
#define NC_FADE_NEAR           800
#define NC_TRIGGER_RADIUS      500

/* ---- THE WEST DOOR ---------------------------------------------------------
   x=-1200, z[1800,2000], y[-1400,-1000] — in the outer wall of the GALLERY's
   west arm. Into THE ROOM OF HEADS, through its one (east) door.

   A door in the YZ plane at fixed X, approached from +X (wall 3 runs x=-1200 at
   gallery height with nx=+4096), so TEXT_PLANE_YZ with mirror=0 and the sign 11
   proud of the wall along +X; the -200 goes on the Z argument, the reading axis
   for a YZ sign. The Room of Heads' east door takes the opposite pair.

   >>> ITS STOREY TEST IS THE MIRROR OF THE SOUTH DOOR'S. <<< The ground floor
   runs straight under the west arm, so a player standing on the ground at
   x~-1000 is inside this door's radius in plan. Gallery only, for the trigger
   and the sign alike.

   ITS Y IS THE GALLERY'S: -1186, the usual eye-level -186 over y=-1000. */
#define NC_WEST_X           (-1200)
#define NC_WEST_Z             1900     /* the art spans z[1800,2000] */
#define NC_WEST_TEXT_Y      (NC_GALLERY_Y - 186)

/* ---- THE LADDER ------------------------------------------------------------
   x=2800, z[1600,1800], y[-1800,-1000] — on the back wall of the ladder alcove
   off the GALLERY's east arm, climbing into the black shaft in the vault. Up to
   THE CLEAVER CORRIDOR, through the top of its shaft.

   The wall is wall 19, x=2800 with the walkable side -X, so the prompt is a
   YZ-plane sign approached from -X: mirror=1, 11 proud of the wall along -X, and
   the -200 on the Z argument — the Room of Heads' east door's pair. The alcove
   is z[1400,2000], so the 195 standoff leaves the player z[1595,1805] and
   x<=2605: always inside the 500 trigger radius once in the alcove.

   Gallery only, the west door's test: see the header for why it is insurance
   here. ITS Y IS THE GALLERY'S, -1186. */
#define NC_LADDER_X           2800
#define NC_LADDER_Z           1700     /* the ladder spans z[1600,1800] */
#define NC_LADDER_TEXT_Y    (NC_GALLERY_Y - 186)

/* Circle edge-detect, one per door. Seeded "held" by the arm below so a press
   carried in through the transition cannot fire on the arrival frame. */
static int south_circle_prev  = 1;
static int west_circle_prev   = 1;
static int ladder_circle_prev = 1;

void north_chamber_arm(void) {
    south_circle_prev  = interact_tapped();
    west_circle_prev   = south_circle_prev;
    ladder_circle_prev = south_circle_prev;
}

/* On the ground floor, as the south door needs it: see the header. The west
   door wants the opposite answer. */
static int nc_on_ground(void) {
    return cam_y > NC_STOREY_SPLIT_Y;
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int north_chamber_south_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !south_circle_prev;
    int32_t dx, dz, xz;
    south_circle_prev = held;
    if (lock || !just) return 0;
    if (!nc_on_ground()) return 0;          /* the gallery is right over it */
    dx = cam_x - NC_SOUTH_X;
    dz = cam_z - NC_SOUTH_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= NC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(NC_SOUTH_X, NC_SOUTH_Z)) return 0;
    return 1;
}

/* The west door's Circle test: the south door's, on the gallery instead of the
   ground. */
int north_chamber_west_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !west_circle_prev;
    int32_t dx, dz, xz;
    west_circle_prev = held;
    if (lock || !just) return 0;
    if (nc_on_ground()) return 0;           /* the ground runs under it */
    dx = cam_x - NC_WEST_X;
    dz = cam_z - NC_WEST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= NC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(NC_WEST_X, NC_WEST_Z)) return 0;
    return 1;
}

/* The ladder's Circle test: the west door's, at the foot of the ladder. */
int north_chamber_ladder_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !ladder_circle_prev;
    int32_t dx, dz, xz;
    ladder_circle_prev = held;
    if (lock || !just) return 0;
    if (nc_on_ground()) return 0;           /* the ladder is on the gallery */
    dx = cam_x - NC_LADDER_X;
    dz = cam_z - NC_LADDER_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= NC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(NC_LADDER_X, NC_LADDER_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within NC_FADE_NEAR, gone by NC_TEXT_RADIUS. Ground floor only, for the
   trigger's reason. */
static void nc_south_sign(RenderContext *ctx) {
    int32_t dx = cam_x - NC_SOUTH_X;
    int32_t dz = cam_z - NC_SOUTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (!nc_on_ground()) return;
    if (xz >= NC_TEXT_RADIUS) return;

    if (xz > NC_FADE_NEAR) {
        int range = NC_TEXT_RADIUS - NC_FADE_NEAR;
        int prog  = xz - NC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        NC_SOUTH_X - 200, NC_SOUTH_TEXT_Y, NC_SOUTH_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

/* The west door's sign: gallery only, mirror=0 (approached from +X). */
static void nc_west_sign(RenderContext *ctx) {
    int32_t dx = cam_x - NC_WEST_X;
    int32_t dz = cam_z - NC_WEST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (nc_on_ground()) return;
    if (xz >= NC_TEXT_RADIUS) return;

    if (xz > NC_FADE_NEAR) {
        int range = NC_TEXT_RADIUS - NC_FADE_NEAR;
        int prog  = xz - NC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        NC_WEST_X + 11, NC_WEST_TEXT_Y, NC_WEST_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

/* The ladder's prompt: gallery only, mirror=1 (approached from -X). */
static void nc_ladder_sign(RenderContext *ctx) {
    int32_t dx = cam_x - NC_LADDER_X;
    int32_t dz = cam_z - NC_LADDER_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (nc_on_ground()) return;
    if (xz >= NC_TEXT_RADIUS) return;

    if (xz > NC_FADE_NEAR) {
        int range = NC_TEXT_RADIUS - NC_FADE_NEAR;
        int prog  = xz - NC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to ascend",
                        NC_LADDER_X - 11, NC_LADDER_TEXT_Y, NC_LADDER_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void north_chamber_spawn_south(void) {
    /* Arriving from The Pit. 220 off wall 0 on its walkable +Z side, facing +Z
       — the direction of travel, and straight at the cage in the middle of the
       room. The ground floor's eye; apply_height settles it next frame. The
       south gallery is overhead here and the zone order is what keeps the
       player on the floor rather than lifting them onto it: the gallery zone is
       skipped because its surface is above the eye. */
    cam_x   = NC_SOUTH_X;
    cam_y   = NC_GROUND_EYE_Y;
    cam_vy  = 0;
    cam_z   = NC_SOUTH_Z + (NC_WALL_RADIUS + 25);
    cam_rot = 0;                       /* facing +Z, north into the room */
    north_chamber_arm();
}

void north_chamber_spawn_west(void) {
    /* Back from the Room of Heads. On the GALLERY's west arm, 220 off wall 3 on
       its walkable +X side, facing +X — the direction of travel, out over the
       drop into the room. The arm is x[-1200,-600], so x=-980 leaves 380 before
       the unrailed edge. The gallery's eye; apply_height finds the gallery zone
       first (it is below this eye) and keeps the player up here. */
    cam_x   = NC_WEST_X + (NC_WALL_RADIUS + 25);
    cam_y   = NC_GALLERY_Y - GROUND_FLOOR_Y - 40;
    cam_vy  = 0;
    cam_z   = NC_WEST_Z;
    cam_rot = 1024;                    /* facing +X, east into the room */
    north_chamber_arm();
}

void north_chamber_spawn_ladder(void) {
    /* Back down the ladder from the Cleaver Corridor. In the ladder alcove, 220
       off wall 19 on its walkable -X side, on the ladder's centre line, facing
       -X — out of the alcove and across the east arm, the way the player walks
       away from the foot of a ladder. x=2580 is inside the alcove's gallery
       zone x[2400,2800]; the gallery's eye, as the west door's spawn. */
    cam_x   = NC_LADDER_X - (NC_WALL_RADIUS + 25);
    cam_y   = NC_GALLERY_Y - GROUND_FLOOR_Y - 40;
    cam_vy  = 0;
    cam_z   = NC_LADDER_Z;
    cam_rot = 3072;                    /* facing -X, west out of the alcove */
    north_chamber_arm();
}

void north_chamber_init(void) {
    north_chamber_collision_init(&current_collision_room);
    /* The vault, read off the VISUAL mesh: y=-1800 over the whole chamber, where
       the proxy's outer walls stop too. 1800 over the ground, 800 over the
       gallery. */
    collision_set_ceiling_y(-1800);
    collision_set_wall_radius(NC_WALL_RADIUS);

    north_chamber_floor_zones_init();
    cam_pitch = 0;

    north_chamber_spawn_south();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside x[-1800,3000] z[0,3600]. The Catacombs Entry's two sconces at
       (+-595,200) land inside it, as they land inside The Pit; sconces gate on
       area, so they do not bite, and this is the cheap guarantee rather than a
       fix. Safe to clear: catacombs_entry_init() re-places all four. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    /* The Pit's bars are NOT cleared: bars_collide() gates on area, and The
       Pit's own init re-places them on every entry there anyway. */

    nc_view_resolve(1);
}

static void draw_north_chamber_smd(RenderContext *ctx) {
    if (!nc_smd) return;

    uint8_t *p = (uint8_t *)nc_smd->p_prims;
    int i, n = nc_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = nc_fog_far;    /* resolved by nc_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* THE REJECT PATH READS cull_keys, NOT THE MESH. XZ only, so the ground
           floor under a player on the gallery is at ring distance ~0 and never
           rejected, which is what makes looking over the edge work. */
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
        SVECTOR *v0 = &nc_smd->p_verts[vi[0]];
        SVECTOR *v1 = &nc_smd->p_verts[vi[1]];
        SVECTOR *v2 = &nc_smd->p_verts[vi[2]];

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

        int nocull = (i < NORTH_CHAMBER_PRIM_COUNT) && north_chamber_nocull[i];
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
            v3 = &nc_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner (render.h). Three
           stacked sets here as in The Pit — ground, gallery, vault — and the
           gallery IS over the ground, so the rule is doing real work. */
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
        int32_t fog = dist < nc_fog_near ? nc_fog_near : (dist > nc_fog_far ? nc_fog_far : dist);
        int32_t fog_factor = ((nc_fog_far - fog) << 8) / (nc_fog_far - nc_fog_near);

        uint8_t tex_idx = (i < NORTH_CHAMBER_PRIM_COUNT) ? north_chamber_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < NORTH_CHAMBER_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + NC_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + NC_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + NC_FOG_B * (256 - fog_factor)) >> 8);

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

void north_chamber_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    nc_view_resolve(0);

    g_fog_near = nc_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : nc_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, NC_FOG_R, NC_FOG_G, NC_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All four textures sit at Voff 0. The cage's and the ladder's UVs both run
       past 127 and rely on it. */
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

    if (exp != DBG_EXP_NO_MESH) draw_north_chamber_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGN AND THE ENEMIES. <<< The room is empty today,
       so what it takes away is the one door sign — STEP 3D's case. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        nc_south_sign(ctx);   /* south: XY plane, approached from +Z */
        nc_west_sign(ctx);    /* west: YZ plane, approached from +X, gallery */
        nc_ladder_sign(ctx);  /* ladder: YZ plane, approached from -X, gallery */
        /* BOTH CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           A placement in this room has to pick a storey (y against 0 for the
           ground, -1000 for the gallery), since world_seed_room() cannot read
           these floor zones. Both sheets sit at Voff 128, so each is handed the
           window to restore after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
    }
}
