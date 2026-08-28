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
#include "tim_slots.h"
#include "camera.h"
#include "maze_two.h"
#include "collision.h"
#include "maze_two_mesh_collision.h"
#include "maze_two_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "garden_courtyard.h"    /* garden_courtyard_upload_textures      */
#include "outside_catacombs.h"   /* outside_catacombs_upload_flowers      */
#include "maze_one.h"            /* maze_one_upload_pipe                  */
#include "valve_puzzle.h"      /* the pipe in this room, and the gate it locks */
#include "valve_handle.h"      /* the wheel on this room's standpipe */
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "living_statue.h"
#include "web.h"
#include "item_pickup.h"
#include "sml_med.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Maze Two: the second hedge maze, north of Maze One. See maze_two.h for the
   layout. */

static SMD  *maze_two_smd  = NULL;
static void *maze_two_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   MAZE ONE'S EXACTLY, which is Fountain Square's exactly: 575/2500, the same
   purple SKY_FOG_* colour, and the cull equal to the fog-far so nothing is
   dropped until the fog has already faded it into the background. The whole
   garden from the courtyard's gate onwards is one continuous outdoors, and the
   step through a gate changes the plan of the place but not the weather.

   It suits this room on its own account for the same reason it suited Maze One.
   The mesh is 1894 prims over 10200 x 4600 units, so at 2500 Manhattan the cull
   is holding roughly three-quarters of it out of the packet buffer at any
   moment — and the hedge runs occlude far more than the fog does anyway. What
   the player sees is a couple of junctions ahead fading into the murk. */
#define MT_CULL_DIST      2500
#define MT_FOG_NEAR        575
#define MT_FOG_FAR        2500

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, exactly as Maze One. The collision generator found seventeen floor
   planes and every one is at y=0 — this room has no step, ramp or storey
   anywhere in it. They tile the maze's corridors rather than the whole bounding
   rectangle, but the gaps between them are the insides of hedge blocks, which
   the player is kept out of by the 500-tall wall runs in the wall list. One rect
   over the collision bounds therefore covers every square unit the player can
   legally stand on. */
static void maze_two_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -4000; floor_zones[0].max_x = 6200;
    floor_zones[0].min_z = 0;     floor_zones[0].max_z = 4600;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Six mesh textures and the room owns exactly ONE of them. The other five are
   already put in VRAM by rooms this one is reached through, so for those it owns
   no texture RAM at all: it takes the tpage/clut headers as compile-time
   constants and delegates the entry-time LoadImage to whichever module holds the
   RAM copy. Registering a second copy of each would have been the obvious thing
   and is the wrong thing — texmgr has a hard TEXMGR_MAX and a registration past
   it fails SILENTLY, breaking that texture everywhere (see
   tools/TEXTURING_NOTES.txt).

     0 hedge              the perimeter and every maze run (clsd_drwr page, x384 y0)
     1 grdn_gte           the two gates                    (kchn_wl page,   x512 y0)
     2 grss_gs            the ground throughout            (stn_stl page,   x320 y0)
     3 poison_flower_base the three flower beds            (trck_clue page, x640 y0)
     4 pipe               the standpipe                    (brick_wall page, x768 y0)
     5 plinth             the block in the north corridor  (opn_drwr page,  x832 y0) OWNED

   Slots 0-2 come from the Garden Courtyard's uploader (which itself runs
   garden_stairs_upload_textures first). As in Maze One, Fountain Square and the
   Outside Catacombs, the mesh's 'grss' material resolves to the grss_gs CLONE
   rather than to grss's own page, so the whole garden chain draws from one set
   of slots — see gen_maze_two_tex_map.py.

   Slots 3 and 4 are taken through NARROW accessors on the modules that own them
   — outside_catacombs_upload_flowers() and maze_one_upload_pipe() — rather than
   through either room's full uploader. That is not tidiness: the Catacombs'
   would also stamp the lamashtu tablet on x768 y0, straight over the pipe, and
   Maze One's would also stamp the drain on x832 y0, straight over the plinth.
   maze_one_upload_pipe() was added for this room, on the
   conservatory_upload_con_tile pattern.

   Slot 5 is the one thing this room owns, and it went to the only full 8bpp page
   this room draws nothing from. Note what that is NOT: Maze One's comment says
   the gravel_gs page at x704 y0 is free to a maze, since neither maze draws any
   gravel — and for a 4bpp texture it is, but only its left half. gravel_gs,
   rusty_fence and upstairs are all 4bpp and take 32 VRAM columns each; anzu3 and
   anzu6 sit in the right half at x736, so a full-page 8bpp texture there lands
   on the Anzu. x832 is the page this room genuinely draws nothing from: it has
   no drain, the one texture Maze One takes from Fountain Square that this room
   does not. It adds no restore obligation — that page is already time-shared
   five ways (con_tile, double_door, drain, opn_drwr, xt_dr_cg) and everyone who
   needs the original re-uploads on their own entry.

   All six sit at Voff 0, so the one 128 texture window set in maze_two_draw
   serves them all. */
