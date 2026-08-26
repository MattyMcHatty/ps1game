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
#include "stables.h"
#include "collision.h"
#include "player.h"
#include "stables_mesh_collision.h"
#include "stables_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "garden_courtyard.h"    /* garden_courtyard_upload_textures      */
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "living_statue.h"
#include "hadad.h"
#include "web.h"
#include "item_pickup.h"
#include "sml_med.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* The Stables: the walled yard west of the Rear Gate. See stables.h for the
   layout. TWO openings now: the east gate back to the Rear Gate, and the
   greenhouse door in the west wall, which since the Greenhouse was built leads
   somewhere instead of being drawn shut. */

static SMD  *stables_smd  = NULL;
static void *stables_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   FOUNTAIN SQUARE'S EXACTLY, which the user asked for: 575/2500, the same purple
   SKY_FOG_* colour, and the cull equal to the fog-far so nothing is dropped
   until the fog has already faded it into the background. It is also what the
   Rear Gate next door runs, so the step through its west gate changes the plan
   of the place but not the weather.

   It suits the room on its own account too. The footprint is 3500 x 3200, so
   from the gate alcove the far wall is at the very edge of the fog and the two
   stable blocks resolve as you walk down the lane between them; and the
   greenhouse standing beyond the west wall — 1100 tall, and 610 further out
   again — is a silhouette rather than a building until you have crossed the
   yard, which is the whole reason it is drawn. */
#define ST_CULL_DIST      2500
#define ST_FOG_NEAR        575
#define ST_FOG_FAR        2500

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, and honestly one: the collision generator reported two planes and
   BOTH are at y=0 — FLOOR 0 the yard, x(-3400,-100) z(-1600,1600), and FLOOR 1
   the gate alcove, x(-100,100) z(-400,400). A single rect over the collision
   bounds covers them both with nothing left over that the player can reach.

   It covers more than the walkable area — the insides of the two stable blocks
   fall inside it — but the player is kept out of those by walls 0-5 and 13-14,
   and giving them zones of their own would only help someone who had already
   clipped a corner. Fountain Square, Maze One and the Rear Gate's flat half all
   do the same. */
static void stables_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -3400; floor_zones[0].max_x =  100;
    floor_zones[0].min_z = -1600; floor_zones[0].max_z = 1600;
    floor_zones[0].y     = 0;

    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   EIGHT mesh textures, and this room OWNS FOUR of them — more than any other
   room in the game, and the reason the GARDEN-WEST VRAM BANK exists. Every room
   before this one had to hunt for a slot that was already time-shared so that it
   owed nobody a restore; this room and the Greenhouse behind it are reached only
   through the Rear Gate's west gate and nothing else is drawn while the player
   is in them, so the whole room-art half of VRAM is theirs to lay out. See
   tools/VRAM_MAP_GARDEN_WEST.txt for the map, and note what it does NOT contain:
   no mansion art at all, only the globals that have to survive everywhere (the
   HUD, the weapons and collectibles, the door panels, and the Rafflesia and
   Mushroom Head sprite sets, so both of those enemies are placeable here).

     0 hedge           the east perimeter and the gate's alcove
                                                        (clsd_drwr page, x384 y0)
     1 grdn_gte        the gate leaf itself              (kchn_wl page,   x512 y0)
     2 grss_gs         the yard                          (stn_stl page,   x320 y0)
     3 brick_wall      the north, south and west walls   (x768 y0)
     4 greenhouse      the building beyond the west wall (trck_clue page, x640 y0)
                                                                            OWNED
     5 stables wood    the two stable blocks — 480 of the room's 945 polys
                                                        (opn_drwr page,  x832 y0)
                                                                            OWNED
     6 stable glyphs   the markings on the blocks' long faces
                                                      (rusty_fence page, x704 y0)
                                                                            OWNED
     7 greenhouse door the shut door in the west wall  (frnt_dr page,  x320 y256)
                                                                            OWNED

   Slots 0-3 come from the Garden Courtyard's uploader (which itself runs
   garden_stairs_upload_textures first, and it is that call which puts brick_wall
   up) — the same four every garden room since the courtyard has drawn, and they
   cost this room nothing but four TIM_SLOT lines.

   >>> THIS IS THE FIRST ROOM TO DRAW BRICK_WALL AND GRSS AT ONCE. <<< Their own
   TIMs both live at x768 y0 — they are an intentional streaming PAIR — so one of
   them had to give. 'grss' resolves to the grss_gs CLONE at x320 y0, as it does
   in every garden room since the Garden Stairs, and that is what makes the pair
   drawable together. See gen_stables_tex_map.py.

   The four this room owns went to pages it draws NOTHING from, which is the same
   test every other room applies, but with the bank behind it there was no
   scramble: the two full 8bpp pages went to the two textures that need 256
   colours (the greenhouse and the stable wood), and the two 4bpp half-pages to
   the two that do not.

     greenhouse      -> x640 y0, the trck_clue/plinth_rg page. Already shared six
                        ways and every occupant is re-uploaded by its own room on
                        entry: delivery restores gravel_texture+trees, the
                        stairwells chnlnk, the attic stairwell trck_clue, the
                        catacombs and both mazes the flower bed, the Rear Gate
                        its plinth_rg.
     stables wood    -> x832 y0, the opn_drwr/drain page. Same test: this room
                        has no drain, no plinth, no con_tile and no double door.
     stable glyphs   -> x704 y0, the rusty_fence/gravel_gs page. 4bpp, so it
                        occupies only x[704,736) and stays clear of anzu3/anzu6
                        at x736 — the trap Maze Two fell into by putting an 8bpp
                        texture on this page. This room draws no gravel at all.
     greenhouse door -> x320 y256, the frnt_dr/red_crpt/dbl_dr_rg page. 4bpp,
                        left half only, clear of graveolver at x352; the same
                        slot and the same reasoning as the Rear Gate's
                        dbl_dr_rg, of which this is now the third sharer.

   All eight sit at Voff 0, so the one 128 texture window in stables_draw serves
   them all. */
