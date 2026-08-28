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
#include "maze_one.h"
#include "collision.h"
#include "maze_one_mesh_collision.h"
#include "maze_one_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "garden_courtyard.h"    /* garden_courtyard_upload_textures      */
#include "fountain_square.h"     /* fountain_square_upload_drain          */
#include "outside_catacombs.h"   /* outside_catacombs_upload_flowers      */
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "living_statue.h"
#include "web.h"
#include "item_pickup.h"
#include "sml_med.h"
#include "birdcage.h"
#include "valve_handle.h"      /* the wheel on this room's standpipe */
#include "valve_puzzle.h"      /* ...and the board that fits it       */           /* the caged Hatch Key's examine prompt */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Maze One: the hedge maze east of Fountain Square. See maze_one.h for the
   layout. */

static SMD  *maze_one_smd  = NULL;
static void *maze_one_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   2200, AND THE CULL AND THE FOG-FAR MOVE TOGETHER. That pairing is the rule
   here, not a coincidence: the cull is set equal to the fog-far so nothing is
   ever dropped until the fog has already faded it into the background colour,
   and the background is painted in that same colour (render_set_clear_colour in
   maze_one_draw). Change one of these two and the other has to follow, or the
   room grows a visible cull line where geometry pops out of a sky it had not
   finished fading into.

   >>> IT USED TO BE 2500, WHICH WAS FOUNTAIN SQUARE'S EXACTLY. <<< The two
   rooms are one continuous walled garden either side of a gate, and matching
   the square's 575/2500 and its purple SKY_FOG_* meant the step through that
   gate changed the plan of the place but not the weather. THAT MATCH IS NOW
   BROKEN ON PURPOSE and it is the one thing to look at first if the gate ever
   reads wrong: the fog NEAR is still the square's 575, so the near half of the
   picture is unchanged and only the far edge closes in by 300 units.

   The reason is frame cost, and this is the lever the room had left after the
   cull-key and bounding-box work (see mo_box below, and STEP 6 of
   tools/DIAGNOSING_FRAME_RATE.txt). Counted offline over 16 headings, at 2500
   against 2200:

       camera at            pass the distance cull    submitted to the GTE
       BIRD CAGE POCKET          588 -> 480                123 -> 102
       maze centre               562 -> 412                121 ->  92
       west gate spawn           263 -> 227                 60 ->  53

   Roughly a fifth off both, and most of it in the middle and the south-east of
   the maze — which is exactly where the room was heavy.

   It suits this room on its own account too. The mesh is 2056 prims, two and a
   half times the square's 802, and it covers 7600 x 6600 units, so even at 2500
   the cull was holding three-quarters of it out of the packet buffer at any
   moment. The maze corridors are only ~600 wide, so the hedge runs occlude far
   more than the fog does anyway; what the player actually sees is a couple of
   junctions ahead fading into the murk, which is the point of putting them in a
   maze at night. 2200 still clears the room's longest straight sightline.

   >>> IT ALSO TIGHTENS THE SPRITE ENEMIES, AND THAT IS BY DESIGN. <<<
   maze_one_draw publishes MO_FOG_FAR as g_fog_far, and both sprite families
   read it: the Rafflesias WAKE at it (rafflesia.c wakes at "the room's own draw
   distance", so that a flower can never be awake and gassing while culled), and
   both the flowers and the Mushroom Heads distance-cull their billboards
   against it. So the five flowers here now wake at 2200 rather than 2500. That
   coupling is deliberate and must stay — the wake radius has to track whatever
   the room can show, not a constant of its own. */
#define MO_CULL_DIST      2200
#define MO_FOG_NEAR        575
#define MO_FOG_FAR        2200

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, exactly as Fountain Square. The collision generator found twenty-
   seven floor planes and every one is at y=0 — this room has no step, ramp or
   storey anywhere in it. They tile the maze's corridors rather than the whole
   bounding rectangle, but the gaps between them are the insides of hedge
   blocks, which the player is kept out of by the 333-tall wall runs in the wall
   list. One rect over the collision bounds therefore covers every square unit
   the player can legally stand on, and giving the hedge interiors zones of their
   own would only help someone who clipped a corner. */
static void maze_one_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -100;  floor_zones[0].max_x = 7500;
    floor_zones[0].min_z = -2095; floor_zones[0].max_z = 4496;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Seven mesh textures and the room owns exactly TWO of them. The other five are
   already put in VRAM by rooms this one is reached through, so for those it owns
   no texture RAM at all: it takes the tpage/clut headers as compile-time
   constants and delegates the entry-time LoadImage to whichever module holds the
   RAM copy. Registering a second copy of each would have been the obvious thing
   and is the wrong thing — texmgr has a hard TEXMGR_MAX and a registration past
   it fails SILENTLY, breaking that texture everywhere (see
   tools/TEXTURING_NOTES.txt).

     0 hedge              the perimeter and every maze run (clsd_drwr page, x384 y0)
     1 grdn_gte           the three gates                  (kchn_wl page,   x512 y0)
     2 grss_gs            the ground throughout            (stn_stl page,   x320 y0)
     3 drain              the channel across the paths     (opn_drwr page,  x832 y0)
     4 poison_flower_base the five flower beds             (trck_clue page, x640 y0)
     5 pipe               the standpipe                    (brick_wall page, x768 y0) OWNED
     6 chain              the hanging chain                (gravel_gs page, x704 y0) OWNED, 4bpp

   Slots 0-2 come from the Garden Courtyard's uploader (which itself runs
   garden_stairs_upload_textures first). As in Fountain Square and the Outside
   Catacombs, the mesh's 'grss' material resolves to the grss_gs CLONE rather
   than to grss's own page, so the whole garden chain draws from one set of
   slots — see gen_maze_one_tex_map.py.

   Slots 3 and 4 are taken through NARROW accessors on the modules that own them
   — fountain_square_upload_drain() and outside_catacombs_upload_flowers() —
   rather than through either room's full uploader. That is not tidiness: both
   full uploaders would stamp their own second texture (fountain, and the
   lamashtu tablet) onto the brick_wall page at x768 y0, which is precisely where
   this room's pipe lives. Calling either one wholesale would put the fountain
   basin on the standpipe.

   Slot 5 is the first of the two things this room owns, and it went to the only
   full 8bpp page this room draws nothing from. It adds no restore obligation:
   that page is already time-shared six ways (grss, bed, fountain, the tablet,
   xt_dr_lckd, xt_dr_cmplt) and everyone who needs the original re-uploads on
   their own entry.

   Slot 6 is the second, and it is what finally spends the gravel_gs page at
   x704 y0 — free for this room because, unlike every other garden room, Maze One
   draws NO gravel at all. >>> IT IS 4bpp AND IT HAS TO BE. <<< Every occupant of
   that page (gravel_gs, rusty_fence, upstairs, stable glyphs and the Greenhouse's
   flower-bed clone) is 4bpp, so they cover only x[704,736); anzu3 and anzu6 sit
   in the RIGHT half at x736. A full-page 8bpp texture here lands on the Anzu —
   the mistake Maze Two came within one slot of shipping, which is why its plinth
   went to x832 instead (see tools/vram_map.py). The chain art is pure greyscale
   — 25 distinct 5-bit levels — so 16 colours costs it nothing visible. It adds
   no restore obligation: gravel_gs comes back through the courtyard's uploader
   and the mansion's three through their own rooms'.

   >>> ITS TIM IS THE 32x32 ART TILED 4x4, NOT STRETCHED 4x. DO NOT "FIX" IT. <<<
   chain.png is 32x32 and the exporter maps 1 UV tile to 128 texels, so the
   obvious conversion — stretch to 128 and convert, the way hedge/pipe/drain go
   64 -> 128 — gives one copy of the art per 128 texels. That is four times too
   coarse for how this mesh is UV'd: it put the nine long strand quads at 300-580
   world units per copy, against 90 for the chain prop beside them and 95-100 for
   the pipe, i.e. individual links taller than the player. Tiling the 32x32 art
   4x4 into the same 128x128 footprint gives the art a period of 32 texels
   instead of 128, which lands the strands at ~100 units per copy — on top of the
   pipe — and keeps every texel a source pixel at 1:1 with no interpolation.
   Seamless because the exporter shifts each poly's UV minimum by whole multiples
   of 128 and the 128 window wraps mod 128, and 32 divides 128 both times.
   See tools/TEXTURING_NOTES.txt PART 7 and textures/chain_128.png.

   All seven sit at Voff 0, so the one 128 texture window set in maze_one_draw
   serves them all. */