#define MAZE_TWO_TEX_COUNT 6

static uint16_t tex_tpage[MAZE_TWO_TEX_COUNT];
static uint16_t tex_clut[MAZE_TWO_TEX_COUNT];

/* The one texture this room OWNS: RAM-resident from startup so the entry-time
   upload is a pure LoadImage with no CD read. Keep this in step with the slot
   numbering above and with NAME_TO_SLOT in gen_maze_two_tex_map.py. */
static int plinth_tex_id;

/* ---- Cull keys -------------------------------------------------------------
   Maze One's scheme verbatim, and for the same reason: in a maze the reject
   path, not the draw path, is what the frame is spent on. Only a small fraction
   of the 1894 primitives survive the distance cull, and each rejected one would
   otherwise cost a read of the primitive header for its stride, a read of its
   first vertex INDEX, and a chase into the 107 KB vertex array — three
   main-memory stalls with no data cache behind them.

   So the cull key is lifted out into its own array, built once at load: the
   first vertex's X and Z, plus the primitive's stride so the walk can advance
   without reading the header at all. A rejected primitive costs ONE sequential
   6-byte read and never touches the mesh. Identical output — this changes what
   the reject path READS, not what it decides.

   6 bytes x 1894 = 11 KB of BSS, alongside the 1.8 KB maze_two_nocull table that
   is already indexed the same way. Indices match the draw loop's `i`. */
typedef struct { int16_t x, z; uint8_t stride, pad; } MtCullKey;
static MtCullKey mt_keys[MAZE_TWO_PRIM_COUNT];
static int       mt_key_count = 0;

static void mt_build_cull_keys(void) {
    mt_key_count = 0;
    if (!maze_two_smd) return;
    uint8_t *p = (uint8_t *)maze_two_smd->p_prims;
    int i, n = maze_two_smd->n_prims;
    if (n > MAZE_TWO_PRIM_COUNT) n = MAZE_TWO_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &maze_two_smd->p_verts[vi[0]];
        mt_keys[i].x      = v0->vx;
        mt_keys[i].z      = v0->vz;
        mt_keys[i].stride = pt->len;
        mt_keys[i].pad    = 0;
        p += pt->len;
    }
    mt_key_count = n;
}

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. At 107 KB this mesh is the
   second largest on the disc and fits the arena Maze One's 117 KB sizes. */
void maze_two_load_geometry(void) {
    maze_two_buff = room_arena_load("\\TEX\\MAZETWO.SMD;1");
    maze_two_smd  = maze_two_buff ? smdInitData(maze_two_buff) : NULL;
    mt_build_cull_keys();
}

/* Read at STARTUP — the only safe time for CD access — the one texture this room
   owns. Geometry moved to maze_two_load_geometry above, and the other five slots
   are compile-time constants that cost nothing here. */