#define STABLES_TEX_COUNT 8

static uint16_t tex_tpage[STABLES_TEX_COUNT];
static uint16_t tex_clut[STABLES_TEX_COUNT];

/* The four textures this room OWNS: RAM-resident from startup so the entry-time
   upload is a pure LoadImage with no CD read. Keep this table in step with the
   slot numbering above and with NAME_TO_SLOT in gen_stables_tex_map.py.

   FOUR REGISTRATIONS is the most any room has taken, and it is worth saying why
   that is affordable rather than reckless: texmgr's cap is 64 (see the comment
   in src/texmgr.c) and every other texture here comes in through another
   module's uploader, so this room adds four and no more. Count what is actually
   registered before adding a fifth anywhere — a texmgr_register past the cap
   fails SILENTLY and breaks that texture in every room that draws it. */
#define STABLES_NEW_TEX 4
static int new_tex_id[STABLES_NEW_TEX];
static const char *new_tex_file[STABLES_NEW_TEX] = {
    "\\TEX\\GRNHOUSE.TIM;1",   /* slot 4 */
    "\\TEX\\STBLWOOD.TIM;1",   /* slot 5 */
    "\\TEX\\STBLGLPH.TIM;1",   /* slot 6 */
    "\\TEX\\GRNHSDR.TIM;1",    /* slot 7 */
};

/* ---- Cull keys -------------------------------------------------------------
   The Rear Gate's scheme, and for the same reason: the cull key is lifted out
   into its own array, built once at load — the first vertex's X and Z plus the
   primitive's stride — so a rejected primitive costs ONE sequential 6-byte read
   and never touches the mesh or its header at all. Identical output; this
   changes what the reject path READS, not what it decides.

   The margin is smaller here than in the Rear Gate: 945 primitives over
   3500 x 3200 against a 2500 Manhattan cull, so rather more of them are in range
   from anywhere the player can stand. It is kept because it costs 6 bytes a
   primitive (5.5 KB of BSS) and because the draw loop is otherwise copied
   verbatim from that room — diverging the two would make the next optimisation
   land in one and not the other. Indices match the draw loop's `i`. */
typedef struct { int16_t x, z; uint8_t stride, pad; } StCullKey;
static StCullKey st_keys[STABLES_PRIM_COUNT];
static int       st_key_count = 0;

static void st_build_cull_keys(void) {
    st_key_count = 0;
    if (!stables_smd) return;
    uint8_t *p = (uint8_t *)stables_smd->p_prims;
    int i, n = stables_smd->n_prims;
    if (n > STABLES_PRIM_COUNT) n = STABLES_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &stables_smd->p_verts[vi[0]];
        st_keys[i].x      = v0->vx;
        st_keys[i].z      = v0->vz;
        st_keys[i].stride = pt->len;
        st_keys[i].pad    = 0;
        p += pt->len;
    }
    st_key_count = n;
}

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. At 53 KB this mesh is well
   inside the arena Maze One's 117 KB sizes, so nothing there had to change. */
