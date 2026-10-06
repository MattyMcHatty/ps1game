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
#include "gaol_cells.h"
#include "gaol_entry.h"         /* the gaol door's art, and its LOCK          */
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "gaol_cells_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "bars.h"               /* ...and the bars'                           */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons, the drop's flag */
#include "item_pickup.h"        /* the Blood Pearl on the sconce */
#include "title.h"              /* STATE_GAOL_CELLS, the area tags */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Gaol Cells — see gaol_cells.h for the layout and the doors. */

static SMD  *gac_smd  = NULL;
static void *gac_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   366/1300 with ~+46%/~+46% for the lantern — THE GAOL ENTRY'S NUMBERS, which
   are the Up Down Maze's, so the fog does not change across the gaol door.
   Keep them in step with src/gaol_entry.c. The room is 5000 x 3600, so 1300
   shows the corridor a cell or two at a time: the lantern is what shows how
   far it goes. 1576 primitives, 419 of them bars. */
#define GAC_BASE_FOG_NEAR   366
#define GAC_BASE_FOG_FAR   1300

#define GAC_VIEW_UNIT        256
#define GAC_VIEW_HELL_BONUS  118   /* ~+46% while the lantern is in hand   */
#define GAC_VIEW_BURN_BONUS  118   /* ~+46% more while it is actually lit  */
#define GAC_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t gac_view     = GAC_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t gac_fog_near = GAC_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t gac_fog_far  = GAC_BASE_FOG_FAR;