#define MAZE_ONE_TEX_COUNT 7

static uint16_t tex_tpage[MAZE_ONE_TEX_COUNT];
static uint16_t tex_clut[MAZE_ONE_TEX_COUNT];

/* The two textures this room OWNS: RAM-resident from startup so the entry-time
   upload is a pure LoadImage with no CD read. Keep these in step with the slot
   numbering above and with NAME_TO_SLOT in gen_maze_one_tex_map.py. */
static int pipe_tex_id;
static int chain_tex_id;

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale — and note that at 118 KB it is
   THIS mesh that now sets the arena's size. */
/* ---- Cull keys -------------------------------------------------------------
   >>> THE REJECT PATH, NOT THE DRAW PATH, IS WHAT THIS ROOM SPENDS ITS FRAME ON.
   <<< Timed with the section counters: the frame is CPU-bound (the GPU wait sits
   at 2 hblanks of a 262-hblank frame) and the draw section runs 232. Only ~120
   of the 2056 primitives survive the distance cull, so roughly 1900 of them are
   pure overhead — and each one still cost a read of the primitive header for its
   stride, a read of its first vertex INDEX, and then a chase into the 118 KB
   vertex array for the coordinates. The R3000 has no data cache behind any of
   that; every one of those is a main-memory stall, and the mesh is far too big
   for the scratchpad.

   So the cull key is lifted out into its own array, built once at load: the
   first vertex's X and Z, plus the primitive's stride so the walk can advance
   without reading the header at all. A rejected primitive now costs ONE
   sequential 6-byte read and never touches the mesh. Identical output — this
   changes what the reject path READS, not what it decides.

   6 bytes x 2056 = 12 KB of BSS, alongside the 2 KB maze_one_nocull table that
   is already indexed the same way. Indices match the draw loop's `i`. */