void stables_load_geometry(void) {
    stables_buff = room_arena_load("\\TEX\\STABLES.SMD;1");
    stables_smd  = stables_buff ? smdInitData(stables_buff) : NULL;
    st_build_cull_keys();
}

/* Read at STARTUP — the only safe time for CD access — the four textures this
   room owns. Geometry moved to stables_load_geometry above, and the other four
   slots are compile-time constants that cost nothing here. */
void stables_load_assets(void) {
    /* Borrowed: header only — no LoadImage, no second RAM copy. All four are put
       in VRAM by garden_courtyard_upload_textures() on the way in. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRSSGS);
    TIM_SLOT(3, BRIKWLL);

    /* This room's own four: RAM-resident copies for a CD-free re-upload. */
    for (int i = 0; i < STABLES_NEW_TEX; i++)
        new_tex_id[i] = texmgr_register(new_tex_file[i]);
    TIM_SLOT(4, GRNHOUSE);
    TIM_SLOT(5, STBLWOOD);
    TIM_SLOT(6, STBLGLPH);
    TIM_SLOT(7, GRNHSDR);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).

   ORDER MATTERS, and it matters more here than anywhere else. The courtyard's
   uploader puts chnlnk on x640 y0, xt_dr_cg on x832 y0 and gravel_gs on
   x704 y0 — which are where the greenhouse, the stable wood and the glyphs live
   — so all four of this room's own textures must go up AFTER it, never before.
   Three of the four would be silently replaced by garden art otherwise, which
   looks exactly like a texture that failed to load.

   The greenhouse door's page (x320 y256) is touched by nothing in this chain, so
   its order is free; it is uploaded with the rest to keep the set together. */
void stables_upload_textures(void) {
    garden_courtyard_upload_textures();   /* hedge, grdn_gte, grss_gs, and
                                             brick_wall via the stairs'      */
    for (int i = 0; i < STABLES_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);     /* greenhouse -> chnlnk, wood -> xt_dr_cg,
                                             glyphs -> gravel_gs, door -> frnt_dr */
}

/* ---- The east-wall gate back to the Rear Gate -------------------------------
   The grdn_gte polys on this side span z[-400,400] at x=100, y[-600,0]. It is
   the same gate leaf as the one in the Rear Gate's west hedge, which stands at
   x=-2200 z[1500,2300] there — the two rooms' meshes are independent, and only
   this pairing links them. The gate's own alcove is collision FLOOR 1,
   x(-100,100) z(-400,400), which is what fixes the centre at z=0.

   Collision wall 10 runs across the opening at x=100 with nx = -4096, so the
   walkable side is -X and the player approaches from inside this room. For a
   sign in the YZ plane that is mirror=1, and it stands 11 units WEST of the wall
   (x - 11). Both are the opposite hand from the Rear Gate's side of the same
   gate, whose wall faces -X into that room. Getting either backwards comes out
   as mirrored text or a sign buried in the hedge.

   The wall STAYS in the collision list: the leaf is shut as far as collision is
   concerned and it is the trigger, not a hole, that lets the player through. */
#define ST_GATE_X              100
#define ST_GATE_Z                0    /* (-400 + 400) / 2, and FLOOR 1's centre */
#define ST_TEXT_Y            (-186)   /* eye level on the y=0 yard              */
#define ST_TEXT_RADIUS        1500
#define ST_FADE_NEAR          1000
#define ST_TRIGGER_RADIUS      500

/* Standing eye in the yard: floor y=0, less GROUND_FLOOR_Y and the 40-unit floor
   standoff apply_height applies. The same expression Fountain Square, the
   Outside Catacombs, both mazes and the Rear Gate's lawn use, and for the same
   reason — this whole run of garden rooms has its floor at y=0 while the
   courtyard below is at positive y. */
#define ST_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, one per opening, each seeded by its own *_arm(). Both
   start "held" so a press carried in through a transition doesn't bounce the
   player straight back out of the room they just entered. Every spawn arms
   BOTH, so whichever door the player arrives by, the other is safe too. */