static int32_t gac_view_target(void) {
    int32_t s = GAC_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += GAC_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += GAC_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void gac_view_resolve(int snap) {
    int32_t target = gac_view_target();
    if (snap) {
        gac_view = target;
    } else if (gac_view < target) {
        gac_view += GAC_VIEW_RATE;
        if (gac_view > target) gac_view = target;
    } else if (gac_view > target) {
        gac_view -= GAC_VIEW_RATE;
        if (gac_view < target) gac_view = target;
    }
    gac_fog_near = (GAC_BASE_FOG_NEAR * gac_view) >> 8;
    gac_fog_far  = (GAC_BASE_FOG_FAR  * gac_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define GAC_FOG_R             7
#define GAC_FOG_G             6
#define GAC_FOG_B             9

/* Wall standoff. The chapter's 195. The corridors are 733 wide (z[233,966])
   and wider, so nothing here argues for anything else. */
#define GAC_WALL_RADIUS     195

/* Standing eye on this room's one floor level (y=0): less GROUND_FLOOR_Y and
   the 40-unit standoff apply_height applies. */
#define GAC_FLOOR_Y            0
#define GAC_EYE_Y           (GAC_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   TWO, BOTH FLAT AT y=0 — the proxy's two floor faces (the FLOOR list at the
   foot of src/gaol_cells_mesh_collision.c):

     x[0,4999]  z[-500,2399]    the cells and the corridors between them
     x[0,3525]  z[-1200,-500]   the southern strip below them

   One plane, so their order does not matter; they are two only because the
   proxy's floor is, and together they cover every cell, walkable or not. */
static void gac_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = 0;      floor_zones[i].max_x = 4999;
    floor_zones[i].min_z = -500;   floor_zones[i].max_z = 2399;
    floor_zones[i].y     = GAC_FLOOR_Y;
    i++;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = 0;      floor_zones[i].max_x = 3525;
    floor_zones[i].min_z = -1200;  floor_zones[i].max_z = -500;
    floor_zones[i].y     = GAC_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   FIVE, AND THE ROOM OWNS ONE.

     0 cobblestones        the walls, the vaulting, the corridor
                           floors — 836 of the 1576 polys  (x384 y0)
     1 catacomb inner door the south and east doorways
                           (sealed), 5 polys               (x832 y0)
     2 bars                the cell fronts, 419 polys,
                           4bpp                            (x512 y256)
     3 gaol door           the western gaol door and the
                           cell doors, 28 polys, 4bpp      (x640 y0)
     4 mud                 the cells' floors and walls,
                           288 polys                       (x576 y0)  OWNED HERE

   Slots 0 and 1 come through src/catacombs_entry.c's narrow uploaders, slot 2
   through src/bars.c's and slot 3 through src/gaol_entry.c's — the Gaol
   Entry's set, plus one.

   >>> SLOT 4 IS ASAG'S MUD, BUT NOT ASAG'S TIM. <<< The art is the same
   128x128 resample (textures/mud.png, from textures/asag/mud.png) that
   \TEXASAG\ASGMUD.TIM was built from, but a Catacombs room borrows nothing
   from another area's bank, and that TIM sits on x384 y0 — cobblestone's
   page, drawn in this very room. So it is converted again, as
   textures/catacombs/gaol_mud.tim (\TEXCTCMB\GAOLMUD.TIM), onto the CRIB'S
   PAGE AND PALETTE: x576 y0, 8bpp, CLUT line (256,484). No crib is drawn
   here, and every room that draws one (Arms, Heads, Legs, Bones, Torsos) puts
   the crib's pixels and palette back through crib_upload_texture() on entry,
   so nobody owes anybody a restore. smxlink still resolves the material name
   `mud` to textures/mud.tim; only the tpage/clut below come from this copy.

   All five sit at Voff 0, so the one 128 texture window in the draw serves
   them. */
#define GAOL_CELLS_TEX_COUNT 5

static uint16_t tex_tpage[GAOL_CELLS_TEX_COUNT];
static uint16_t tex_clut[GAOL_CELLS_TEX_COUNT];

/* The one texmgr entry this room owns (slot 4). -1 until registration — a
   refused registration stops the boot on texmgr_refused()'s red screen, so a
   -1 here past startup would be a bug. texmgr_upload on -1 is a no-op. */
static int mud_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/gaol_entry.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int gac_key_count = 0;

static void gac_build_cull_keys(void) {
    gac_key_count = 0;
    if (!gac_smd) return;
    uint8_t *p = (uint8_t *)gac_smd->p_prims;
    int i, n = gac_smd->n_prims;
    if (n > GAOL_CELLS_PRIM_COUNT) n = GAOL_CELLS_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &gac_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    gac_key_count = n;
}

void gaol_cells_load_geometry(void) {
    gac_buff = room_arena_load("\\TEXCTCMB\\GAOLCELL.SMD;1");
    gac_smd  = gac_buff ? smdInitData(gac_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (gac_smd && !room_data_bind(gac_smd->n_prims)) gac_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    gac_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: five compile-time headers and ONE
   deferred registration. The bank mask is derived, not guessed — py
   tools/check_tex_banks.py walks the uploader graph and fails if CATACOMBS is not
   in it. */
void gaol_cells_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    mud_tex = texmgr_register("\\TEXCTCMB\\GAOLMUD.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
    TIM_SLOT(2, BARS);
    TIM_SLOT(3, GAOLDOOR);
    TIM_SLOT(4, GAOLMUD);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> THE MUD LINE IS WHAT OVERWRITES THE CRIB, PIXELS AND PALETTE. <<<
   Nothing else in this room draws x576 y0 or reads CLUT (256,484), and every
   crib room's uploader puts both back on the way in there, so there is no
   ordering rule between the calls. */
void gaol_cells_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    bars_upload_texture();
    gaol_entry_upload_gaol_door();
    texmgr_upload(mud_tex);
    /* ...and the SCONCE's own page (x448 y0), for the cold one in the
       north-east cell. Nothing else here draws that page either. */
    sconce_upload_texture();
}

/* ---- THE SCONCE ------------------------------------------------------------
   ONE, COLD, in the NORTH-EAST CELL x[3637,4999] z[1033,1700], the one
   reached from the north corridor through its gap at x[4599,4999]: 362 off
   the cell's west wall, 314 off its south one and 353 off its north one.

   UNLIT (sconce_place's `lit` 0), as in the Cleaver Corridor and the Up Down
   Maze: no flame, no point light, until the Blood Pearl on it is taken, which
   lights it (src/sconce.c, THE PEARL).

   The BLOOD PEARL is a pickup, placed with the room's other residents in
   src/world.c from these same two numbers — keep them in step. */
#define GAC_SCONCE_X          3999
#define GAC_SCONCE_Z          1347

/* ---- THE MAGGOT DROP ------------------------------------------------------
   FIVE MAGGOTS OUT OF THE DARK OVER THE EAST DOOR (x=5000, z[500,700], its
   top at y=-400), sprung the first time the player crosses the TRIPWIRE: a
   band right across the centre corridor at x=2637, the full corridor width
   z[233,966] between walls 29 and 27, and 200 deep so a sprinting player (20
   a frame) cannot step over it. Either direction trips it.

   Two spots 150 in from the east wall, either side of the door's centre line,
   taken in turn. SPAWNED HIGH, the H Corridor's construction (h_corridor.c):
   maggot_spawn() takes a floor ANCHOR and an anchor above the floor falls
   under gravity to it, so an anchor of -599 puts the body about y=-620 —
   above the door's head and under the y=-800 ceiling — and it drops into the
   corridor already hunting.

   THE TIMING: the first appears on the frame the wire trips, the rest every
   GAC_MAGGOT_GAP. The door is ~2200 from the wire, ~200 frames at MGT_SPEED,
   so all five are out before the first arrives — a string of them coming down
   the corridor.

   ONCE PER PLAYTHROUGH: FLAG_GAOL_CELLS_MAGGOTS is set on the frame it trips.
   The maggots are transient (src/maggot.h), so leaving mid-drop loses whatever
   has not appeared yet, and the flag means it never restarts. */
#define GAC_WIRE_MIN_X        2537
#define GAC_WIRE_MAX_X        2737
#define GAC_WIRE_MIN_Z         233
#define GAC_WIRE_MAX_Z         966

#define GAC_MAGGOT_COUNT         5
#define GAC_MAGGOT_GAP          45    /* 3/4 s between them                   */
#define GAC_MAGGOT_X          4850    /* 150 in from the east wall            */
#define GAC_MAGGOT_DROP_Y    (-450)   /* above the standing anchor            */

static const int16_t gac_maggot_spot_z[2] = { 550, 650 };

static int gac_maggots_left = 0;   /* still to appear on this visit             */
static int gac_maggot_timer = 0;   /* frames to the next one                    */
static int gac_maggot_next  = 0;   /* which spot the next one comes from        */

void gaol_cells_update(void) {
    if (game_over) return;

    /* The wire. Checked every frame until the flag is set, then never again. */
    if (!game_flag(FLAG_GAOL_CELLS_MAGGOTS) &&
        cam_x >= GAC_WIRE_MIN_X && cam_x <= GAC_WIRE_MAX_X &&
        cam_z >= GAC_WIRE_MIN_Z && cam_z <= GAC_WIRE_MAX_Z) {
        game_flag_set(FLAG_GAOL_CELLS_MAGGOTS);
        gac_maggots_left = GAC_MAGGOT_COUNT;
        gac_maggot_timer = 1;              /* the first, this frame */
        gac_maggot_next  = 0;
    }

    if (gac_maggots_left <= 0) return;
    if (--gac_maggot_timer > 0) return;
    /* A full pool places nothing; that one is lost rather than retried —
       maggots_reset() empties the pool on every room change, so nothing else
       can be holding it here. */
    maggot_spawn(GAC_MAGGOT_X, gac_maggot_spot_z[gac_maggot_next],
                 GAC_FLOOR_Y - GROUND_FLOOR_Y + GAC_MAGGOT_DROP_Y,
                 STATE_GAOL_CELLS);
    gac_maggot_next ^= 1;
    gac_maggots_left--;
    gac_maggot_timer = GAC_MAGGOT_GAP;
}

/* ---- THE WEST GAOL DOOR ----------------------------------------------------
   x=0, z[300,700], y[-400,0] — the barred door at the west end of the entry
   corridor, the far face of the Gaol Entry's east door. LOCKED until the Gaol
   Key is used on it from either side: the press goes to gaol_door_press() in
   src/gaol_entry.c, which is the whole lock for both faces, and the sign to
   gaol_door_sign().

   A door in the YZ plane at fixed X, approached from +X (wall 43 runs x=0
   with nx=+4095), so TEXT_PLANE_YZ with mirror=0 and the sign 11 proud of the
   wall along +X — the Gaol Entry's west door's terms.

   THE SOUTH DOOR is wired too, below: the catacomb inner door in the south-east
   corner of the south wall, into the Nursery. And so is THE EAST DOOR, the
   catacomb inner door in the east wall, into the Room of Baby Names.

   THE OTHER DOORS ARE DRAWN AND NOTHING ELSE: the three barred cell doors (x[600,1000] and x[3075,3525] at z~200,
   x[4600,5000] at z~1000), which stand in solid proxy walls. No sign, no
   trigger. The mesh's own copy of the Gaol Entry, west of x=0, is what is
   seen back through the bars. */
#define GAC_WEST_X              0
#define GAC_WEST_Z            500     /* the art spans z[300,700] */
#define GAC_WEST_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define GAC_TEXT_RADIUS      1200
#define GAC_FADE_NEAR         800
#define GAC_TRIGGER_RADIUS    500

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int west_circle_prev  = 1;
static int south_circle_prev = 1;
static int east_circle_prev  = 1;

void gaol_cells_arm(void) {
    west_circle_prev  = interact_tapped();
    south_circle_prev = interact_tapped();
    east_circle_prev  = interact_tapped();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int gaol_cells_west_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !west_circle_prev;
    int32_t dx, dz, xz;
    west_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - GAC_WEST_X;
    dz = cam_z - GAC_WEST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GAC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(GAC_WEST_X, GAC_WEST_Z)) return 0;
    return gaol_door_press();
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within GAC_FADE_NEAR, gone by GAC_TEXT_RADIUS. */
static void gac_west_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - GAC_WEST_X;
    int32_t dz = cam_z - GAC_WEST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= GAC_TEXT_RADIUS) return;

    if (xz > GAC_FADE_NEAR) {
        int range = GAC_TEXT_RADIUS - GAC_FADE_NEAR;
        int prog  = xz - GAC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    gaol_door_sign(ctx, GAC_WEST_X + 11, GAC_WEST_TEXT_Y, GAC_WEST_Z - 200,
                   fade, 0);   /* mirror=0: YZ door approached from +X */
}

void gaol_cells_spawn_west(void) {
    /* 220 off wall 43 on its walkable +X side, on the door's centre line,
       facing +X — the direction of travel, east down the entry corridor.
       (220,500) is 267 off the corridor's south wall (z=233) and 466 off its
       north one (z=966), both clear of the 195 push. */
    cam_x   = GAC_WEST_X + (GAC_WALL_RADIUS + 25);
    cam_y   = GAC_EYE_Y;
    cam_vy  = 0;
    cam_z   = GAC_WEST_Z;
    cam_rot = 1024;                    /* facing +X, east down the corridor */
    gaol_cells_arm();
}

/* ---- THE SOUTH DOOR --------------------------------------------------------
   x[4600,4800], z=-500, y[-400,0] — the catacomb inner door in the SOUTH-EAST
   corner, in wall 25 (x[3525,4999] at z=-500, nz=+4096), into the Nursery's
   east door. Unlocked.

   A door in the XY plane at fixed Z, approached from +Z, so TEXT_PLANE_XY with
   mirror=1, the -200 on the X argument, and the sign 11 proud of the wall along
   +Z. The approach is the south-east cell block's strip, x[3525,4999]
   z[-500,166], so nothing else is within the trigger radius. */
#define GAC_SOUTH_X          4700     /* the art spans x[4600,4800] */
#define GAC_SOUTH_Z          (-500)
#define GAC_SOUTH_TEXT_Y     (-186)   /* eye level on the y=0 floor */

int gaol_cells_south_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !south_circle_prev;
    int32_t dx, dz, xz;
    south_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - GAC_SOUTH_X;
    dz = cam_z - GAC_SOUTH_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GAC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(GAC_SOUTH_X, GAC_SOUTH_Z)) return 0;
    return 1;
}

static void gac_south_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - GAC_SOUTH_X;
    int32_t dz = cam_z - GAC_SOUTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= GAC_TEXT_RADIUS) return;

    if (xz > GAC_FADE_NEAR) {
        int range = GAC_TEXT_RADIUS - GAC_FADE_NEAR;
        int prog  = xz - GAC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        GAC_SOUTH_X - 200, GAC_SOUTH_TEXT_Y, GAC_SOUTH_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

void gaol_cells_spawn_south(void) {
    /* 220 off wall 25 on its walkable +Z side, on the door's centre line,
       facing +Z — the direction of travel, back up into the cells. (4700,-280)
       is 299 off the east wall (x=4999) and 446 off the cell block's south
       face (z=166), both clear of the 195 push. */
    cam_x   = GAC_SOUTH_X;
    cam_y   = GAC_EYE_Y;
    cam_vy  = 0;
    cam_z   = GAC_SOUTH_Z + (GAC_WALL_RADIUS + 25);
    cam_rot = 0;                       /* facing +Z, north */
    gaol_cells_arm();
}

/* ---- THE EAST DOOR ---------------------------------------------------------
   x=5000, z[500,700], y[-400,0] — the catacomb inner door at the east end of
   the centre corridor, in wall 44 (x=4999, z[-500,2399], nx=-4096), into the
   Room of Baby Names' west door, the one with JOHN over it. Unlocked.

   A door in the YZ plane at fixed X, approached from -X, so TEXT_PLANE_YZ with
   mirror=1, the -200 on the Z argument, and the sign 11 proud of the wall
   along -X — the Nursery's east door's terms. The approach is the centre
   corridor, z[233,966], so nothing else is within the trigger radius.

   >>> THE MAGGOT DROP COMES OUT OF THE DARK OVER THIS DOOR (above). <<< Its
   two spots, (4850, 550/650), are 70 from this arrival's (4779,600), so a
   player who comes back through the door before the wire is ever tripped
   meets them on the doorstep when it is. */
#define GAC_EAST_X           4999     /* wall 44; the art is at x=5000 */
#define GAC_EAST_Z            600     /* the art spans z[500,700] */
#define GAC_EAST_TEXT_Y      (-186)   /* eye level on the y=0 floor */

int gaol_cells_east_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !east_circle_prev;
    int32_t dx, dz, xz;
    east_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - GAC_EAST_X;
    dz = cam_z - GAC_EAST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GAC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(GAC_EAST_X, GAC_EAST_Z)) return 0;
    return 1;
}