/* Both tables live in the shared cull arena now rather than in this
   room's own BSS: only one room's are ever live, for exactly the reason
   only one room's MESH is (src/cull_arena.h). The names below are this
   file's own view of the arena, so everything under them reads as it
   always did. */
#define MoCullKey CullKey
#define mo_keys     cull_keys
static int       mo_key_count = 0;

/* ---- The SECOND key: each primitive's XZ bounding box ----------------------
   >>> THE DISTANCE CULL STOPPED READING THE MESH AND THE FRUSTUM TEST PUT IT
   BACK ONE TEST LATER. <<< This is the Greenhouse's gh_box, ported here after
   the measurement tools/DIAGNOSING_FRAME_RATE.txt asked for. That document says
   Maze One's vertex loop "runs on ~120 primitives, not 296, because a maze is
   not an open room" and should not get this unmeasured. IT WAS MEASURED, AND
   THAT FIGURE WAS TAKEN AT THE GATES. Counted offline against this mesh over
   16 headings (the tool is twenty lines; write one, do not estimate):

       camera at        distance-cull pass   ENTER THE VERTEX LOOP   submitted
       west gate spawn         263                  154                 60
       north gate spawn        271                  153                 60
       east gate spawn         254                  141                 56
       maze centre             562                  294                121
       BIRD CAGE POCKET        588                  329                123   <--

   The three gates are where the ~120 came from. The middle of the maze and the
   south-east pocket -- which is where the cage hangs, and where the lag was
   reported -- run at DOUBLE that, and 329 is more than the 296 that justified
   the box key in the Greenhouse. Ninety-four per cent of those 329 are then
   thrown away, each having chased v1..v3 out of a 118 KB vertex array with no
   data cache behind it. The gates are the cheap corners of this room; nobody
   had counted the middle of it.

   IT IS NOT AN APPROXIMATION OF THE VERTEX TEST, it is a cheaper way of taking
   it. The plane predicate is LINEAR in (x,z), so over a convex box its minimum
   is at a corner: all four corners outside a plane PROVES every vertex is
   outside it. Hole-free by construction. Verified anyway over the whole
   walkable footprint (x -100..7300, z -1300..4700 at 200-unit steps, 32
   headings): 10,897,337 primitive/pose combinations, ZERO holes, and it culls
   7,797,880 against the vertex loop's 7,727,873 -- 0.9% MORE, because most of
   this mesh is axis-aligned hedge and floor quads whose XZ box IS the quad.

   A SEPARATE ARRAY FROM mo_keys, for the Greenhouse's reason: the key is read
   for all 2056 primitives every frame, the box only by the ~590 that pass the
   distance cull. Widening the key to 14 bytes would put 8 bytes of extra
   traffic on the hot path to save reads on the cold one. Both live in the
   shared cull arena (src/cull_arena.h) and cost this room no BSS of its own:
   the arena is sized to 2056 primitives BECAUSE of this mesh, so the box came
   in free and the seven rooms between them gave 31 KB back.

   THE ARITHMETIC STAYS IN THE <<12 DOMAIN rather than shifting down first the
   way the vertex loop did. >>12 floors, and a floored comparison can go the
   wrong way by a unit at the screen edge -- tolerable when it only confirms a
   per-vertex answer, not tolerable when it is a proof about a box. The widest
   span here is 7600 x 4096 x 8, far inside an int32 (checked in the sweep
   above: zero overflows). */
#define MoCullBox CullBox
#define mo_box      cull_boxes

static void mo_build_cull_keys(void) {
    mo_key_count = 0;
    if (!maze_one_smd) return;
    uint8_t *p = (uint8_t *)maze_one_smd->p_prims;
    int i, n = maze_one_smd->n_prims;
    if (n > MAZE_ONE_PRIM_COUNT) n = MAZE_ONE_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &maze_one_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 1; k < nv; k++) {
            SVECTOR *vk = &maze_one_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
        }
        mo_keys[i].x      = v0->vx;
        mo_keys[i].z      = v0->vz;
        mo_keys[i].stride = pt->len;
        mo_keys[i].pad    = 0;
        mo_box[i].min_x = lo_x; mo_box[i].max_x = hi_x;
        mo_box[i].min_z = lo_z; mo_box[i].max_z = hi_z;
        p += pt->len;
    }
    mo_key_count = n;
}

