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
#include "the_hatch.h"
#include "collision.h"
#include "the_hatch_mesh_collision.h"
#include "the_hatch_tex_map.h"
#include "hatch_doors.h"        /* the two leaves over the pit */
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "cdaudio.h"             /* suspend/resume around the entry-time read */
#include "dresser.h"
#include "garden_courtyard.h"    /* garden_courtyard_upload_textures      */
#include "maze_two.h"            /* maze_two_upload_plinth                */
#include "maze_one.h"            /* maze_one_upload_chain                 */
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

/* The Hatch: the walled lawn east of the Keystone Maze. See the_hatch.h for the
   layout, the pit and the hatch itself. */

static SMD  *the_hatch_smd  = NULL;
static void *the_hatch_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   MAZE ONE'S EXACTLY, which is Fountain Square's and the Chain Room's exactly:
   575/2500, the same purple SKY_FOG_* colour, and the cull equal to the fog-far
   so nothing is dropped until the fog has already faded it into the background.
   The whole garden from the courtyard's gate onwards is one continuous outdoors,
   and the step through a gate changes the plan of the place but not the weather.

   Unlike the Chain Room, the distance cull DOES fire here: the footprint is
   5000 x 5400 and its far corners are 10400 Manhattan apart, so from the north
   chamber the west end of the corridor is well past 2500 and fades out into the
   sky rather than popping. That is the same behaviour the mazes have and it is
   why the number is not raised — the hedges block the sight lines that would
   otherwise make the fade visible as a wall of nothing. */
#define TH_CULL_DIST      2500
#define TH_FOG_NEAR        575
#define TH_FOG_FAR        2500

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, as all three mazes and the Chain Room. The collision generator found
   EIGHT floor planes and every one is at y=0 — the corridor, the north passage,
   the chamber, and the five rectangles the yard is cut into by its four corner
   blocks — so this room has no step, ramp or storey anywhere in it. One rect over
   the collision bounds covers all eight, plus the hedge blocks and the four
   plinth corners between them, which the player is kept out of by the 500-tall
   wall runs.

   >>> IT ALSO COVERS THE PIT, ON PURPOSE. <<< x(3000,4200) z(-300,300) is a hole
   in the lawn with grass sides falling to a black floor 1200 below and NO
   collision floor down there. Extending the zone over it is right rather than
   sloppy: walls 20..23 fence the hole on all four sides facing outward, so the
   player cannot get above the gap, and holding them at lawn height is exactly
   what is wanted while they stand at the lip. A zone that stopped at the lip
   would drop the player through the moment the push radius let them lean over
   it. See the_hatch.h. */
static void the_hatch_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x =  -200; floor_zones[0].max_x = 4800;
    floor_zones[0].min_z = -1499; floor_zones[0].max_z = 3900;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Seven mesh textures and the room owns exactly ONE. The other six are already
   in VRAM through rooms this one is reached past, so for those it holds no
   texture RAM at all: it takes the tpage/clut headers as compile-time constants
   out of tim_slots.h and delegates the entry-time LoadImage to whichever module
   holds the RAM copy.

     0 hedge       the perimeter and every run    (clsd_drwr page, x384 y0)
     1 grdn_gte    the west gate                  (kchn_wl page,   x512 y0)
     2 grss_gs     the lawn, and the pit's sides  (stn_stl page,   x320 y0)
     3 brick_wall  the well in the north chamber  (own page,       x768 y0)
     4 plinth      the yard's four corner blocks  (opn_drwr page,  x832 y0)
     5 chain       the four chains on the lid     (rusty_fence pg, x704 y0)
     6 hatch       THE LID                        (trck_clue page, x640 y0) OWNED

   Slots 0-3 come from the Garden Courtyard's uploader (which itself runs
   garden_stairs_upload_textures first, and that is what puts brick_wall up) and
   cost this room NOTHING. As everywhere in the garden chain, the mesh's 'grss'
   material resolves to the grss_gs CLONE rather than to its own page — forced
   rather than tidy here, because grss.tim's own home is x768 y0, which is this
   room's brick wall.

   Slots 4 and 5 are taken through NARROW accessors on the modules that own
   them, maze_two_upload_plinth() and maze_one_upload_chain(), rather than
   through either room's full uploader. That is not tidiness: Maze Two's would
   also stamp its borrowed pipe on x768 y0 and Maze One's its own pipe on the
   same page, which is exactly where this room's brick wall is. Both accessors
   already existed — the Keystone Maze added the first and the Chain Room the
   second, for this same clash.

   Slot 6 is the one thing this room owns, and it went to the trck_clue page at
   x640 y0: an 8bpp full page this room draws nothing from. That page is the one
   big slot left once the six borrowed ones are accounted for — no chnlnk, no
   gravel of any kind (this room is grass and brick throughout, the first garden
   room since Maze One with no paving at all), no flower bed, no trees, no
   plinth_rg, no greenhouse. It adds no restore obligation: the page is already
   time-shared seven ways and every consumer re-uploads on its own entry.

   Its CLUT is borrowed on the same argument, and this matters more than the
   pixels do — y=511 x[288,544) is still the ONLY free 256-word CLUT run in the
   whole map, and a hatch lid is not what to spend the last of it on. `hatch`
   takes plinth_rg's palette row at (256,506), a texture whose pixels it is
   already displacing on x640 y0, so the pair goes back together the moment
   rear_gate_upload_textures runs again. The Greenhouse's trick, and the reason
   it exists (tools/ADDING_A_ROOM.txt STEP 3c).

   All seven sit at Voff 0, so the one 128 texture window set in the_hatch_draw
   serves them all. */