static void gac_east_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - GAC_EAST_X;
    int32_t dz = cam_z - GAC_EAST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= GAC_TEXT_RADIUS) return;

    if (xz > GAC_FADE_NEAR) {
        int range = GAC_TEXT_RADIUS - GAC_FADE_NEAR;
        int prog  = xz - GAC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        GAC_EAST_X - 11, GAC_EAST_TEXT_Y, GAC_EAST_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void gaol_cells_spawn_east(void) {
    /* 220 off wall 44 on its walkable -X side, on the door's centre line,
       facing -X — the direction of travel, back west down the centre
       corridor. (4779,600) is 366 off the corridor's north wall (z=966) and
       409 off its south one (z=233), both clear of the 195 push. */
    cam_x   = GAC_EAST_X - (GAC_WALL_RADIUS + 25);
    cam_y   = GAC_EYE_Y;
    cam_vy  = 0;
    cam_z   = GAC_EAST_Z;
    cam_rot = 3072;                    /* facing -X, west */
    gaol_cells_arm();
}

void gaol_cells_init(void) {
    room_data_collision(&current_collision_room);
    /* The ceiling, read off the VISUAL mesh: y=-800 over the whole room, where
       the proxy's walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(GAC_WALL_RADIUS);

    gac_floor_zones_init();
    cam_pitch = 0;

    gaol_cells_spawn_west();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       not all of them are area-gated in every collide routine, so an instance
       left from another room would block invisibly anywhere it falls inside
       x[0,4999] z[-1200,2399]. The Catacombs Entry's sconce at (595,200) lands
       inside it. Safe to clear: catacombs_entry_init() re-places all four,
       and the Up Down Maze's init re-places its own sconce. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* ...then this room's own sconce, cold, in the north-east cell. */
    sconce_place(STATE_GAOL_CELLS, GAC_SCONCE_X, -GROUND_FLOOR_Y,
                 GAC_SCONCE_Z, 0, 0);

    /* Nothing of a drop carries across a visit: the flag says whether the
       wire can still trip, and a drop cut short by leaving stays cut short. */
    gac_maggots_left = 0;

    gac_view_resolve(1);
}

static void draw_gaol_cells_smd(RenderContext *ctx) {
    if (!gac_smd) return;

    uint8_t *p = (uint8_t *)gac_smd->p_prims;
    int i, n = gac_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = gac_fog_far;   /* resolved by gac_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* THE REJECT PATH READS cull_keys, NOT THE MESH. */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t cd = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            /* SHORT-CIRCUITED ON PURPOSE, the Cleaver Corridor's arrangement:
               only a primitive the camera would drop asks the sconce's light,
               which is empty until the Blood Pearl is taken (src/sconce.c). */
            if (cd > cull &&
                render_light_dist((int32_t)cull_keys[i].x,
                                  (int32_t)cull_keys[i].z, cd) > cull)
                { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12)) { p += stride; continue; }
        }

        /* SURVIVED BOTH CULLS: only now is the header read and the vertex array
           addressed. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &gac_smd->p_verts[vi[0]];
        SVECTOR *v1 = &gac_smd->p_verts[vi[1]];
        SVECTOR *v2 = &gac_smd->p_verts[vi[2]];

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

        int nocull = (i < GAOL_CELLS_PRIM_COUNT) && room_nocull(i);
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
            v3 = &gac_smd->p_verts[vi[3]];
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
           must not be sorted over the bars standing on it. */
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
        /* The sconce's discount, and it MUST match the one the cull above
           applied or a lit poly survives the cull and is then shaded as though
           it had not been — drawn in the clear colour, a hole. */
        dist = render_light_dist(face_cx, face_cz, dist);
        int32_t fog = dist < gac_fog_near ? gac_fog_near : (dist > gac_fog_far ? gac_fog_far : dist);
        int32_t fog_factor = ((gac_fog_far - fog) << 8) / (gac_fog_far - gac_fog_near);

        uint8_t tex_idx = (i < GAOL_CELLS_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < GAOL_CELLS_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + GAC_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + GAC_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + GAC_FOG_B * (256 - fog_factor)) >> 8);

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