void maze_one_load_geometry(void) {
    maze_one_buff = room_arena_load("\\TEX\\MAZEONE.SMD;1");
    maze_one_smd  = maze_one_buff ? smdInitData(maze_one_buff) : NULL;
    mo_build_cull_keys();
}

/* Read at STARTUP — the only safe time for CD access — the two textures this
   room owns. Geometry moved to maze_one_load_geometry above, and the other five
   slots are compile-time constants that cost nothing here. */
void maze_one_load_assets(void) {
    /* Every one of these is uploaded by another module; header only — no
       LoadImage, no second RAM copy. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRSSGS);
    TIM_SLOT(3, DRAIN);
    TIM_SLOT(4, PSNFLWR);

    /* This room's own: RAM-resident copies for a CD-free re-upload. */
    pipe_tex_id  = texmgr_register("\\TEX\\PIPE.TIM;1");
    TIM_SLOT(5, PIPE);
    chain_tex_id = texmgr_register("\\TEX\\CHAIN.TIM;1");
    TIM_SLOT(6, CHAIN);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).

   ORDER MATTERS on every line here. The courtyard's uploader (via the Garden
   Stairs') puts xt_dr_cg on x832 y0, chnlnk on x640 y0, brick_wall on x768 y0
   and gravel_gs on x704 y0 — which are where the drain, the flower beds, the
   pipe and the chain live — so all four of the following must go up AFTER it,
   never before. The two delegated calls are the NARROW ones by design:
   fountain_square_upload_textures and outside_catacombs_upload_textures would
   each also drop their own second texture on x768 y0, straight over the pipe. */
void maze_one_upload_textures(void) {
    garden_courtyard_upload_textures();   /* hedge, grdn_gte, grss_gs, gravel_gs */
    fountain_square_upload_drain();       /* drain   -> the xt_dr_cg slot        */
    outside_catacombs_upload_flowers();   /* flowers -> the chnlnk slot          */
    texmgr_upload(pipe_tex_id);           /* pipe    -> the brick_wall slot      */
    texmgr_upload(chain_tex_id);          /* chain   -> the gravel_gs slot       */
}

/* Just the pipe, for Maze Two through the north gate — which draws the same
   standpipe but no drain. The conservatory_upload_con_tile pattern: this room
   owns the only RAM copy, and a second texmgr_register in Maze Two would spend a
   registration to hold identical bytes (see tools/TEXTURING_NOTES.txt). Calling
   maze_one_upload_textures() wholesale instead would ALSO drop the drain on
   x832 y0, which is precisely where Maze Two's plinth goes. The caller must have
   run the courtyard's uploader first — brick_wall lands on this slot. */
void maze_one_upload_pipe(void) {
    texmgr_upload(pipe_tex_id);
}

/* Just the chain, for the Chain Room — which hangs the same four strands but
   draws no drain, no flower bed and no 8bpp pipe. The same reasoning as
   maze_one_upload_pipe above: this module owns the only RAM copy, and
   maze_one_upload_textures() wholesale would ALSO drop this room's pipe on
   x768 y0, which is precisely where the Chain Room's brick wall goes. The
   caller must have run the courtyard's uploader first — gravel_gs lands on this
   slot, and the Chain Room takes its gravel from x640 y0 instead so that this
   one is free. */
void maze_one_upload_chain(void) {
    texmgr_upload(chain_tex_id);
}

/* ---- The west-wall gate back to Fountain Square -----------------------------
   The grdn_gte polys on this side span z[-300,500] at x=-100, y[-600,0]. It is
   the same gate leaf as the one in Fountain Square's east hedge, which is 72
   units narrower there — the two rooms' meshes are independent, and only this
   pairing links them. The gate's own alcove is collision FLOOR 11,
   x(-100,100) z(-300,500), which is what fixes the centre at z=100.

   Collision wall 73 runs across the opening at x=-100 with nx = +4095, so the
   walkable side is +X and the player approaches from inside the maze. For a
   sign in the YZ plane that is mirror=0, and it stands 11 units EAST of the wall
   (x + 11). Both are the opposite hand from Fountain Square's side of the same
   gate, whose wall faces -X — getting either backwards comes out as mirrored
   text or a sign buried in the hedge. */
#define MO_GATE_X            (-100)
#define MO_GATE_Z              100    /* (-300 + 500) / 2, and FLOOR 11's centre */
#define MO_TEXT_Y             (-186)  /* eye level on the y=0 ground             */
#define MO_TEXT_RADIUS         1500
#define MO_FADE_NEAR           1000
#define MO_TRIGGER_RADIUS       500