#define THE_HATCH_TEX_COUNT 7

static uint16_t tex_tpage[THE_HATCH_TEX_COUNT];
static uint16_t tex_clut[THE_HATCH_TEX_COUNT];

/* ---- WHY `hatch` IS STREAMED AND NOT REGISTERED ----------------------------
   texmgr_register() would keep the whole 16.5 KB TIM resident for the life of
   the run, and the heap has no room for it. tools/heap_budget.py reports 273 KB
   free at rest before this room existed; the measured boot cliff is between 233
   and 235 KB and the note there says to treat anything under 256 KB as already
   in trouble, because the figure is a RESTING total and the startup PEAK is what
   collides with the stack. An 18 KB registration plus this room's own BSS would
   have spent most of that margin on a texture thirteen polys use.

   So it is read on entry into a scratch buffer that is freed again — the
   Greenhouse's pattern, which the Chain Room also uses for its pipe. It costs a
   little loading time and no permanent bytes at all, and it is the right answer
   for a room at the end of a branch that nothing else reaches
   (tools/ADDING_A_ROOM.txt STEP 3f).

   Keep this in step with the slot numbering above and with NAME_TO_SLOT in
   gen_the_hatch_tex_map.py. */
#define THE_HATCH_STREAM_TEX 1
static const char *stream_tex_file[THE_HATCH_STREAM_TEX] = {
    "\\TEX\\HATCH.TIM;1",      /* slot 6 -> x640 y0 */
};

/* HATCH is 8bpp 128x128, 16928 bytes = 9 sectors. 10 gives a sector of slack. */
#define TH_TEX_SCRATCH  (10 * 2048)

/* ---- Cull keys -------------------------------------------------------------
   Maze Two's scheme verbatim, and carried over rather than reasoned about again:
   the cull key is lifted out into its own array, built once at load, holding the
   first vertex's X and Z plus the primitive's stride so the walk can advance
   without reading the header at all.

   Both tables live in the shared cull arena rather than in this room's own BSS:
   only one room's are ever live, for exactly the reason only one room's MESH is
   (src/cull_arena.h). The names below are this file's own view of the arena, so
   everything under them reads as it always did. At 661 primitives this room is
   well inside the arena's 2056, which Maze One sizes. */
#define ThCullKey CullKey
#define th_keys     cull_keys
static int       th_key_count = 0;

static void th_build_cull_keys(void) {
    th_key_count = 0;
    if (!the_hatch_smd) return;
    uint8_t *p = (uint8_t *)the_hatch_smd->p_prims;
    int i, n = the_hatch_smd->n_prims;
    if (n > THE_HATCH_PRIM_COUNT) n = THE_HATCH_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &the_hatch_smd->p_verts[vi[0]];
        th_keys[i].x      = v0->vx;
        th_keys[i].z      = v0->vz;
        th_keys[i].stride = pt->len;
        th_keys[i].pad    = 0;
        p += pt->len;
    }
    th_key_count = n;
}

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. At 37 KB this is a third of
   Maze One's 118 KB and it sizes nothing. */
void the_hatch_load_geometry(void) {
    the_hatch_buff = room_arena_load("\\TEX\\THEHATCH.SMD;1");
    the_hatch_smd  = the_hatch_buff ? smdInitData(the_hatch_buff) : NULL;
    th_build_cull_keys();

    /* THE PIT'S TWO DOORS, read HERE and not at startup. They are a prop in one
       room at the end of one branch, and 12 KB of sector-rounded .smd and .pva
       is more than the heap has to give permanently - see the memory note in
       hatch_doors.h. main.c's load_area_geometry() frees them again on the way
       into any other room, and this call is safe to make twice. The CD access is
       legal for the same reason the room_arena_load above it is: STATE_LOADING
       is the one point in the frame where blocking reads are allowed. */
    hatch_doors_load();
}