static int gate_circle_prev  = 1;
static int gdoor_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void stables_gate_arm(void) {
    gate_circle_prev = circle_held();
}

int stables_gate_triggered(void) {
    int held = circle_held();
    int just = held && !gate_circle_prev;
    gate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - ST_GATE_X;
    int32_t dz = cam_z - ST_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < ST_TRIGGER_RADIUS && interact_facing(ST_GATE_X, ST_GATE_Z);
}

/* Floating "Press O to enter" sign on the gate. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. Sits just west (x-11) of the wall so it floats in front
   of the gate on the side the player is actually on. */
static void gate_text(RenderContext *ctx) {
    int32_t dx = cam_x - ST_GATE_X;
    int32_t dz = cam_z - ST_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= ST_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > ST_FADE_NEAR) {
        int range = ST_TEXT_RADIUS - ST_FADE_NEAR;
        int prog  = xz - ST_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        ST_GATE_X - 11, ST_TEXT_Y, ST_GATE_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* ---- The west-wall greenhouse door, into the Greenhouse ---------------------
   The greenhouse door polys stand at x=-3400, z[-133,133], y[-500,0], in the YZ
   plane. Behind them, until August 2026, was nothing: the door was drawn shut
   and backed onto collision wall 15 because the room it opens into did not
   exist. It does now (src/greenhouse.h), and the two meshes were modelled in a
   shared world - greenhouse_x = stables_x + 3500, greenhouse_z = stables_z - 100
   maps this door onto that room's, at x=100 z=-100.

   Collision wall 15 runs the FULL west side at x=-3400 with nx = +4096, so the
   walkable side is +X and the player approaches from inside this room. For a
   sign in the YZ plane that is mirror=0, and it stands 11 units EAST of the wall
   (x + 11). Both are the opposite hand from the east gate at the other end of
   the yard, whose wall faces -X - and the opposite of the Greenhouse's side of
   this same door. Getting either backwards comes out as mirrored text or a sign
   buried in the brickwork.

   The wall STAYS in the collision list, exactly as the east gate's does: the
   door is shut as far as collision is concerned, and it is the trigger rather
   than a hole in the wall that lets the player through. Nothing about wall 15
   changed when the Greenhouse was built. */
#define ST_GDOOR_X          (-3400)
#define ST_GDOOR_Z               0    /* (-133 + 133) / 2                      */

void stables_gdoor_arm(void) {
    gdoor_circle_prev = circle_held();
}

int stables_gdoor_triggered(void) {
    int held = circle_held();
    int just = held && !gdoor_circle_prev;
    gdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - ST_GDOOR_X;
    int32_t dz = cam_z - ST_GDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < ST_TRIGGER_RADIUS && interact_facing(ST_GDOOR_X, ST_GDOOR_Z);
}

/* The greenhouse door's sign. Same YZ-plane arrangement as the gate's, mirrored
   the other way and standing on the +X side, because the player reads this one
   from the east. */
static void gdoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - ST_GDOOR_X;
    int32_t dz = cam_z - ST_GDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= ST_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > ST_FADE_NEAR) {
        int range = ST_TEXT_RADIUS - ST_FADE_NEAR;
        int prog  = xz - ST_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        ST_GDOOR_X + 11, ST_TEXT_Y, ST_GDOOR_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving back out of the Greenhouse: stand EAST of the x=-3400 wall, clear of
   the wall push radius, facing +X down the yard toward the two stable blocks and
   the gate beyond them. x lands at -3180, which is 380 west of the near block's
   x=-2799 face, so nothing pushes the player anywhere on the frame they arrive. */
void stables_spawn_west(void) {
    cam_x   = ST_GDOOR_X + COLLISION_WALL_RADIUS + 25;
    cam_y   = ST_EYE_Y;
    cam_vy  = 0;
    cam_z   = ST_GDOOR_Z;
    cam_rot = 1024;   /* facing +X, into the yard */
    stables_gate_arm();
    stables_gdoor_arm();
}

/* Arriving from the Rear Gate: stand WEST of the x=100 gate wall, clear of the
   wall push radius (so the player isn't shoved on their first frame), facing -X
   — the direction of travel through the gate, looking down the lane between the
   two stable blocks at the greenhouse standing beyond the far wall.

   x lands at -120, which is 20 units past the alcove mouth at x=-100 and so out
   on the open yard rather than inside the alcove; that is deliberate, and it is
   also past the ends of both alcove side walls (6 and 11 run only z[400,1600]
   and z[-1600,-400]), so nothing pushes the player anywhere on the frame they
   arrive. The nearest wall from there is the gate itself, 220 behind. */
void stables_spawn_east(void) {
    cam_x   = ST_GATE_X - COLLISION_WALL_RADIUS - 25;
    cam_y   = ST_EYE_Y;
    cam_vy  = 0;
    cam_z   = ST_GATE_Z;
    cam_rot = 3072;   /* facing -X, into the room */
    stables_gate_arm();
    stables_gdoor_arm();
}

void stables_init(void) {
    stables_collision_init(&current_collision_room);
    /* The perimeter is DRAWN to y=-500 — hedge on the east side, brick wall on
       the other three — and the collision runs are 500 tall to match. State the
       DRAWN value so anything ceiling-mounted hangs at the height the room
       actually has (see tools/ADDING_A_ROOM.txt on visual-vs-collision heights).

       NOTE THAT THIS IS THE PERIMETER, NOT THE TALLEST THING IN THE ROOM. The
       two stable blocks are drawn AND collide to -600, and the greenhouse beyond
       the west wall reaches -1100 — but that stands outside the collision bounds
       entirely (x < -3400) and nothing can ever be placed on it. Anything hung
       against a stable block wants that block's -600, not this. */
    collision_set_ceiling_y(-500);
    /* No collision_set_wall_radius: the default 195 is right here. The tightest
       space is the 800-wide lane between the two stable blocks, which at 195
       leaves 410 units of walkable width — wider than either maze. The 400-unit
       strips north and south of the blocks are the real squeeze at 205, which is
       still the margin the Rear Gate's hedged corridor runs on. main.c resets to
       the default before every room init, so saying nothing is enough. */

    stables_floor_zones_init();

    /* Default to the east gate, back into the Rear Gate; main.c overrides it
       with stables_spawn_west() when the player has come back out of the
       Greenhouse. Either spawn arms BOTH openings, so whichever branch runs the
       other one is safe. */
    stables_spawn_east();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room spans x[-3400,100] z[-1600,1600], which puts reception's save point
       at (78,-67) squarely in the gate alcove. Clearing is safe: reception_init()
       re-places both on every reception entry. No save point of this room's own;
       the nearest is on the Garden Stairs' top landing. */
    save_points_clear();
    dressers_clear();
}

static void draw_stables_smd(RenderContext *ctx) {
    if (!stables_smd) return;

    uint8_t *p = (uint8_t *)stables_smd->p_prims;
    int i, n = stables_smd->n_prims;

    /* Hoisted: all constant for the whole frame, and every one of them would
       otherwise be recomputed per primitive inside the hottest loop in the room —
       the two trig lookups once for each poly that passed the distance cull, the
       cull distance and the debug test all 945 times. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = ST_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    if (st_key_count < n) n = st_key_count;   /* keys not built: draw nothing */

    for (i = 0; i < n; i++) {
        /* ---- The reject path. ONE sequential read, no mesh access ----------
           st_keys carries this primitive's first vertex and its stride, so a
           primitive outside the view distance is skipped without touching the
           header or the vertex array at all. See rg_build_cull_keys. */
        uint8_t stride = st_keys[i].stride;
        int32_t kdx = (int32_t)st_keys[i].x - cam_x;
        int32_t kdz = (int32_t)st_keys[i].z - cam_z;
        if ((kdx < 0 ? -kdx : kdx) + (kdz < 0 ? -kdz : kdz) > cull)
            { p += stride; continue; }

        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &stables_smd->p_verts[vi[0]];
        SVECTOR *v1 = &stables_smd->p_verts[vi[1]];
        SVECTOR *v2 = &stables_smd->p_verts[vi[2]];

        {
            /* ---- SIDE-PLANE FRUSTUM CULL -------------------------------------
               Maze One's test verbatim; read the long note there for the whole
               argument. In short: the behind-the-camera test below rejects
               almost nothing on its own, because within the cull radius the
               geometry is all around you rather than behind you, and everything
               else was being handed to the GTE, transformed, and then — if it
               projected inside the GPU's +/-1023 coordinate limit, which is over
               three screens wide — queued as a primitive the GPU had to take in
               and throw away.

               The test is the two side planes of the frustum. With
               gte_SetGeomScreen(256) on a 320-wide screen the half-field is
               160/256, so a point is outside the right plane when
                   side > (5/8) * fwd,  i.e.  8*side > 5*fwd
               and outside the left when the same holds for -side. Both sides of
               that comparison are world units (the >>12 undoes isin/icos), and
               at this room's scale the products stay far inside an int32.

               >>> A POLY IS ONLY CULLED WHEN EVERY VERTEX IS OUTSIDE THE SAME
               PLANE. <<< That makes the test exact rather than conservative —
               it can never remove a poly with a vertex inside the true frustum,
               whatever the mesh — and it is why the loop is written this way
               rather than around a bounding sphere on v0, which measured barely
               half as effective on Maze One's mesh.

               Y is not tested, and that is still right here even though this
               room, unlike the mazes, has a ramp: adding top and bottom planes
               would cost multiplies to reject nothing, because the camera never
               leaves standing height over a surface that spans only 500 units.
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
                    int nv = is_quad ? 4 : 3, k, all_out = 1;
                    for (k = 1; k < nv; k++) {
                        SVECTOR *vk = &stables_smd->p_verts[vi[k]];
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
           build time in stables_nocull — same scheme as the other rooms. This
           mesh has none either (gen_stables_tex_map.py reports 0), but the
           table is generated and checked all the same. */
        int nocull = (i < STABLES_PRIM_COUNT) && stables_nocull[i];
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
            v3 = &stables_smd->p_verts[vi[3]];
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
        int32_t fog = dist < ST_FOG_NEAR ? ST_FOG_NEAR : (dist > ST_FOG_FAR ? ST_FOG_FAR : dist);
        int32_t fog_factor = ((ST_FOG_FAR - fog) << 8) / (ST_FOG_FAR - ST_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in stables_draw. */
        uint8_t tex_idx = (i < STABLES_PRIM_COUNT) ? stables_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < STABLES_TEX_COUNT);
        /* Purple fog, the same night sky the rest of the garden looks out on,
           at FOUNTAIN SQUARE's exact near/far — the user asked for this room to
           match that one, and it is the same 575/2500 the Rear Gate next door
           already runs. One continuous outdoors; the gate between them is not a
           change in the weather. */
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


void stables_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = ST_FOG_NEAR; g_fog_far = ST_FOG_FAR;

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
       page. All EIGHT of this room's textures sit at page-top (Voff 0) — the
       four it owns were placed with that as a hard requirement, and the four it
       borrows have always been there — so one window serves them (see
       tools/VRAM_MAP_GARDEN_WEST.txt). */
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

    if (exp != DBG_EXP_NO_MESH) draw_stables_smd(ctx);

    /* Every sprite enemy renderer is handed this room's texture window, because
       all of their sprites live at Voff >= 128 and must bracket it rather than
       sample the stable wood (see tools/TEXTURING_NOTES.txt PART 5). NOTHING is
       placed here yet — the updaters and draws below run over empty arrays and
       cost nothing — but they are wired up so that a future spawn brackets its
       quad correctly instead of drawing this room's grass on a monster.

       THE RAFFLESIA AND THE MUSHROOM HEAD ARE THE TWO THAT ARE READY. Both were
       put in the garden-west bank deliberately (the user asked for them): the
       mushrooms own their four VRAM slots outright and are resident for the
       whole run, and main.c's pending_area test now streams the RAFFLESIA half
       of the shared x320/x384 y128 sprite pair in on entry here rather than the
       spiders' — this is a garden room and spiders are house-interior. Both are
       also sound-legal: this room is on SND_BANK_GARDEN (main.c's
       STATE_LOADING), which is where SFX_HISS and the flowers' own loops live.
       Check anything else against src/sound.h the way Maze One's placement
       was. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
        rafflesias_set_texwindow(&tw);
        mushrooms_set_texwindow(&tw);
        living_statues_set_texwindow(&tw);
        hadads_set_texwindow(&tw);
    }

    {
        draw_zombies(ctx);
        draw_spiders(ctx);
        draw_rafflesias(ctx);
        draw_mushrooms(ctx);
        draw_living_statues(ctx);
        draw_hadads(ctx);
        webs_draw(ctx);
        item_pickups_draw(ctx);
        sml_meds_draw(ctx);
    }

    /* Last: the two signs, one at each end of the yard. */
    gate_text(ctx);
    gdoor_text(ctx);
}