void maze_two_load_assets(void) {
    /* Every one of these is uploaded by another module; header only — no
       LoadImage, no second RAM copy. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRSSGS);
    TIM_SLOT(3, PSNFLWR);
    TIM_SLOT(4, PIPE);

    /* This room's own: a RAM-resident copy for a CD-free re-upload. */
    plinth_tex_id = texmgr_register("\\TEX\\PLINTH.TIM;1");
    TIM_SLOT(5, PLINTH);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).

   ORDER MATTERS on every line here. The courtyard's uploader (via the Garden
   Stairs') puts chnlnk on x640 y0, brick_wall on x768 y0 and xt_dr_cg on
   x832 y0 — which are where the flower beds, the pipe and the plinth live — so
   all three of the following must go up AFTER it, never before. The two
   delegated calls are the NARROW ones by design: outside_catacombs_upload_textures
   would also drop the lamashtu tablet on x768 y0 over the pipe, and
   maze_one_upload_textures would also drop the drain on x832 y0 over the
   plinth. */
void maze_two_upload_textures(void) {
    garden_courtyard_upload_textures();   /* hedge, grdn_gte, grss_gs, gravel_gs */
    outside_catacombs_upload_flowers();   /* flowers -> the chnlnk slot          */
    maze_one_upload_pipe();               /* pipe    -> the brick_wall slot      */
    texmgr_upload(plinth_tex_id);         /* plinth  -> the xt_dr_cg slot        */
}

/* Just the plinth, for the Keystone Maze through the east gate of Maze One —
   which draws the same block but no pipe. The conservatory_upload_con_tile
   pattern: this room owns the only RAM copy, and a second texmgr_register there
   would spend a registration to hold identical bytes (see
   tools/TEXTURING_NOTES.txt). Calling maze_two_upload_textures() wholesale
   instead would ALSO drop the borrowed pipe on x768 y0, which is precisely where
   the Keystone Maze's plinth_diamond goes. The caller must have run the
   courtyard's uploader first — xt_dr_cg lands on this slot. */
void maze_two_upload_plinth(void) {
    texmgr_upload(plinth_tex_id);
}

/* ---- The south-wall gate back to Maze One -----------------------------------
   The grdn_gte polys on this side span x[-1600,-1000] at z=0, y[-600,0]. It is
   the same gate leaf as the one in Maze One's north hedge, which is four units
   narrower there — the two rooms' meshes are independent, and only this pairing
   links them. The gate's own alcove is collision FLOOR 1, x(-1599,-1000)
   z(0,202), which is what fixes the centre at x=-1300.

   Collision wall 30 runs across the opening at z=0 with nz = +4096, so the
   walkable side is +Z and the player approaches from inside the maze. For a sign
   in the XY plane that is mirror=1, and it stands 11 units NORTH of the wall
   (z + 11). Both are the opposite hand from Maze One's side of the same gate,
   whose wall faces -Z — getting either backwards comes out as mirrored text or a
   sign buried in the hedge. */
#define MT_GATE_X          (-1300)   /* (-1600 + -1000) / 2, and FLOOR 1's centre */
#define MT_GATE_Z               0
#define MT_TEXT_Y           (-186)   /* eye level on the y=0 ground               */
#define MT_TEXT_RADIUS       1500
#define MT_FADE_NEAR         1000
#define MT_TRIGGER_RADIUS     500

/* Standing eye on the ground: floor y=0, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. The same expression Maze One, Fountain
   Square and the Outside Catacombs use, and for the same reason — this whole run
   of garden rooms has its floor at y=0 while the courtyard below is at positive
   y. */
#define MT_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by maze_two_gate_arm(). Starts "held" so a press
   carried in through the transition doesn't bounce the player straight back. */
static int gate_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void maze_two_gate_arm(void) {
    gate_circle_prev = circle_held();
}

int maze_two_gate_triggered(void) {
    int held = circle_held();
    int just = held && !gate_circle_prev;
    gate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - MT_GATE_X;
    int32_t dz = cam_z - MT_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < MT_TRIGGER_RADIUS && interact_facing(MT_GATE_X, MT_GATE_Z);
}

/* Floating "Press O to enter" sign on the south gate. XY plane:
   door_draw_string_3d centres the reading axis (X) on world_x after adding 200,
   so pass door_x - 200. Sits just north (z+11) of the wall so it floats in front
   of the gate on the side the player is actually on. */