/* Startup. NO CD ACCESS AT ALL, and no texmgr registration: every slot here is a
   pair of compile-time constants out of tim_slots.h. Six of the seven are
   uploaded by another module; the seventh is streamed on entry by
   the_hatch_upload_textures below, and the tpage/clut a stream would compute are
   the same constants TIM_SLOT puts here, so they are not captured at upload time
   either. Geometry moved to the_hatch_load_geometry above. */
void the_hatch_load_assets(void) {
    /* Uploaded by another module; header only — no LoadImage, no RAM copy. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRSSGS);
    TIM_SLOT(3, BRIKWLL);
    TIM_SLOT(4, PLINTH);      /* Maze Two's, at x832 y0 */
    TIM_SLOT(5, CHAIN);       /* Maze One's, at x704 y0 where gravel_gs was */

    /* Streamed on entry; the header is still a compile-time constant. */
    TIM_SLOT(6, HATCH);
}

/* Upload this room's textures on entry.

   ORDER MATTERS. The courtyard's uploader (via the Garden Stairs') puts
   gravel_gs on x704 y0 and brick_wall on x768 y0, and it is also what stamps
   chnlnk on x640 y0 — so the chain, which takes the gravel_gs slot, and the
   hatch, which takes the chnlnk one, must both FOLLOW it and never precede it.
   brick_wall is the one page here the courtyard's own upload is wanted for, so
   nothing may land on x768 afterwards; that is exactly why the plinth and the
   chain come through NARROW accessors rather than through Maze Two's and Maze
   One's full uploaders, either of which would have dropped a pipe there.

   THE CD READ. Like the Greenhouse's uploader and the Chain Room's, this one
   touches the drive, because HATCH.TIM is resident nowhere. The cdaudio bracket
   is mandatory rather than defensive: a data read issued while CD-DA streams
   hangs the drive. suspend/resume are no-ops when nothing is playing, and the
   gate trigger into here stops the music first — but a title-screen load or a
   debug level-select jump can arrive with a track running, and that is the case
   the bracket is for. LoadImage itself is safe here because main's STATE_LOADING
   has already done a DrawSync(0). */
static void th_stream_tim(const char *filename, uint8_t *buf, int bufcap) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return;
    int sectors = (file.size + 2047) / 2048;
    if (sectors * 2048 > bufcap) return;   /* too big for the scratch buffer */
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    /* Pixels and CLUT both. The tpage/clut this would produce are the same
       constants TIM_SLOT already put in tex_tpage/tex_clut, so they are not
       captured here. */
    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
    }
}

void the_hatch_upload_textures(void) {
    garden_courtyard_upload_textures();   /* hedge, grdn_gte, grss_gs, and
                                             brick_wall via the Stairs'        */
    maze_two_upload_plinth();             /* plinth -> the opn_drwr slot, x832  */
    maze_one_upload_chain();              /* chain  -> the gravel_gs slot, x704 */

    uint8_t *scratch = malloc(TH_TEX_SCRATCH);
    if (!scratch) return;                 /* draw with the courtyard's art rather
                                             than crash — 20 KB is always there
                                             at the point a room is entered    */
    cdaudio_suspend();
    for (int i = 0; i < THE_HATCH_STREAM_TEX; i++)
        th_stream_tim(stream_tex_file[i], scratch, TH_TEX_SCRATCH);
    cdaudio_resume();
    free(scratch);                        /* hatch -> x640 y0 */
}

/* ---- The WEST-wall gate, back into the Keystone Maze ------------------------
   The grdn_gte leaf at x=-200 spanning z[-300,300], y[-600,0], in the YZ plane.
   Its neck is collision FLOOR 0, x(-200,1800) z(-300,300), which is what fixes
   the centre at z=0.

   Collision wall 0 runs across the opening with nx = +4096, so the walkable side
   is +X and the player approaches from inside this room heading WEST. For a YZ
   sign that is mirror=0, and it stands 11 units EAST of the wall (x + 11) — the
   Keystone Maze's own west gate exactly, and the OPPOSITE hand from that room's
   side of THIS gate, whose wall 28 faces -X. Get either backwards and the text
   comes out reversed or buried in the hedge.

   The wall STAYS in the collision list, as at every other gate in the garden:
   the leaf is shut as far as collision is concerned and it is the trigger, not a
   hole, that lets the player through.

   NO LOCK. The Chain Room's two gates and the Keystone Maze's north gate are
   held by the Valve Puzzle; this one is not, and neither is the Keystone Maze's
   east gate that leads here. A player who has reached the Keystone Maze at all
   has already turned Maze Two's valve, so a second check on the same bit would
   only ever read true. */
