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
#include "sliding_bars_room.h"
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "sliding_bars_room_mesh_collision.h"
#include "sliding_bars_room_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* three of the narrow uploaders this room borrows */
#include "incinerator.h"        /* ...the incinerator panel's page               */
#include "bars.h"               /* ...and the bars'                              */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Sliding Bars Room — see sliding_bars_room.h for the layout and the
   doors. */

static SMD  *sb_smd  = NULL;
static void *sb_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 with +50%/+50% for the lantern — the chapter's usual numbers, the
   Crucifix Corridor's and the Cleaver Corridor's. In a grid of 600 blocks the
   player never sees more than a few cells down any corridor, so 1600 is three
   cells of dark and then the fog. */
#define SB_BASE_FOG_NEAR   450
#define SB_BASE_FOG_FAR   1600

#define SB_VIEW_UNIT        256
#define SB_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define SB_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define SB_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t sb_view     = SB_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t sb_fog_near = SB_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t sb_fog_far  = SB_BASE_FOG_FAR;

static int32_t sb_view_target(void) {
    int32_t s = SB_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += SB_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += SB_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void sb_view_resolve(int snap) {
    int32_t target = sb_view_target();
    if (snap) {
        sb_view = target;
    } else if (sb_view < target) {
        sb_view += SB_VIEW_RATE;
        if (sb_view > target) sb_view = target;
    } else if (sb_view > target) {
        sb_view -= SB_VIEW_RATE;
        if (sb_view < target) sb_view = target;
    }
    sb_fog_near = (SB_BASE_FOG_NEAR * sb_view) >> 8;
    sb_fog_far  = (SB_BASE_FOG_FAR  * sb_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define SB_FOG_R             7
#define SB_FOG_G             6
#define SB_FOG_B             9

/* Wall standoff. The chapter's 195. The corridors between the blocks are 600
   wide, which leaves a 210 band down the middle of each. */
#define SB_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define SB_FLOOR_Y            0
#define SB_EYE_Y           (SB_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's one floor face: the whole square. The
   blocks stand on it and their walls keep the player out of them. */
static void sb_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = 0;  floor_zones[0].max_x = 4200;
    floor_zones[0].min_z = 0;  floor_zones[0].max_z = 4200;
    floor_zones[0].y     = SB_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   FIVE, AND THE ROOM OWNS NONE OF THEM.

     0 cobblestones        the floor, the vault and the blocks — 857 of the
                           1179 polys                     (x384 y0)
     1 catacomb inner door the three doorways             (x832 y0)
     2 loculus             the niches in the blocks       (x512 y0)
     3 incinerator         the four wall panels           (x704 y256)
     4 bars                the barred partitions, 4bpp    (x512 y256)

   Cobble, the inner door and the loculus come through src/catacombs_entry.c's
   narrow uploaders, as the Tomb takes them; the panel through
   incinerator_upload_texture() and the bars through bars_upload_texture(), as
   the Incinerator Room and the North Chamber take theirs. All five sit at
   Voff 0, so the one 128 texture window in the draw serves them. */
#define SLIDING_BARS_ROOM_TEX_COUNT 5

static uint16_t tex_tpage[SLIDING_BARS_ROOM_TEX_COUNT];
static uint16_t tex_clut[SLIDING_BARS_ROOM_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/crucifix_corridor.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int sb_key_count = 0;

static void sb_build_cull_keys(void) {
    sb_key_count = 0;
    if (!sb_smd) return;
    uint8_t *p = (uint8_t *)sb_smd->p_prims;
    int i, n = sb_smd->n_prims;
    if (n > SLIDING_BARS_ROOM_PRIM_COUNT) n = SLIDING_BARS_ROOM_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &sb_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    sb_key_count = n;
}

void sliding_bars_room_load_geometry(void) {
    sb_buff = room_arena_load("\\TEXCTCMB\\SLDBARS.SMD;1");
    sb_smd  = sb_buff ? smdInitData(sb_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    sb_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: five compile-time headers and no
   registration at all, so no texmgr_set_bank() either. The bank this room's art
   is in is decided by its owners'. */
void sliding_bars_room_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
    TIM_SLOT(2, LOCULUS);
    TIM_SLOT(3, INCINPRP);
    TIM_SLOT(4, BARS);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first). */
void sliding_bars_room_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    catacombs_entry_upload_loculus();
    incinerator_upload_texture();
    bars_upload_texture();
}

/* ---- THE SOUTH-WEST DOOR ---------------------------------------------------
   z=0, x[200,400], in the south wall at its west end. Out to THE CRUCIFIX
   CORRIDOR, at the north door of its cross arm.

   In the XY plane at fixed Z, approached from +Z (wall 21 runs z=0 with
   nz = +4095, so the walkable side is +Z): TEXT_PLANE_XY with mirror=1, the
   sign 11 proud of the wall along +Z, and the -200 door_draw_string_3d wants on
   the X argument. The strip in front of it, x[0,600] z[0,1100], is open floor:
   the bars at z[1100,1200] close it to the north, and it opens east along the
   south wall at x=600 z[0,600]. */
#define SB_SOUTH_X            300     /* the art spans x[200,400] */
#define SB_SOUTH_Z              0
#define SB_SOUTH_TEXT_Y       (-186)   /* eye level on the y=0 floor */
#define SB_TEXT_RADIUS       1200
#define SB_FADE_NEAR          800
#define SB_TRIGGER_RADIUS     500

/* ---- THE NORTH-EAST DOOR --------------------------------------------------
   x=4200, z[3800,4000], in the east wall at its north end (square 7 of the
   grid below). Out to THE ROOM OF LEGS, at its one door.

   In the YZ plane at fixed X, approached from -X (the east wall is the room's
   edge at x=4200, so the walkable side is -X): TEXT_PLANE_YZ with mirror=1, the
   sign 11 proud of the wall along -X, and the -200 door_draw_string_3d wants on
   the Z argument. */
#define SB_NE_X              4200
#define SB_NE_Z              3900     /* the art spans z[3800,4000] */

/* ---- THE SOUTH-EAST DOOR --------------------------------------------------
   x=4200, z[200,400], in the east wall at its south end (square 49 of the grid
   below). Out to THE MEAT PLANT, at its west door. The north-east door's twin:
   YZ plane, approached from -X, mirror=1. */
#define SB_SE_X              4200
#define SB_SE_Z               300     /* the art spans z[200,400] */

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int south_circle_prev = 1;
static int ne_circle_prev    = 1;
static int se_circle_prev    = 1;
static int gate_circle_prev  = 1;   /* the four panels share one: one Circle */

void sliding_bars_room_arm(void) {
    south_circle_prev = interact_tapped();
    ne_circle_prev    = south_circle_prev;
    se_circle_prev    = south_circle_prev;
    gate_circle_prev  = south_circle_prev;
}

/* ---- THE GATES -------------------------------------------------------------
   THE PUZZLE. Read the room as a 7x7 grid of 600 cells, numbered 1..49 from
   the NORTH-WEST corner, west to east then north to south: square n is column
   c = (n-1)%7 (x[600c, 600c+600]) and row r = (n-1)/7 (z[3600-600r, 4200-600r]).
   The nine blocks are 9, 11, 13, 23, 25, 27, 37, 39 and 41.

   Four Bars instances are gates, each closing the gap between two cells, and
   each has one incinerator panel (drawn by the mesh; the four in it are these
   four) that slides it two cells along its line to its other spot, ONCE:

     gate  panel                          start         moved
     1     E wall of 36 (x=600  z=900)    38|45         40|47
     2     S wall of 2  (x=900  z=3600)   15|16         29|30
     3     S wall of 4  (x=2100 z=3600)   33|34         19|20
     4     W wall of 28 (x=3600 z=2100)   12|19         14|21

   Every gate's travel runs past the face of the block between its two spots,
   so it rides on the CORRIDOR side of its line, 30 off it (its 50 depth leaves
   5 clear of the stone), and never passes through a block — a gate half inside
   the stone would paint over the face in front of it, since props sort at true
   depth and the mesh 40 deeper. That is why it is also resized to the gap,
   600 x 800 (bars_set_size): at 860 it would overhang into the stone at rest.
   Gates 3 and 4 meet at 19's north-east corner when 3 is moved and 4 is at its
   start; the two grilles cross there like fence panels at a post.

   >>> THE INTENDED SOLUTION (the designer's, 2026-09-30). <<< From the
   south-west door: press 1, then 2. Then, for the NORTH-EAST door, press 4
   BEFORE 3. Pressing 3 first is a deliberate trap: it locks button 4 out (the
   player can no longer reach square 28) and with it the north-east door, but
   the SOUTH-EAST door is still reachable, so the player can go on. The way
   back is the RESET BUTTON in another room (not built yet), which calls
   sliding_bars_room_reset_gates() to put all four gates back to their start
   spots so the puzzle can be solved again.

     1 -> 2 -> 4 -> 3   both east doors reachable (button 4 then out of reach)
     1 -> 2 -> 3        south-east door only; button 4 locked out

   Checked by simulation (tracking where the player stands, since pressing 2
   cuts them off from the south-west door) against the mesh's nine static
   bars: 2|3, 4|5, 7|14, 17|24, 28|35, 29|36, 31|32, 33|40, 48|49.

   >>> EVERY GATE JAMS AFTER ONE PRESS (the designer's call). <<< A panel
   works only while its gate is at its START spot; once moved it stays moved
   until sliding_bars_room_reset_gates(). If the panels toggled, the trap would
   not hold: after 1 -> 2 -> 3 the player is still standing at button 3, and
   pressing it again would send gate 3 home, so 4 -> 3 would open the
   north-east door with no reset. Jamming also means a player past button 2
   cannot retreat to the south-west door. A jammed panel's sign is down and a
   press on it only says so ("It's jammed."). No extra state: jammed IS "its
   bit is set", because every gate starts at 0.

   STATE: sb_gates, bit i = gate i+1 is at its MOVED spot. Flipped on the frame
   of the press, so leaving mid-slide (or saving) finds it at its destination.
   Saved as SaveData.sb_gates; reset by a new game and by
   sliding_bars_room_reset_gates(), for the room that will reset them. */
#define SB_GATE_COUNT       4
#define SB_GATE_WIDTH     600    /* the gap between two blocks               */
#define SB_GATE_HEIGHT    800    /* floor to vault                           */
#define SB_GATE_TEXT_Y  (-343)   /* glyph TOP, ABOVE the panel: the panel is
                                    y[-300,-100] (read off the visual mesh),
                                    so the 28-tall line sits 15 above it,
                                    y[-343,-315], well under the -800 vault */
#define SB_GATE_TEXT_OUT   11    /* proud of the block face, as a door sign's */

typedef struct {
    int32_t ax, az;              /* START plan centre                        */
    int32_t bx, bz;              /* MOVED plan centre                        */
    int32_t rot;                 /* 0: runs along x; 1024: runs along z      */
    int32_t px, pz;              /* the panel's centre, on the block face    */
    int32_t tx, tz;              /* its sign's anchor (door_draw_string_3d)  */
    int     plane, mirror;
    int32_t nx, nz;              /* the face's outward normal, unit          */
} SbGate;

static const SbGate sb_gate[SB_GATE_COUNT] = {
    /* 1: line z=600, blocks to the north; rides at z=570. Panel on block 37's
          west face, approached from -X: YZ, mirror=1. */
    { 1500,  570, 2700,  570,    0,
       600,  900,  600 - SB_GATE_TEXT_OUT,  900 - 200, TEXT_PLANE_YZ, 1, -1, 0 },
    /* 2: line x=600, blocks to the east; rides at x=570. Panel on block 9's
          north face, approached from +Z: XY, mirror=1. */
    {  570, 2700,  570, 1500, 1024,
       900, 3600,  900 - 200, 3600 + SB_GATE_TEXT_OUT, TEXT_PLANE_XY, 1, 0, 1 },
    /* 3: line x=3000, blocks to the east; rides at x=2970. Panel on block
          11's north face, approached from +Z: XY, mirror=1. */
    { 2970, 1500, 2970, 2700, 1024,
      2100, 3600, 2100 - 200, 3600 + SB_GATE_TEXT_OUT, TEXT_PLANE_XY, 1, 0, 1 },
    /* 4: line z=3000, blocks to the north; rides at z=2970. Panel on block
          27's east face, approached from +X: YZ, mirror=0. */
    { 2700, 2970, 3900, 2970,    0,
      3600, 2100, 3600 + SB_GATE_TEXT_OUT, 2100 - 200, TEXT_PLANE_YZ, 0, 1, 0 },
};

static uint8_t sb_gates = 0;                  /* bit i: gate i+1 is MOVED */
static int     sb_gate_idx[SB_GATE_COUNT] = { -1, -1, -1, -1 };

int  sliding_bars_room_gates(void)          { return sb_gates; }
void sliding_bars_room_set_gates(int bits)  { sb_gates = (uint8_t)(bits & 0x0F); }
void sliding_bars_room_reset_gates(void)    { sb_gates = 0; }

/* Clear the Bars array and stand all four gates where sb_gates says, at rest.
   The Pit's instance goes with the clear; its own init re-places it. */
static void sb_place_gates(void) {
    int i;
    bars_clear();
    for (i = 0; i < SB_GATE_COUNT; i++) {
        const SbGate *g = &sb_gate[i];
        int moved = (sb_gates >> i) & 1;
        sb_gate_idx[i] = bars_place(STATE_SLIDING_BARS_ROOM,
                                    moved ? g->bx : g->ax,
                                    SB_FLOOR_Y - GROUND_FLOOR_Y,
                                    moved ? g->bz : g->az, g->rot, 0);
        bars_set_size(sb_gate_idx[i], SB_GATE_WIDTH, SB_GATE_HEIGHT);
    }
}

void sliding_bars_room_apply_flags(void) {
    sb_place_gates();
}

/* One frame of the gates: the slides, then the four panels. Same edge rule
   as the door: the edge state is kept current even while locked. A press on a
   moving or JAMMED panel moves nothing, and its sign is down in both cases,
   so the offer and the ability go together. */
void sliding_bars_room_update(int lock) {
    int held = interact_tapped();
    int just = held && !gate_circle_prev;
    int i;
    gate_circle_prev = held;

    bars_update();

    if (lock || !just) return;
    for (i = 0; i < SB_GATE_COUNT; i++) {
        const SbGate *g = &sb_gate[i];
        int32_t dx = cam_x - g->px, dz = cam_z - g->pz;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz >= SB_TRIGGER_RADIUS) continue;
        if (!interact_facing(g->px, g->pz)) continue;
        if (bars_sliding(sb_gate_idx[i])) return;
        if ((sb_gates >> i) & 1) {          /* JAMMED: used once already */
            show_pickup_msg_raw("It's jammed.");
            return;
        }
        sb_gates |= (uint8_t)(1 << i);
        bars_slide(sb_gate_idx[i], g->bx, g->bz);
        return;
    }
}

/* The panels' signs, on the door sign's fade. */
static void sb_gate_text(RenderContext *ctx) {
    int i;
    for (i = 0; i < SB_GATE_COUNT; i++) {
        const SbGate *g = &sb_gate[i];
        int32_t dx = cam_x - g->px, dz = cam_z - g->pz;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int fade = 256;
        if (xz >= SB_TEXT_RADIUS) continue;
        /* ONLY FROM IN FRONT OF THE FACE: door_draw_string_3d has no facing
           test, and the corridor behind the block is inside the radius. */
        if (dx * g->nx + dz * g->nz <= 0) continue;
        if (bars_sliding(sb_gate_idx[i])) continue;
        if ((sb_gates >> i) & 1) continue;  /* jammed */
        if (xz > SB_FADE_NEAR) {
            int range = SB_TEXT_RADIUS - SB_FADE_NEAR;
            int prog  = xz - SB_FADE_NEAR;
            if (prog > range) prog = range;
            fade = 256 - ((prog * 256) / range);
        }
        door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to operate",
                            g->tx, SB_GATE_TEXT_Y, g->tz,
                            50, 255, 50, fade, g->mirror, g->plane,
                            DOOR_PIXEL_SIZE);
    }
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int sliding_bars_room_south_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !south_circle_prev;
    int32_t dx, dz, xz;
    south_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - SB_SOUTH_X;
    dz = cam_z - SB_SOUTH_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= SB_TRIGGER_RADIUS) return 0;
    if (!interact_facing(SB_SOUTH_X, SB_SOUTH_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every door sign in the game: opaque
   within SB_FADE_NEAR, gone by SB_TEXT_RADIUS. */
static void sb_south_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - SB_SOUTH_X;
    int32_t dz = cam_z - SB_SOUTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= SB_TEXT_RADIUS) return;

    if (xz > SB_FADE_NEAR) {
        int range = SB_TEXT_RADIUS - SB_FADE_NEAR;
        int prog  = xz - SB_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        SB_SOUTH_X - 200, SB_SOUTH_TEXT_Y, SB_SOUTH_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

/* The north-east door: the south-west door's test and sign, on the YZ plane. */
int sliding_bars_room_ne_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !ne_circle_prev;
    int32_t dx, dz, xz;
    ne_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - SB_NE_X;
    dz = cam_z - SB_NE_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= SB_TRIGGER_RADIUS) return 0;
    if (!interact_facing(SB_NE_X, SB_NE_Z)) return 0;
    return 1;
}

static void sb_ne_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - SB_NE_X;
    int32_t dz = cam_z - SB_NE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= SB_TEXT_RADIUS) return;

    if (xz > SB_FADE_NEAR) {
        int range = SB_TEXT_RADIUS - SB_FADE_NEAR;
        int prog  = xz - SB_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        SB_NE_X - 11, SB_SOUTH_TEXT_Y, SB_NE_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void sliding_bars_room_spawn_ne(void) {
    /* Back from the Room of Legs. 220 off the east wall on its walkable -X
       side, on the door's centre line (300 clear of the north wall), facing
       -X — the direction of travel, west into square 7. */
    cam_x   = SB_NE_X - (SB_WALL_RADIUS + 25);
    cam_y   = SB_EYE_Y;
    cam_vy  = 0;
    cam_z   = SB_NE_Z;
    cam_rot = 3072;                    /* facing -X, west into the room */
    sliding_bars_room_arm();
}

/* The south-east door: the north-east door's test and sign, at the other end
   of the east wall. */
int sliding_bars_room_se_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !se_circle_prev;
    int32_t dx, dz, xz;
    se_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - SB_SE_X;
    dz = cam_z - SB_SE_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= SB_TRIGGER_RADIUS) return 0;
    if (!interact_facing(SB_SE_X, SB_SE_Z)) return 0;
    return 1;
}