static void gate_text(RenderContext *ctx) {
    int32_t dx = cam_x - MT_GATE_X;
    int32_t dz = cam_z - MT_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= MT_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > MT_FADE_NEAR) {
        int range = MT_TEXT_RADIUS - MT_FADE_NEAR;
        int prog  = xz - MT_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        MT_GATE_X - 200, MT_TEXT_Y, MT_GATE_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Arriving from Maze One: stand in the gate alcove north of the z=0 hedge, clear
   of the wall push radius (so the player isn't shoved on their first frame),
   facing +Z — the direction of travel through the gate, looking down the maze's
   southern lane. z lands at 220, just past the alcove's own FLOOR 1 (which stops
   at 202) and inside collision FLOOR 11's z(202,802) run, so the floor is under
   them immediately. x=-1300 is 300 clear of the alcove's side walls at x=-1599
   and x=-1000, comfortably outside the 195 push radius. */
void maze_two_spawn_south(void) {
    cam_x   = MT_GATE_X;
    cam_y   = MT_EYE_Y;
    cam_vy  = 0;
    cam_z   = MT_GATE_Z + COLLISION_WALL_RADIUS + 25;
    cam_rot = 0;      /* facing +Z, into the room */
    maze_two_gate_arm();
    maze_two_egate_arm();
}

/* ---- The east-wall gate, into the Chain Room --------------------------------
   The grdn_gte polys on this side span z[3400,4000] at x=6200, y[-600,0], in the
   YZ plane. It was drawn shut and backed onto solid collision until the Chain
   Room was built; it now opens on that room's west gate (src/chain_room.h). Its
   own corridor is collision FLOOR 9, x(5400,6200) z(3400,4000), which is what
   fixes the centre at z=3700.

   >>> IT IS THE OPPOSITE HAND FROM THE SOUTH GATE, AND ONLY THE NORMAL SAYS SO.
   <<< Collision wall 28 runs across this opening with nx = -4096, so the
   walkable side is -X and the player approaches from inside this maze heading
   EAST. For a sign in the YZ plane that is mirror=1 and the sign stands 11 units
   WEST of the wall (x - 11) - the Rear Gate's EAST gate exactly, and the
   opposite hand from the Chain Room's side of this same gate, whose wall 12
   faces +X. Getting either backwards comes out as mirrored text or a sign buried
   in the hedge.

   The wall STAYS in the collision list, as wall 30 does on the south side: the
   leaf is shut as far as collision is concerned and it is the trigger, not a
   hole, that lets the player through. */
#define MT_EGATE_X          6200
#define MT_EGATE_Z          3700   /* (3400 + 4000) / 2, and FLOOR 9's centre */

static int egate_circle_prev = 1;

void maze_two_egate_arm(void) {
    egate_circle_prev = circle_held();
}

int maze_two_egate_triggered(void) {
    int held = circle_held();
    int just = held && !egate_circle_prev;
    egate_circle_prev = held;
    if (!just) return 0;
    /* THE VALVE LOCK. This gate is the outside approach to the Chain Room and it
       does not open until THIS ROOM'S OWN standpipe has been worked -- see
       src/valve_puzzle.c. After the edge state, not before, so the held press is
       still consumed while it is refused. */
    if (!valve_puzzle_gates_unlocked()) return 0;

    int32_t dx = cam_x - MT_EGATE_X;
    int32_t dz = cam_z - MT_EGATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < MT_TRIGGER_RADIUS && interact_facing(MT_EGATE_X, MT_EGATE_Z);
}

/* YZ plane, mirror=1, and 11 units WEST of the wall - see the note above for why
   both differ from the south gate's. Everything else (the radii, the fade ramp,
   the eye-level Y) is that gate's verbatim: it is the same leaf in the same
   perimeter at the same height. */
static void egate_text(RenderContext *ctx) {
    int32_t dx = cam_x - MT_EGATE_X;
    int32_t dz = cam_z - MT_EGATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= MT_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > MT_FADE_NEAR) {
        int range = MT_TEXT_RADIUS - MT_FADE_NEAR;
        int prog  = xz - MT_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* Red and glyphless until the valve in this room is turned, as Reception's
       sealed doors and the Garden Stairs' read. */
    if (!valve_puzzle_gates_unlocked()) {
        door_draw_string_3d(ctx, "Locked by some mechanism",
                            MT_EGATE_X - 11, MT_TEXT_Y, MT_EGATE_Z - 200,
                            255, 50, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
        return;
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        MT_EGATE_X - 11, MT_TEXT_Y, MT_EGATE_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving back from the Chain Room: stand in the east corridor west of the
   x=6200 wall, clear of the 195 push radius, facing -X - the direction of travel
   through the gate, looking back down the corridor into the maze. x lands at
   5980, and at z=3700 the nearest ends of the corridor's own walls (27 and 29 at
   x=5400 and z=3400, 21 and 24 at z=4000 and x=6000) are all 300 away. */
void maze_two_spawn_east(void) {
    cam_x   = MT_EGATE_X - COLLISION_WALL_RADIUS - 25;
    cam_y   = MT_EYE_Y;
    cam_vy  = 0;
    cam_z   = MT_EGATE_Z;
    cam_rot = 3072;   /* facing -X, into the room */
    maze_two_gate_arm();
    maze_two_egate_arm();
}

void maze_two_init(void) {
    maze_two_collision_init(&current_collision_room);
    /* The perimeter hedge is DRAWN to y=-500 (the gates reach -600, but they are
       the openings, not the roofline); the collision runs are 500 tall here,
       which happens to agree, but state the DRAWN value regardless so anything
       ceiling-mounted hangs at the height the room actually has — see
       tools/ADDING_A_ROOM.txt on visual-vs-collision heights. */
    collision_set_ceiling_y(-500);
    /* No collision_set_wall_radius: the default 195 is right here, as in Maze
       One. The tightest lane in this maze is the 600-wide run along the south
       wall, which at 195 leaves 210 units of walkable width — the same margin
       Maze One's corridors give. The courtyard's 260 would cut it to 80 and make
       the maze impassable. main.c resets to the default before every room init,
       so saying nothing is enough. */

    maze_two_floor_zones_init();

    /* Two gates now, so main.c overrides this default with
       maze_two_spawn_east() when the player arrives from the Chain Room. Both
       spawns arm both gates. */
    maze_two_spawn_south();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room's bounds contain the origin, so they certainly do. Clearing is safe:
       reception_init() re-places both on every reception entry. No save point of
       this room's own; the nearest is on the Garden Stairs' top landing. */
    save_points_clear();
    dressers_clear();

    /* The valve pipe's prompt keeps its own Circle edge state, so it is armed
       here for the reason both gates are: a press held through the transition
       must not open the board on the arrival frame. */
    valve_puzzle_arm();
}

static void draw_maze_two_smd(RenderContext *ctx) {
    if (!maze_two_smd) return;


    uint8_t *p = (uint8_t *)maze_two_smd->p_prims;
    int i, n = maze_two_smd->n_prims;

    /* Hoisted: all constant for the whole frame, and every one of them would
       otherwise be recomputed per primitive inside the hottest loop in the room
       — the two trig lookups once for each poly that passed the distance cull,
       the cull distance and the debug test all 1894 times. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = MT_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    if (mt_key_count < n) n = mt_key_count;   /* keys not built: draw nothing */

    for (i = 0; i < n; i++) {
        /* ---- The reject path. ONE sequential read, no mesh access ----------
           mt_keys carries this primitive's first vertex and its stride, so a
           primitive outside the view distance is skipped without touching the
           header or the vertex array at all. See mt_build_cull_keys. */
        uint8_t stride = mt_keys[i].stride;
        int32_t kdx = (int32_t)mt_keys[i].x - cam_x;
        int32_t kdz = (int32_t)mt_keys[i].z - cam_z;
        if ((kdx < 0 ? -kdx : kdx) + (kdz < 0 ? -kdz : kdz) > cull)
            { p += stride; continue; }

        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &maze_two_smd->p_verts[vi[0]];
        SVECTOR *v1 = &maze_two_smd->p_verts[vi[1]];
        SVECTOR *v2 = &maze_two_smd->p_verts[vi[2]];

        {

            /* ---- SIDE-PLANE FRUSTUM CULL -------------------------------------
               Maze One's test, unchanged, and load-bearing for the same reason:
               the behind-the-camera test below rejects almost nothing on its own,
               because in a maze the geometry within the cull radius is all around
               you rather than behind you. Everything else would be handed to the
               GTE, transformed, and then — if it projected inside the GPU's
               +/-1023 coordinate limit, which is over three screens wide — queued
               as a primitive the GPU had to take in and throw away.

               The test is the two side planes of the frustum. With
               gte_SetGeomScreen(256) on a 320-wide screen the half-field is
               160/256, so a point is outside the right plane when
                   side > (5/8) * fwd,  i.e.  8*side > 5*fwd
               and outside the left when the same holds for -side. Both sides of
               that comparison are world units (the >>12 undoes isin/icos), and
               at this room's scale the products stay far inside an int32.

               >>> A POLY IS ONLY CULLED WHEN EVERY VERTEX IS OUTSIDE THE SAME
               PLANE. <<< Testing v0 alone would slice through polys that
               straddle the screen edge. The exact test costs up to three more
               vertex evaluations, but ONLY for polys whose v0 is already outside
               a plane — a poly in view exits on the first test — and each one it
               rejects saves a full GTE transform and a queued primitive.

               Verified against THIS mesh offline before it was written, the same
               way Maze One's was: over 16 headings at each of three poses (the
               gate arrival, mid-maze, and the east side) it removes 71%, 72% and
               69% of the primitives that would have been submitted, and
               introduces ZERO holes — where a hole means culling a poly with any
               vertex inside the true frustum. Re-run that check if the mesh is
               re-exported.

               Y is not tested. The camera is at standing height in a room whose
               hedges are drawn to y=-500 and whose floor is flat, so nothing is
               ever outside the top or bottom planes while inside the side ones;
               adding them would cost multiplies to reject nothing.
               ------------------------------------------------------------- */
            int32_t fwd = kdx * sn + kdz * cs;
            if (fwd < -(700 << 12))
                { p += stride; continue; }
            if (!no_frustum) {
                int32_t f0 = fwd >> 12;                      /* world units */
                int32_t s0 = (kdx * cs - kdz * sn) >> 12;
                int     sign = 0;
                if      ( s0 * 8 > f0 * 5) sign =  1;        /* v0 off right */
                else if (-s0 * 8 > f0 * 5) sign = -1;        /* v0 off left  */
                if (sign) {
                    int n = is_quad ? 4 : 3, k, all_out = 1;
                    for (k = 1; k < n; k++) {
                        SVECTOR *vk = &maze_two_smd->p_verts[vi[k]];
                        int32_t ex = (int32_t)vk->vx - cam_x;
                        int32_t ez = (int32_t)vk->vz - cam_z;
                        int32_t f  = (ex * sn + ez * cs) >> 12;
                        int32_t sd = (ex * cs - ez * sn) >> 12;
                        if (!(sign * sd * 8 > f * 5)) { all_out = 0; break; }
                    }
                    if (all_out) { p += stride; continue; }
                }
            }

        }

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

        /* Backface cull, except degenerate (triangle-shaped) quads flagged at
           build time in maze_two_nocull — same scheme as the other rooms. This
           mesh happens to contain none, but the table is generated and read the
           same way regardless. */
        int nocull = (i < MAZE_TWO_PRIM_COUNT) && maze_two_nocull[i];
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
            v3 = &maze_two_smd->p_verts[vi[3]];
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
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog = dist < MT_FOG_NEAR ? MT_FOG_NEAR : (dist > MT_FOG_FAR ? MT_FOG_FAR : dist);
        int32_t fog_factor = ((MT_FOG_FAR - fog) << 8) / (MT_FOG_FAR - MT_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in maze_two_draw. */
        uint8_t tex_idx = (i < MAZE_TWO_PRIM_COUNT) ? maze_two_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < MAZE_TWO_TEX_COUNT);
        /* Purple fog, the same night sky the rest of the garden looks out on,
           and at Maze One's exact near/far — this is one continuous outdoors and
           the gate between them is not a change in the weather. */
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

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

void maze_two_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = MT_FOG_NEAR; g_fog_far = MT_FOG_FAR;

    /* Background in the SAME colour the fog saturates to, so a poly that has
       faded out is indistinguishable from the void behind it and the cull never
       shows a seam. Purple here, matching the rest of the garden.

       Painted by the HARDWARE CLEAR, not by a full-screen TILE — the draw
       environments already clear the framebuffer (isbg=1) before anything is
       drawn, so a tile on top of that is a second full-screen fill of the same
       77k pixels every frame. See the note on render_set_clear_colour, and Maze
       One, which is the room that trialled it. */
    render_set_clear_colour(ctx, SKY_FOG_R, SKY_FOG_G, SKY_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. All six of this room's textures sit at page-top (Voff 0), so one
       window serves them (see tools/VRAM_MAP.txt). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix via camera_build_view rather than the hand-rolled yaw-only
       block the older rooms use — same matrix at pitch 0, and it does not drop
       cam_pitch on the floor should anything here ever tilt the camera. */
    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    /* --- Isolation switches (debug levels 4-7; see camera.c). Normal play and
       every other debug level take exp == 0 and none of this applies. --- */
    int exp = DEBUG_EXPERIMENT();

    /* The ladder moves the FOG with the cull, so each rung shows what the room
       would actually look like at that view distance — geometry fading out into
       the background colour rather than popping off at the cull line. */
    if (DEBUG_CULL_DIST()) g_fog_far = DEBUG_CULL_DIST();

    if (exp != DBG_EXP_NO_MESH) draw_maze_two_smd(ctx);
    /* THE VALVE WHEEL, if one is fitted to this room's standpipe. After the mesh
       so it sorts against it and inside the 128 texture window set above, which
       is what its pipe texture wants; it restores the plain view matrix on the
       way out, so the sprite draws below still project correctly. The Greenhouse
       draws its own the same way and in the same place. Nothing is drawn at all
       until the puzzle fits one -- valve_handles_draw skips a mount whose
       `present` is clear. */
    if (exp != DBG_EXP_NO_ENTITIES) valve_handles_draw(ctx);


    /* Every sprite enemy renderer is handed this room's texture window, because
       all of their sprites live at Voff >= 128 and must bracket it rather than
       sample the hedge (see tools/TEXTURING_NOTES.txt PART 5). Three of these
       arrays are live here now — a Rafflesia on each of the three flower beds
       (rafflesias_init), one Mushroom Head pacing the southern lane and the
       plinth's Living Statue (both world_seed_room) — and the zombie and spider
       calls cost nothing on their empty arrays.

       This room is on SND_BANK_GARDEN (main.c's STATE_LOADING), so all three
       reach SFX_HISS and the flowers' own loops; check any further placement
       against src/sound.h the way Maze One's was. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
        rafflesias_set_texwindow(&tw);
        mushrooms_set_texwindow(&tw);
        living_statues_set_texwindow(&tw);
    }
    {
        draw_zombies(ctx);
        draw_spiders(ctx);
        draw_rafflesias(ctx);
        draw_mushrooms(ctx);
        draw_living_statues(ctx);
        webs_draw(ctx);
        item_pickups_draw(ctx);
        sml_meds_draw(ctx);
    }

    /* Last: the two gate signs, and the standpipe's. */
    gate_text(ctx);
    egate_text(ctx);
    valve_puzzle_text(ctx);

    /* Dead last, and NOT world-space: the valve board is a 2D overlay on the
       menu's OT range, so it goes on top of everything the room has drawn. */
    valve_puzzle_draw(ctx);
}