#define TH_GATE_X          (-200)
#define TH_GATE_Z              0    /* (-300 + 300) / 2, and FLOOR 0's centre  */
#define TH_TEXT_Y         (-186)    /* eye level on the y=0 ground             */
#define TH_TEXT_RADIUS      1500
#define TH_FADE_NEAR        1000
#define TH_TRIGGER_RADIUS    500

/* Standing eye on the ground: floor y=0, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. The same expression the rest of the
   garden uses, and for the same reason — this whole run of rooms has its floor
   at y=0 while the courtyard below is at positive y. */
#define TH_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by the_hatch_gate_arm(). Starts "held" so a press
   carried in through the transition doesn't bounce the player straight back. */
static int gate_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void the_hatch_gate_arm(void) {
    gate_circle_prev = circle_held();
}

int the_hatch_gate_triggered(void) {
    int held = circle_held();
    int just = held && !gate_circle_prev;
    gate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - TH_GATE_X;
    int32_t dz = cam_z - TH_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < TH_TRIGGER_RADIUS && interact_facing(TH_GATE_X, TH_GATE_Z);
}

/* Floating "Press O to enter" sign on the west gate. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. Sits just east (x+11) of the wall so it floats in front
   of the gate on the side the player is actually on. */
static void gate_text(RenderContext *ctx) {
    int32_t dx = cam_x - TH_GATE_X;
    int32_t dz = cam_z - TH_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= TH_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > TH_FADE_NEAR) {
        int range = TH_TEXT_RADIUS - TH_FADE_NEAR;
        int prog  = xz - TH_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        TH_GATE_X + 11, TH_TEXT_Y, TH_GATE_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving from the Keystone Maze: stand EAST of the x=-200 gate, clear of the
   195 wall push radius (so the player isn't shoved on their first frame), facing
   +X — the direction of travel through the gate, looking straight down the neck
   at the yard and the pit beyond it. x lands at 20, z at 0, inside collision
   FLOOR 0's x(-200,1800) z(-300,300) run so the floor is under them immediately,
   and 300 clear of the neck's own side walls (1 at z=-300 and 2 at z=300).

   It arms the gate, which is the room's only interaction: a Circle held through
   the transition would otherwise fire the trigger the arrival is standing in
   front of and bounce the player straight back out. */
void the_hatch_spawn_west(void) {
    cam_x   = TH_GATE_X + COLLISION_WALL_RADIUS + 25;
    cam_y   = TH_EYE_Y;
    cam_vy  = 0;
    cam_z   = TH_GATE_Z;
    cam_rot = 1024;   /* facing +X, into the room */
    the_hatch_gate_arm();
}

void the_hatch_init(void) {
    the_hatch_collision_init(&current_collision_room);
    /* The perimeter hedge is DRAWN to y=-500 (the gate reaches -600, but that is
       the opening, not the roofline), and every collision run agrees at 500. The
       hatch lid reaches -639 and is the tallest thing in the room, but it stands
       over a footprint the player is fenced out of, so it is not the ceiling
       over any walkable ground — see the_hatch.h and tools/ADDING_A_ROOM.txt on
       visual-vs-collision heights. */
    collision_set_ceiling_y(-500);
    /* No collision_set_wall_radius: the default 195 is right here, as in all
       three mazes and the Chain Room. The tightest spaces are the two 600-wide
       necks — the west gate's and the passage into the north chamber — which at
       195 leave 210 units of walkable width, the same margin the mazes'
       corridors give. main.c resets to the default before every room init, so
       saying nothing is enough. */

    the_hatch_floor_zones_init();

    /* One gate, so this is not merely a default: it is the only arrival. */
    the_hatch_spawn_west();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room spans x[-200,4800] z[-1499,3900], which contains reception's save
       point at (78,-67), so they certainly do. Clearing is safe: reception_init()
       re-places both on every reception entry. No save point of this room's own;
       the nearest is on the Garden Stairs' top landing. */
    save_points_clear();
    dressers_clear();

    /* AFTER the flag restore, which savegame_apply_pending has already done by
       the time a room init runs: this poses the pair shut or open to match
       FLAG_HATCH_DOORS_OPEN and arms their Circle edge state, so a press held
       through the gate transition does not throw them open on the arrival
       frame. */
    hatch_doors_init();
}

static void draw_the_hatch_smd(RenderContext *ctx) {
    if (!the_hatch_smd) return;


    uint8_t *p = (uint8_t *)the_hatch_smd->p_prims;
    int i, n = the_hatch_smd->n_prims;

    /* Hoisted: all constant for the whole frame, and every one of them would
       otherwise be recomputed per primitive inside the hottest loop in the room
       — the two trig lookups once for each poly that passed the distance cull,
       the cull distance and the debug test all 661 times. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = TH_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    if (th_key_count < n) n = th_key_count;   /* keys not built: draw nothing */

    for (i = 0; i < n; i++) {
        /* ---- The reject path. ONE sequential read, no mesh access ----------
           th_keys carries this primitive's first vertex and its stride, so a
           primitive outside the view distance is skipped without touching the
           header or the vertex array at all. See th_build_cull_keys. */
        uint8_t stride = th_keys[i].stride;
        int32_t kdx = (int32_t)th_keys[i].x - cam_x;
        int32_t kdz = (int32_t)th_keys[i].z - cam_z;
        if ((kdx < 0 ? -kdx : kdx) + (kdz < 0 ? -kdz : kdz) > cull)
            { p += stride; continue; }

        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &the_hatch_smd->p_verts[vi[0]];
        SVECTOR *v1 = &the_hatch_smd->p_verts[vi[1]];
        SVECTOR *v2 = &the_hatch_smd->p_verts[vi[2]];

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

               NOT measured on this mesh, unlike Maze One's and Maze Two's,
               and it does not need to be: at 661 primitives over 5000 x 5400
               units it is the frustum test that does nearly all of the work
               here — the far corners are 8000 Manhattan apart, so the 2500
               distance cull does fire, but only for the far end of the yard
               seen from the chamber, and the four hedged sub-spaces mean most
               of what survives it is off to one side. Both cost a handful of
               multiplies each. The test is
               exact by construction — a poly is dropped only when EVERY vertex
               is outside the same plane — so it cannot introduce a hole whatever
               the mesh, which is why it is safe to carry over unmeasured.

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
                        SVECTOR *vk = &the_hatch_smd->p_verts[vi[k]];
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
           build time in the_hatch_nocull — same scheme as the other rooms. This
           mesh happens to contain none (the generator reports 0), but the table
           is generated and read the same way regardless. */
        int nocull = (i < THE_HATCH_PRIM_COUNT) && the_hatch_nocull[i];
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
            v3 = &the_hatch_smd->p_verts[vi[3]];
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
        int32_t fog = dist < TH_FOG_NEAR ? TH_FOG_NEAR : (dist > TH_FOG_FAR ? TH_FOG_FAR : dist);
        int32_t fog_factor = ((TH_FOG_FAR - fog) << 8) / (TH_FOG_FAR - TH_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in the_hatch_draw. */
        uint8_t tex_idx = (i < THE_HATCH_PRIM_COUNT) ? the_hatch_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < THE_HATCH_TEX_COUNT);
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
void the_hatch_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = TH_FOG_NEAR; g_fog_far = TH_FOG_FAR;

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
       page. All seven of this room's textures sit at page-top (Voff 0), so one
       window serves them (see tools/VRAM_MAP.txt) — including grss_gs and the
       chain, which is half the reason those two were placed where they were. */
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

    if (exp != DBG_EXP_NO_MESH) draw_the_hatch_smd(ctx);

    /* Every sprite enemy renderer is handed this room's texture window, because
       all of their sprites live at Voff >= 128 and must bracket it rather than
       sample the hedge (see tools/TEXTURING_NOTES.txt PART 5). NOTHING is seeded
       into this room today — world_seed_room() places no enemy and no pickup
       here — so every one of these calls runs over an empty array and costs
       nothing. They are here so that the first thing ever placed on the lawn
       draws correctly without anyone having to remember this paragraph.

       This room is on SND_BANK_GARDEN (main.c's STATE_LOADING), so anything put
       here reaches SFX_HISS and the flowers' loops; check any placement against
       src/sound.h the way Maze One's was. */
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

    /* THE PIT'S DOORS, after the room mesh and before the signs. They take the
       128 texture window set above (their UVs run past 128 like the mesh's) and
       the g_fog_near/g_fog_far this function has already set, and they restore
       the plain view matrix on the way out so the two signs below still project
       in world space. */
    hatch_doors_draw(ctx);

    /* Last: the two signs. The doors' prompt takes itself off the moment they
       start moving. */
    hatch_doors_text(ctx);
    gate_text(ctx);
}