/* Standing eye on the ground: floor y=0, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. The same expression Fountain Square and
   the Outside Catacombs use, and for the same reason — this whole run of garden
   rooms has its floor at y=0 while the courtyard below is at positive y. */
#define MO_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by maze_one_gate_arm(). Starts "held" so a press
   carried in through the transition doesn't bounce the player straight back. */
static int gate_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void maze_one_gate_arm(void) {
    gate_circle_prev = circle_held();
}

int maze_one_gate_triggered(void) {
    int held = circle_held();
    int just = held && !gate_circle_prev;
    gate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - MO_GATE_X;
    int32_t dz = cam_z - MO_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < MO_TRIGGER_RADIUS && interact_facing(MO_GATE_X, MO_GATE_Z);
}

/* Floating "Press O to enter" sign on the west gate. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. Sits just east (x+11) of the wall so it floats in front
   of the gate on the side the player is actually on. */
static void gate_text(RenderContext *ctx) {
    int32_t dx = cam_x - MO_GATE_X;
    int32_t dz = cam_z - MO_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= MO_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > MO_FADE_NEAR) {
        int range = MO_TEXT_RADIUS - MO_FADE_NEAR;
        int prog  = xz - MO_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        MO_GATE_X + 11, MO_TEXT_Y, MO_GATE_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving from Fountain Square: stand in the gate alcove east of the x=-100
   hedge, clear of the wall push radius (so the player isn't shoved on their
   first frame), facing +X — the direction of travel through the gate, looking
   down the first corridor of the maze. x lands at 120, which is inside collision
   FLOOR 10's x(100,7500) run, so the floor is under them immediately. */
void maze_one_spawn_west(void) {
    cam_x   = MO_GATE_X + COLLISION_WALL_RADIUS + 25;
    cam_y   = MO_EYE_Y;
    cam_vy  = 0;
    cam_z   = MO_GATE_Z;
    cam_rot = 1024;   /* facing +X, into the room */
    /* Arm ALL THREE gates, not just this one: a Circle held through the
       transition would otherwise fire whichever interaction was left unarmed. */
    maze_one_gate_arm();
    maze_one_ngate_arm();
    maze_one_egate_arm();
}

/* ---- The north-wall gate on to Maze Two -------------------------------------
   The second of this room's three modelled gates to be connected. Its grdn_gte
   polys span x[1501,2096] at z=4497, y[-600,0], and the alcove behind them is
   collision FLOOR 4, x(1501,2095) z(4300,4496) — which is what fixes the centre
   at x=1798.

   Collision wall 19 runs across the opening at z=4496 with nz = -4096, so the
   walkable side is -Z and the player approaches from inside the maze. For a sign
   in the XY plane that is mirror=0, and it stands 11 units SOUTH of the wall
   (z - 11). That is the opposite hand from Maze Two's side of the same gate,
   whose wall faces +Z — getting either backwards comes out as mirrored text or a
   sign buried in the hedge.

   The wall STAYS in the collision list: as with the west gate, the leaf is shut
   as far as collision is concerned and it is the trigger, not a hole, that lets
   the player through. */
#define MO_NGATE_X            1798    /* (1501 + 2095) / 2, and FLOOR 4's centre */
#define MO_NGATE_Z            4496

/* Its own Circle edge-detect. Two gates in one room means two independent edge
   states — sharing one would let a press consumed by the near gate re-arm the
   far one — and both are seeded on every entry by maze_one_init. */
static int ngate_circle_prev = 1;

void maze_one_ngate_arm(void) {
    ngate_circle_prev = circle_held();
}

int maze_one_ngate_triggered(void) {
    int held = circle_held();
    int just = held && !ngate_circle_prev;
    ngate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - MO_NGATE_X;
    int32_t dz = cam_z - MO_NGATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < MO_TRIGGER_RADIUS && interact_facing(MO_NGATE_X, MO_NGATE_Z);
}

/* Floating sign on the north gate. XY plane: door_draw_string_3d centres the
   reading axis (X) on world_x after adding 200, so pass door_x - 200. Same radii
   and fade as the west one; only the plane, the mirror flag and the sign's side
   of the wall differ (see above). */
static void ngate_text(RenderContext *ctx) {
    int32_t dx = cam_x - MO_NGATE_X;
    int32_t dz = cam_z - MO_NGATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= MO_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > MO_FADE_NEAR) {
        int range = MO_TEXT_RADIUS - MO_FADE_NEAR;
        int prog  = xz - MO_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        MO_NGATE_X - 200, MO_TEXT_Y, MO_NGATE_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Arriving back from Maze Two: stand SOUTH of the z=4496 hedge, clear of the
   wall push radius, facing -Z — the direction of travel through the gate. z
   lands at 4276, which is inside collision FLOOR 1's z(3698,4300) run and in the
   x gap between walls 1 and 15, so the player drops straight out of the gate
   alcove into the maze's northern corridor rather than onto its roof. x=1798 is
   297 clear of the alcove's side walls at x=1501 and x=2095, comfortably outside
   the 195 push radius. */
void maze_one_spawn_north(void) {
    cam_x   = MO_NGATE_X;
    cam_y   = MO_EYE_Y;
    cam_vy  = 0;
    cam_z   = MO_NGATE_Z - COLLISION_WALL_RADIUS - 25;
    cam_rot = 2048;   /* facing -Z, into the room */
    maze_one_gate_arm();
    maze_one_ngate_arm();
    maze_one_egate_arm();
}

/* ---- The east-wall gate on to the Keystone Maze -----------------------------
   The last of this room's three modelled gates to be connected, and the one its
   header used to describe as "there for a room that does not exist yet". Its
   grdn_gte polys span z[-900,-312] at x=7100, y[-600,0], and the alcove behind
   them is collision FLOOR 26, x(6901,7100) z(-900,-312) — which is what fixes
   the centre at z=-606.

   Collision wall 4 runs across the opening at x=7100 with nx = -4096, so the
   walkable side is -X and the player approaches from inside the maze. For a sign
   in the YZ plane that is mirror=1, and it stands 11 units WEST of the wall
   (x - 11). Both are the opposite hand from the WEST gate in this same room,
   whose wall faces +X, and the opposite hand again from the Keystone Maze's side
   of this gate, whose wall faces +X — getting any of them backwards comes out as
   mirrored text or a sign buried in the hedge.

   The wall STAYS in the collision list: as with the other two, the leaf is shut
   as far as collision is concerned and it is the trigger, not a hole, that lets
   the player through. */
#define MO_EGATE_X            7100
#define MO_EGATE_Z           (-606)   /* (-900 + -312) / 2, and FLOOR 26's centre */

/* Its own Circle edge-detect. Three gates in one room means three independent
   edge states — sharing one would let a press consumed by the near gate re-arm
   the far one — and all three are seeded on every entry by maze_one_init. */
static int egate_circle_prev = 1;

void maze_one_egate_arm(void) {
    egate_circle_prev = circle_held();
}

int maze_one_egate_triggered(void) {
    int held = circle_held();
    int just = held && !egate_circle_prev;
    egate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - MO_EGATE_X;
    int32_t dz = cam_z - MO_EGATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < MO_TRIGGER_RADIUS && interact_facing(MO_EGATE_X, MO_EGATE_Z);
}

/* Floating sign on the east gate. YZ plane, like the west one:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. Same radii and fade; only the mirror flag and the sign's
   side of the wall differ (see above). */
static void egate_text(RenderContext *ctx) {
    int32_t dx = cam_x - MO_EGATE_X;
    int32_t dz = cam_z - MO_EGATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= MO_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > MO_FADE_NEAR) {
        int range = MO_TEXT_RADIUS - MO_FADE_NEAR;
        int prog  = xz - MO_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        MO_EGATE_X - 11, MO_TEXT_Y, MO_EGATE_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving back from the Keystone Maze: stand WEST of the x=7100 hedge, clear of
   the wall push radius, facing -X — the direction of travel through the gate. x
   lands at 6880, which is 21 units outside the alcove's mouth at x=6901 and so
   inside collision FLOOR 25's x(6293,6901) z(-2095,1300) run, in the z gap
   between walls 11 and 12; the player drops straight out of the gate alcove into
   the corridor along the maze's east side rather than onto a hedge. z=-606 is
   294 clear of both alcove side walls, comfortably outside the 195 push
   radius. */
void maze_one_spawn_east(void) {
    cam_x   = MO_EGATE_X - COLLISION_WALL_RADIUS - 25;
    cam_y   = MO_EYE_Y;
    cam_vy  = 0;
    cam_z   = MO_EGATE_Z;
    cam_rot = 3072;   /* facing -X, into the room */
    maze_one_gate_arm();
    maze_one_ngate_arm();
    maze_one_egate_arm();
}

void maze_one_init(void) {
    maze_one_collision_init(&current_collision_room);
    /* The perimeter hedge is DRAWN to y=-500 (the gates reach -600, but they are
       the openings, not the roofline); the collision runs are only 333 tall,
       which is the proxy, not what the player sees. State the DRAWN value so
       anything ceiling-mounted hangs at the height the room actually has — see
       tools/ADDING_A_ROOM.txt on visual-vs-collision heights. */
    collision_set_ceiling_y(-500);
    /* No collision_set_wall_radius: the default 195 is right here. The maze
       corridors are about 600 wide, which at 195 leaves 210 units of walkable
       lane — the same margin Fountain Square's 728-wide parterre paths give at
       338, and comfortably clear. The courtyard's 260 would cut it to 80 and
       make the maze impassable. main.c resets to the default before every room
       init, so saying nothing is enough. */

    maze_one_floor_zones_init();

    /* Default spawn: the west gate, back into Fountain Square. main.c overrides
       it with maze_one_spawn_north() when the player is arriving from Maze Two,
       and maze_one_spawn_east() when they are coming back out of the Keystone
       Maze. Every spawn arms ALL THREE gates. */
    maze_one_spawn_west();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room's bounds are the largest in the game, so they certainly do. Clearing
       is safe: reception_init() re-places both on every reception entry. No save
       point of this room's own; the nearest is on the Garden Stairs' top
       landing. */
    save_points_clear();
    dressers_clear();

    /* The bird cage's examine prompt keeps its own Circle edge state, so it is
       armed here for the reason all three gates are: a press held through the
       transition must not post a log line on the arrival frame. After the spawn,
       so it reads the same input frame the gates did. */
    birdcage_init();
    /* The valve pipe's prompt keeps its own Circle edge state too, and is armed
       for the same reason and at the same moment. */
    valve_puzzle_arm();
}

static void draw_maze_one_smd(RenderContext *ctx) {
    if (!maze_one_smd) return;


    uint8_t *p = (uint8_t *)maze_one_smd->p_prims;
    int i, n = maze_one_smd->n_prims;

    /* Hoisted: all constant for the whole frame, and every one of them was being
       recomputed per primitive inside the hottest loop in the room — the two
       trig lookups once for each poly that passed the distance cull, the cull
       distance and the debug test all 2056 times. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = MO_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    /* The packet-buffer limit too: ctx->active_buffer cannot change inside a
       draw, so this was a double indirection recomputed for every primitive
       that got as far as being queued. */
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (mo_key_count < n) n = mo_key_count;   /* keys not built: draw nothing */

    for (i = 0; i < n; i++) {
        /* ---- The reject path. ONE sequential read, no mesh access ----------
           mo_keys carries this primitive's first vertex and its stride, so a
           primitive outside the view distance is skipped without touching the
           header or the vertex array at all. See mo_build_cull_keys. */
        uint8_t stride = mo_keys[i].stride;
        int32_t kdx = (int32_t)mo_keys[i].x - cam_x;
        int32_t kdz = (int32_t)mo_keys[i].z - cam_z;
        if ((kdx < 0 ? -kdx : kdx) + (kdz < 0 ? -kdz : kdz) > cull)
            { p += stride; continue; }

        {

            /* ---- SIDE-PLANE FRUSTUM CULL -------------------------------------
               The behind-the-camera test below rejects almost nothing on its
               own: measured over the whole mesh at the entry pose it threw away
               ZERO polys, because in a maze the geometry within the cull radius
               is all around you rather than behind you. Everything else was
               being handed to the GTE, transformed, and then — if it projected
               inside the GPU's +/-1023 coordinate limit, which is over three
               screens wide — queued as a primitive the GPU had to take in and
               throw away. Measured at the entry pose: 297 primitives submitted,
               133 of them inside the horizontal field of view. Sweeping the
               heading, 72% of what was submitted was off to the sides, rising to
               93% when facing into a hedge.

               The test is the two side planes of the frustum. With
               gte_SetGeomScreen(256) on a 320-wide screen the half-field is
               160/256, so a point is outside the right plane when
                   side > (5/8) * fwd,  i.e.  8*side > 5*fwd
               and outside the left when the same holds for -side. Both sides of
               that comparison carry isin/icos's <<12 scale, which cancels
               across the inequality, and at this room's scale the products stay
               far inside an int32 (the widest is 7600 x 4096 x 8).

               >>> A POLY IS ONLY CULLED WHEN EVERY VERTEX IS OUTSIDE THE SAME
               PLANE, AND HERE THAT IS DECIDED FROM ITS BOUNDING BOX RATHER THAN
               ITS VERTICES. <<< Testing v0 alone would slice through polys that
               straddle the screen edge. This used to walk v1..v3 out of the mesh
               to settle it — exact, and exactly the 118 KB scattered chase
               mo_keys exists to avoid, reintroduced one test later. Counted
               offline, ~329 primitives a frame entered that loop from the bird
               cage pocket and 94% of them were thrown away. See the long note on
               mo_box above for the measurement and the proof.

               The box is not an approximation of the test, it is a cheaper way
               of taking it: the plane predicate is LINEAR in (x,z), so over a
               convex box its minimum is at a corner. All four corners outside a
               plane therefore PROVES every vertex is outside it — no holes,
               whatever the mesh, and no offline verification needed. Checked
               anyway over the whole walkable footprint: 10.9M primitive/pose
               combinations, zero holes, and it culls 0.9% MORE than the vertex
               loop did.

               The arithmetic stays in the <<12 fixed-point domain rather than
               shifting down first, as the vertex loop did. >>12 floors, and a
               floored comparison can go the wrong way by a unit right at the
               screen edge — harmless when the answer is only being confirmed per
               vertex, not harmless when it is a proof about a box.

               Y is not tested. The camera is at standing height in a room whose
               hedges are drawn to y=-500 and whose floor is flat, so nothing is
               ever outside the top or bottom planes while inside the side ones;
               adding them would cost multiplies to reject nothing.
               ------------------------------------------------------------- */
            int32_t fwd = kdx * sn + kdz * cs;
            if (fwd < -(700 << 12))
                { p += stride; continue; }
            if (!no_frustum) {
                int32_t bx0 = (int32_t)mo_box[i].min_x - cam_x;
                int32_t bx1 = (int32_t)mo_box[i].max_x - cam_x;
                int32_t bz0 = (int32_t)mo_box[i].min_z - cam_z;
                int32_t bz1 = (int32_t)mo_box[i].max_z - cam_z;
                int out_r = 1, out_l = 1, c;
                for (c = 0; c < 4; c++) {
                    int32_t ex = (c & 1) ? bx1 : bx0;
                    int32_t ez = (c & 2) ? bz1 : bz0;
                    int32_t f  = ex * sn + ez * cs;
                    int32_t sd = ex * cs - ez * sn;
                    if (!( sd * 8 > f * 5)) out_r = 0;
                    if (!(-sd * 8 > f * 5)) out_l = 0;
                    if (!out_r && !out_l) break;
                }
                if (out_r || out_l) { p += stride; continue; }
            }

        }

        /* SURVIVED BOTH CULLS: only now is the primitive header read and the
           vertex array addressed. Everything above answers out of mo_keys and
           mo_box, so a rejected primitive touches neither. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &maze_one_smd->p_verts[vi[0]];
        SVECTOR *v1 = &maze_one_smd->p_verts[vi[1]];
        SVECTOR *v2 = &maze_one_smd->p_verts[vi[2]];

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
           build time in maze_one_nocull — same scheme as the other rooms. */
        int nocull = (i < MAZE_ONE_PRIM_COUNT) && maze_one_nocull[i];
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
            v3 = &maze_one_smd->p_verts[vi[3]];
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
        int32_t fog = dist < MO_FOG_NEAR ? MO_FOG_NEAR : (dist > MO_FOG_FAR ? MO_FOG_FAR : dist);
        int32_t fog_factor = ((MO_FOG_FAR - fog) << 8) / (MO_FOG_FAR - MO_FOG_NEAR);

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in maze_one_draw. */
        uint8_t tex_idx = (i < MAZE_ONE_PRIM_COUNT) ? maze_one_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < MAZE_ONE_TEX_COUNT);
        /* Purple fog, the same night sky the rest of the garden looks out on,
           and at Fountain Square's exact near/far — this is one continuous
           outdoors and the gate between them is not a change in the weather. */
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

void maze_one_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = MO_FOG_NEAR; g_fog_far = MO_FOG_FAR;

    /* Background in the SAME colour the fog saturates to, so a poly that has
       faded out is indistinguishable from the void behind it and the cull never
       shows a seam. Purple here, matching the rest of the garden.

       Painted by the HARDWARE CLEAR, not by a full-screen TILE like the other
       eighteen rooms — the draw environments already clear the framebuffer
       (isbg=1) before anything is drawn, so a tile on top of that was a second
       full-screen fill of the same 77k pixels every frame. See the note on
       render_set_clear_colour. This room is the trial: it is the one measured
       sitting within a millisecond of the vblank boundary, and if the reading
       moves the same change is worth rolling out to the rest. */
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
       the background colour rather than popping off at the cull line. Without
       this the rungs would all look wrong and the choice would be made on a
       picture the shipped game never draws. */
    if (DEBUG_CULL_DIST()) g_fog_far = DEBUG_CULL_DIST();

    if (exp != DBG_EXP_NO_MESH) draw_maze_one_smd(ctx);
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
       sample the hedge (see tools/TEXTURING_NOTES.txt PART 5). The zombies and
       spiders are still seeded empty here; the FIVE RAFFLESIAS (one per
       poison_flower_base bed, rafflesias_init) and TWO MUSHROOM HEADS
       (world_seed_room) are real.

       Both of those are sound-legal here for the same reason Fountain Square's
       mushroom is: this room is on SND_BANK_GARDEN, which is where SFX_HISS and
       the flowers' own loops live (src/sound.h). */
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

    /* Last: the gate signs, and the bird cage's examine prompt. All four are
       world-space pixel text and want the view matrix that is still loaded. */
    gate_text(ctx);
    ngate_text(ctx);
    egate_text(ctx);
    birdcage_text(ctx);
    valve_puzzle_text(ctx);   /* ...and the standpipe's, same font, same window */

    /* Dead last, and NOT world-space: the valve board is a 2D overlay on the
       menu's OT range, so it goes on top of everything the room has drawn. The
       kitchen calls stove_puzzle_draw from the same place for the same reason. */
    valve_puzzle_draw(ctx);
}