static void sb_se_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - SB_SE_X;
    int32_t dz = cam_z - SB_SE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= SB_TEXT_RADIUS) return;

    if (xz > SB_FADE_NEAR) {
        int range = SB_TEXT_RADIUS - SB_FADE_NEAR;
        int prog  = xz - SB_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        SB_SE_X - 11, SB_SOUTH_TEXT_Y, SB_SE_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void sliding_bars_room_spawn_se(void) {
    /* Back from the Meat Plant. 220 off the east wall on its walkable -X side,
       on the door's centre line (300 clear of the south wall), facing -X — the
       direction of travel, west into square 49. */
    cam_x   = SB_SE_X - (SB_WALL_RADIUS + 25);
    cam_y   = SB_EYE_Y;
    cam_vy  = 0;
    cam_z   = SB_SE_Z;
    cam_rot = 3072;                    /* facing -X, west into the room */
    sliding_bars_room_arm();
}

void sliding_bars_room_spawn_south(void) {
    /* In from the Crucifix Corridor. 220 off wall 21 on its walkable +Z side,
       on the door's centre line (300 clear of the west wall 30), facing +Z —
       the direction of travel, north into the room. */
    cam_x   = SB_SOUTH_X;
    cam_y   = SB_EYE_Y;
    cam_vy  = 0;
    cam_z   = SB_SOUTH_Z + (SB_WALL_RADIUS + 25);
    cam_rot = 0;                       /* facing +Z, north into the room */
    sliding_bars_room_arm();
}

void sliding_bars_room_init(void) {
    sliding_bars_room_collision_init(&current_collision_room);
    /* The vault, read off the VISUAL mesh: y=-800 over the whole square, where
       the proxy's walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(SB_WALL_RADIUS);

    sb_floor_zones_init();
    cam_pitch = 0;

    /* The four gates, where sb_gates left them. Placed again after a load by
       sliding_bars_room_apply_flags(), which runs once the save's byte is in. */
    sb_place_gates();

    sliding_bars_room_spawn_south();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside the square. Safe to clear: every room that has one re-places its
       own on entry. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    sb_view_resolve(1);
}

static void draw_sliding_bars_room_smd(RenderContext *ctx) {
    if (!sb_smd) return;

    uint8_t *p = (uint8_t *)sb_smd->p_prims;
    int i, n = sb_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = sb_fog_far;   /* resolved by sb_view_resolve() this frame */
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
        SVECTOR *v0 = &sb_smd->p_verts[vi[0]];
        SVECTOR *v1 = &sb_smd->p_verts[vi[1]];
        SVECTOR *v2 = &sb_smd->p_verts[vi[2]];

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

        int nocull = (i < SLIDING_BARS_ROOM_PRIM_COUNT) && sliding_bars_room_nocull[i];
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
            v3 = &sb_smd->p_verts[vi[3]];
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
        int32_t fog = dist < sb_fog_near ? sb_fog_near : (dist > sb_fog_far ? sb_fog_far : dist);
        int32_t fog_factor = ((sb_fog_far - fog) << 8) / (sb_fog_far - sb_fog_near);

        uint8_t tex_idx = (i < SLIDING_BARS_ROOM_PRIM_COUNT) ? sliding_bars_room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < SLIDING_BARS_ROOM_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + SB_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + SB_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + SB_FOG_B * (256 - fog_factor)) >> 8);

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

void sliding_bars_room_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    sb_view_resolve(0);

    g_fog_near = sb_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : sb_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, SB_FOG_R, SB_FOG_G, SB_FOG_B);

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

    if (exp != DBG_EXP_NO_MESH) draw_sliding_bars_room_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGNS, THE ENEMIES AND THE GATES. <<< */
    if (exp != DBG_EXP_NO_ENTITIES) {
        sb_south_door_text(ctx);  /* the SW door: XY plane, approached from +Z */
        sb_ne_door_text(ctx);     /* the NE door: YZ plane, approached from -X */
        sb_se_door_text(ctx);     /* the SE door: YZ plane, approached from -X */
        sb_gate_text(ctx);        /* the four panels                          */
        /* BOTH CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Both sheets sit at Voff 128, so each is handed the window to restore
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
        /* The four gates. AFTER the two enemy calls: the prop's UVs run past
           127 and tile only under the 128 window the enemies put back
           (src/bars.h). */
        bars_draw(ctx);
    }
}