void gaol_cells_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    gac_view_resolve(0);

    g_fog_near = gac_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : gac_fog_far;

    /* THE SCONCE'S LIGHT, after the two numbers it is a discount on and before
       the mesh that reads it. Nothing is published while the sconce is cold;
       taking the Blood Pearl lights it (src/sconce.c, THE PEARL). */
    sconces_publish_lights();

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, GAC_FOG_R, GAC_FOG_G, GAC_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All five textures sit at Voff 0. */
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

    if (exp != DBG_EXP_NO_MESH) draw_gaol_cells_smd(ctx);

    /* The sconce, cold until its Blood Pearl is taken. Its texture sits at
       Voff 0, so the window above serves it. */
    sconces_draw(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGN, THE ENEMIES AND THE PEARL. <<< */
    if (exp != DBG_EXP_NO_ENTITIES) {
        gac_west_door_text(ctx);   /* west: YZ plane, approached from +X */
        gac_south_door_text(ctx);  /* south: XY plane, approached from +Z */
        gac_east_door_text(ctx);   /* east: YZ plane, approached from -X  */
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
        /* THE BLOOD PEARL, on the cold sconce. Pickups are not drawn globally.
           After the enemy calls: its art sits at Voff 64 and needs the 128
           window they restore. */
        item_pickups_draw(ctx);
    }
}
