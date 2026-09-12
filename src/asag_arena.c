#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "camera.h"
#include "collision.h"
#include "asag_arena.h"
#include "asag_arena_mesh_collision.h"
#include "asag_arena_tex_map.h"
#include "asag.h"            /* the boss body: loaded per-room, drawn here */
#include "asag_boss.h"       /* the DEMO director: cycles the six clips     */
#include "cdaudio.h"          /* suspend/resume around the entry-time read */
#include "dresser.h"
#include "save_point.h"

/* ASAG'S ARENA. See asag_arena.h for the whole rationale — this file is the
   mechanism, that one is the argument. */

static SMD  *asag_arena_smd  = NULL;
static void *asag_arena_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE REAL MESH'S NUMBERS. The arena is 3000 wide (x[-1500,1500]) by 2900 deep
   (z[0,2900]), so its far corner is 5900 Manhattan from the near one — the
   distance the cull and the fog both have to reach, because a boss fight where
   the far wall fades out is a guessing game and not a fight. The 7000 below is
   deliberately longer than that: the BOSS reaches out to z=3931, a thousand
   units behind the arena's own back wall, and a cull tight to the room would
   clip the far end of it off.

   >>> THESE ARE SET FOR VISIBILITY, NOT FOR SPEED. <<< 522 primitives is a
   quarter of Maze One's 2056 and the whole room is inside the cull from
   anywhere in it, so nothing is being rejected by distance today — the frustum
   test in the draw below is doing all the work. If the finished room is heavy,
   measure U/D/G per tools/DIAGNOSING_FRAME_RATE.txt before shortening either.
   The fog only starts at 2500 so that the near half of the arena reads at the
   mesh's own vertex colours and the depth cue lands on the far wall. */
#define AA_CULL_DIST      ASAG_CULL_DIST
#define AA_FOG_NEAR       2500
#define AA_FOG_FAR        ASAG_CULL_DIST

/* The arena is UNDERGROUND — the player fell 1200 units to get here — so it
   does NOT take the garden's purple sky. Near-black is both the honest colour
   for a pit and the one that makes additive light (which is what a boss reveal
   is made of, see tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 7) read at full
   strength: additive geometry adds to whatever is behind it, so a dark ground
   is what buys the contrast. Not pure black — a fog that saturates to 0,0,0
   makes the cull line invisible, which sounds good and is actually how you lose
   an hour to "the far wall is missing". */
#define AA_CLEAR_R  ASAG_FOG_R
#define AA_CLEAR_G  ASAG_FOG_G
#define AA_CLEAR_B  ASAG_FOG_B

/* ---- Floor zones -----------------------------------------------------------
   THREE zones, and they are the three floor planes
   tools/gen_asag_arena_collision.py read out of the proxy mesh, verbatim:

       FLOOR 0   y=0   x[-1300,1300]  z[2613,2700]  the SOUTH recess, under Asag
       FLOOR 1   y=0   x[-1300,1300]  z[   0, 186]  the NORTH recess, where the
                                                    player lands off the shaft
       FLOOR 2   y=0   x[-1500,1500]  z[ 186,2613]  the arena floor itself

   ALL THREE ARE AT y=0, so this is one walkable height and multi_level stays 0
   in the collision file. They are three zones rather than one box because both
   recesses are NARROWER than the middle: a single zone spanning z[0,2700] at
   full width would hand the player a floor in the solid rock either side of
   each recess, and apply_height() would hold them up there if collision ever
   let them past a wall.

   >>> IF THE MESH EVER GAINS A TERRACE, A STEP OR A PIT, ADD ONE ZONE PER
   LEVEL <<< and set multi_level in the generator with it — and if it gains a
   low lip, collision_shoot_over_short_walls() in asag_arena_init() too, or
   every projectile in the fight dies on it. See the head of
   src/asag_arena_mesh_collision.c. */
static void asag_arena_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -1300; floor_zones[0].max_x = 1300;
    floor_zones[0].min_z =  2613; floor_zones[0].max_z = 2700;
    floor_zones[0].y     = 0;

    floor_zones[1].type  = FLOOR_FLAT;
    floor_zones[1].min_x = -1300; floor_zones[1].max_x = 1300;
    floor_zones[1].min_z =     0; floor_zones[1].max_z =  186;
    floor_zones[1].y     = 0;

    floor_zones[2].type  = FLOOR_FLAT;
    floor_zones[2].min_x = -1500; floor_zones[2].max_x = 1500;
    floor_zones[2].min_z =   186; floor_zones[2].max_z = 2613;
    floor_zones[2].y     = 0;

    floor_zone_count = 3;
}

/* ---- Per-room textures -----------------------------------------------------
   EMPTY, AND THE MACHINERY AROUND IT IS NOT. Add a texture by putting its TIM
   in textures/, adding a <file> line to disc.xml's TEX directory, and adding
   one row below. Nothing else — no generator run, no header regeneration, no
   texmgr registration, no startup work. The slot index is the row's position in
   this table and is what the draw loop's per-poly tex map will index.

   >>> PICK THE VRAM RECTANGLE OUT OF tools/VRAM_MAP_ASAG.txt, NOT OUT OF
   tools/VRAM_MAP.txt. <<< The whole-disc map shows a sheet with almost nothing
   free on it. This room's map shows the same sheet with everything the room
   cannot see marked reclaimable, which is nearly all of it. Taking a
   reclaimable slot costs NOTHING here: the player cannot leave except through a
   transition, and every transition re-uploads the destination room's art.

   THE COST OF A ROW: its VRAM rectangle, its bytes on the disc, and ~9 sectors
   of read on the one loading screen that reaches this room. NOT one byte of
   main RAM — which is the constraint that actually binds. See the header. */
#define ASAG_ARENA_STREAM_TEX 3
static const char *stream_tex_file[] = {
    "\\TEXASAG\\ASGMUD.TIM;1",   /* 0 ASAG_TEX_MUD    x384 y0  8bpp  the floor */
    "\\TEXASAG\\ASGWALL.TIM;1",  /* 1 ASAG_TEX_WALL   x512 y0  8bpp  "Boss Wall" */
    /* --- Asag's own skin. Streamed here, not registered by src/asag.c; the
       argument is in the header beside the ASAG_TEX_* slot numbers. --- */
    "\\TEXASAG\\ASGSKIN.TIM;1",  /* 2 ASAG_TEX_SKIN   x640 y0  8bpp  the body  */
};

static uint16_t tex_tpage[ASAG_ARENA_TEX_COUNT];
static uint16_t tex_clut[ASAG_ARENA_TEX_COUNT];

/* The boils' two pieces of state. Declared up here rather than beside the code
   that uses them because asag_arena_init() — which zeroes both on arrival — is
   above that code, and the block comment on the mechanism belongs with the
   mechanism. See boil_lit() further down, and the header. */
static int32_t boil_level;      /* 0..ASAG_BOIL_LEVEL_MAX, the director's     */
static int32_t boil_clock;      /* game frames; ticked by the draw            */

/* src/asag.c reads the boss's skin back through these. Zero for a slot that
   never streamed, which draws the body in whatever art sits at tpage 0 - ugly,
   and better than a crash on a bad CD read. */
uint16_t asag_arena_tex_page(int slot) {
    return (slot >= 0 && slot < ASAG_ARENA_TEX_COUNT) ? tex_tpage[slot] : 0;
}
uint16_t asag_arena_tex_clut(int slot) {
    return (slot >= 0 && slot < ASAG_ARENA_TEX_COUNT) ? tex_clut[slot] : 0;
}

/* Scratch for the entry-time stream. Nine sectors covers the largest thing a
   TIM can be here — an 8bpp 128x128 plus its 256-word CLUT is 16,928 bytes —
   and ten gives a sector of slack, which is what the Greenhouse allocates for
   the same reason. ONE buffer serves every texture in turn and is freed again,
   so this is 20 KB of transient heap on a loading screen and nothing at rest. */
#define AA_TEX_SCRATCH  (10 * 2048)

/* ---- Standing eye height ---------------------------------------------------
   Floor y=0, less GROUND_FLOOR_Y and the 40-unit standoff apply_height applies.
   The same expression the whole garden chain uses. */
#define AA_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* ---- The shaft mouth -------------------------------------------------------
   ON THE MESH, at the NORTH end (low z), which is the recess the proxy cuts at
   x[-1300,1300] z[0,186]. 300 puts the player just clear of it and looking down
   the length of the arena at Asag.

   300 rather than something tighter because 220 is the minimum that clears the
   default COLLISION_WALL_RADIUS of 195 plus the arrival margin, and the extra
   80 leaves the first frame of free play clear of the push-out boundary.
   Handing control back ON the boundary makes that frame a shove, which is the
   trap tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 2E is about.

   >>> AND THERE IS NO EXIT TO GO WITH IT. <<< See the header: the way out of
   this room has not been decided, so rather than stand a prompt somewhere that
   offers to climb an unclimbable shaft, there is no prompt, no trigger and no
   destination at all. */
#define AA_SHAFT_X            0
#define AA_SHAFT_Z          300

/* ---- Geometry --------------------------------------------------------------
   Read on ENTRY into the shared arena, not at startup — the invariant every
   room in this game keeps (src/room_arena.h). A MISSING FILE IS NOT AN ERROR
   HERE: room_arena_load returns NULL, asag_arena_smd stays NULL, and the room
   simply draws nothing but its clear colour and the exit sign.

   ASAGARNA.SMD is 29 KB against the arena's 118 KB (Maze One still sets that
   size), so it fits with room to spare — but re-run tools/gen_room_arena.py if
   the mesh is ever re-exported much larger. A mesh bigger than the arena is
   REFUSED at load time and the room draws empty, which looks exactly like a
   missing file and will waste an afternoon. */
void asag_arena_load_geometry(void) {
    asag_arena_buff = room_arena_load("\\TEXASAG\\ASAGARNA.SMD;1");
    asag_arena_smd  = asag_arena_buff ? smdInitData(asag_arena_buff) : NULL;
}

/* Startup. NOTHING. No CD access, no LoadImage, no RAM copy, no texmgr
   registration — the shape west_corridor.c and garden_courtyard.c already have,
   and the reason this room costs zero permanent bytes in textures.

   It exists rather than being deleted because main() calls one *_load_assets()
   per room and a room that is missing from that list is the sort of thing that
   is noticed three features later. When the textures land, this stays empty:
   their tpage/clut are captured by the streamer, not baked here (see the
   header's note on why this room does not use TIM_SLOT). */
void asag_arena_load_assets(void) {
}

/* ---- The entry-time texture stream -----------------------------------------
   THE CD READ. Like the Greenhouse's and the Chain Room's uploaders and unlike
   every other room's, this one touches the drive. The cdaudio bracket is
   MANDATORY, not defensive: a data read issued while CD-DA streams hangs the
   drive (tools/TEXTURE_STREAMING_DEBUG.txt). suspend/resume are no-ops when
   nothing is playing, and the drop into here stops the music first — but a
   debug level-select jump can arrive with a track running, and that is the case
   the bracket is for. LoadImage itself is safe because main's STATE_LOADING has
   already done a DrawSync(0).

   Unlike those two, the tpage and clut are CAPTURED FROM THE TIM rather than
   taken from src/tim_slots.h, so a texture can be added to the table above
   without regenerating a generated header. See the header. */
static void aa_stream_tim(const char *filename, int slot,
                          uint8_t *buf, int bufcap) {
    CdlFILE file;
    if (!filename || slot < 0 || slot >= ASAG_ARENA_TEX_COUNT) return;
    if (!CdSearchFile(&file, (char *)filename)) return;
    int sectors = (file.size + 2047) / 2048;
    if (sectors * 2048 > bufcap) return;   /* too big for the scratch buffer */
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0,
                               tim.prect->x, tim.prect->y);
    tex_clut[slot]  = 0;
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        tex_clut[slot] = getClut(tim.crect->x, tim.crect->y);
    }
}

void asag_arena_upload_textures(void) {
    /* >>> THIS ROOM BORROWS NOTHING AND RESTORES NOTHING. <<< Every other garden
       room opens its uploader by calling a neighbour's, because they share art
       across gates the player walks back and forth through. Nothing is shared
       here: there is one way in, it is a drop, and the room the player fell out
       of is not visible from anywhere down here. That is the whole reason
       tools/VRAM_MAP_ASAG.txt can call so much of VRAM reclaimable — and it is
       also why NOTHING NEEDS PUTTING BACK on the way out: whatever exit this
       room eventually grows will be a transition like any other, and the
       destination room's own uploader runs on the far side of it. Today there
       is no exit at all, which makes the point moot rather than wrong. */
    if (ASAG_ARENA_STREAM_TEX == 0) return;   /* no art yet; nothing to read */

    uint8_t *scratch = malloc(AA_TEX_SCRATCH);
    if (!scratch) return;                 /* draw untextured rather than crash —
                                             20 KB is always there at the point
                                             a room is entered               */
    cdaudio_suspend();
    for (int i = 0; i < ASAG_ARENA_STREAM_TEX; i++)
        aa_stream_tim(stream_tex_file[i], i, scratch, AA_TEX_SCRATCH);
    cdaudio_resume();
    free(scratch);
}

/* ---- The mud, on its own, for the arriving transition ----------------------
   See the header. door_anim.c's DOOR_PANEL_FALL draws four mud quads rushing at
   the camera, and it draws them BEFORE main.c's STATE_LOADING has run — so the
   arena's own upload above has not happened and VRAM still holds The Hatch's
   art. This is that one row of the table, early.

   >>> THE DrawSync IS MINE TO DO HERE, WHICH IT IS NOT ABOVE. <<< LoadImage
   collides with an async DrawOTagEnv still in flight and hard-crashes after a
   few calls — the whole of PROBLEM A in tools/TEXTURING_NOTES.txt. The uploader
   above is safe because STATE_LOADING has already idled the GPU for it; this one
   runs from a door trigger inside update_current_area(), with the PREVIOUS
   frame's draw very much in flight, so it has to idle the GPU itself. Two
   LoadImage calls behind one DrawSync is exactly the shape STATE_LOADING uses.

   The cdaudio bracket is for the same reason the uploader's is: a data read
   issued while CD-DA streams hangs the drive. The drop stops the music a line
   later in main.c, so this bracket is doing real work rather than covering an
   edge case. */
void asag_arena_upload_mud(void) {
    uint8_t *scratch = malloc(AA_TEX_SCRATCH);
    if (!scratch) return;    /* the panel then draws in whatever is at tpage 0 —
                                ugly for five seconds, and not a crash        */
    DrawSync(0);
    cdaudio_suspend();
    aa_stream_tim(stream_tex_file[ASAG_TEX_MUD], ASAG_TEX_MUD,
                  scratch, AA_TEX_SCRATCH);
    cdaudio_resume();
    free(scratch);
}

/* ---- Arrival ---------------------------------------------------------------
   The one way in: dropped down the shaft. Faces +Z, straight down the length of
   the arena at Asag — which is the whole of the staging this room does, since
   there is no reveal camera yet and no exit to put the boss in front of. */
void asag_arena_spawn_shaft(void) {
    cam_x   = AA_SHAFT_X;
    cam_y   = AA_EYE_Y;
    cam_vy  = 0;
    cam_z   = AA_SHAFT_Z;
    cam_rot = 0;      /* facing +Z */
}

void asag_arena_init(void) {
    asag_arena_collision_init(&current_collision_room);

    /* READ OFF THE MESH, not off the collision proxy. The arena's perimeter
       walls top out at y=-1000 and that is the roofline over the walkable
       ground; the mesh does reach y=-1534 in places, but a ceiling probe wants
       the height above where the player STANDS and not the tallest thing in the
       room (the "visual vs collision heights" rule in tools/ADDING_A_ROOM.txt).
       Anything hung higher than this — and a boss reveal camera very likely is
       — takes its own literal. Note the BOSS is above this line too, at
       y[-960,-528], which is fine: a ceiling is a limit on the camera, not a
       claim about what can be drawn above it. */
    collision_set_ceiling_y(-1000);

    /* No collision_set_wall_radius: the default 195 is right for an open square
       with no necks in it. main.c resets to the default before every room init,
       so saying nothing is enough.

       NOTHING SETS shoot_over_mask HERE, and that is only correct while the room
       is a flat box. See the head of asag_arena_mesh_collision.c: the moment the
       arena has a low lip in it, this is where
       collision_shoot_over_short_walls(<threshold>) goes, AFTER the
       *_collision_init() above, which zeroes it. */

    asag_arena_floor_zones_init();

    asag_arena_spawn_shaft();   /* the only arrival */

    /* Save points and dresser props are GLOBAL (not room-swapped) and neither is
       area-gated in its collide routine, so another room's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room spans x[-1500,1500] z[0,3700] in its own space, which contains
       plenty of them. Clearing is safe: every room that owns one re-places it on
       entry.

       >>> AND THE ABSENCE OF A SAVE POINT IS A DESIGN CONSTRAINT, NOT AN
       OVERSIGHT. <<< A sealed arena is only sealed if there is no other way out
       of the state. Placing one here would make "no re-entry handling needed"
       false, and the encounter would have to replay or skip its own reveal.
       tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 9. */
    save_points_clear();
    dressers_clear();

    /* The boss, back to visible and on its bind pose with its clock stopped.
       This is not the load: asags_load_model() runs in main.c's STATE_LOADING
       beside the Rabisu's, and this only resets the playback state, so it is
       safe on a debug jump that arrives before the read has happened. */
    asag_reset();

    /* The boils go DARK on every arrival, before the director touches them.
       The encounter ramps them up in its own phase, so a second arrival — a
       debug level-select jump, or a load — has to start from unlit or the
       lighting beat plays to boils that are already on. The clock is zeroed
       with it so the breath starts at the trough rather than wherever the last
       visit left it. */
    asag_arena_set_boil_glow(0);
    boil_clock = 0;

    /* ...and the director, from the top. It is reset here rather than only at
       load so that a second arrival replays the opening instead of picking up
       mid-fight. src/asag_boss.h. */
    asag_boss_reset();
}

/* ---- The mesh --------------------------------------------------------------
   chain_room.c's draw_chain_room_smd(), textured branches and all. The arena's
   522 primitives are FT4 over two textures - mud (401) and "Boss Wall" (121) -
   indexed per polygon by src/asag_arena_tex_map.h, which
   gen_asag_arena_tex_map.py writes by TEXTURE NAME rather than by the SMX's own
   index. That matters: the Blender exporter renumbers its texture list whenever
   the material set changes, and a raw-index map then silently shifts every
   texture past the one that moved.

   The tpage and clut come from tex_tpage/tex_clut, which the entry-time stream
   captured from each TIM - NOT from the values smxlink baked into the .smd. The
   .smd's are correct today and would stop being correct the moment a TIM moved
   in VRAM without a re-link.

   WHAT IS DELIBERATELY NOT HERE: the cull-key table. chain_room.c and the mazes
   precompute one cache line per primitive so the distance reject never touches
   the mesh; at 522 primitives inside a 5900-unit room NOTHING is ever rejected
   by distance here, so a key table would cost BSS to answer a question that is
   always "yes". The frustum test below is what does the work. Revisit if the
   mesh ever grows several-fold. */
/* ---- The boils -------------------------------------------------------------
   See the header for what they are and why they cost no geometry. This is the
   whole mechanism: a level the director sets, a clock this module ticks, and
   four lines in the draw loop below.

   >>> LIGHTING A TEXTURED POLY MEANS setRGB0 ABOVE 128. <<< 128 is neutral on
   the PS1's texture blend and 255 doubles, so the boils can be brightened
   in place with no additive pass, no second primitive and nothing to sort —
   which is the one way they are cheaper than the Rabisu's lawn lights, and it
   is only available because the art already put them in the wall.

   THE RED IS IN THE GAIN, NOT ONLY IN THE TEXTURE. The art is reddish already;
   pushing all three channels equally would just make it a brighter grey-red. R
   goes to ASAG_BOIL_R_GAIN and G/B to ASAG_BOIL_GB_GAIN, so the hue moves
   toward the red as it lights rather than washing out of it.

   AND IT IS APPLIED AFTER THE FOG, not before. The fog lerps toward the clear
   colour by distance; a boost applied first would be fogged back down again,
   and the back wall is 2500 out — most of the way to AA_FOG_NEAR — so the
   brightest the boils could ever get would be whatever survived the lerp. They
   are meant to be a light source, and a light source does not dim with the
   wall it is in.

   ---- THE PULSE -------------------------------------------------------------
   "They light up over two seconds, then pulse slowly." The ramp is the
   director's (it sets `level`); the pulse is here, on this module's own clock,
   so the director sets one number once and has nothing to tick.

   ONE CLOCK FOR BOTH BOILS, and that is a choice against the runbook's default.
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 7A says to offset a bank of lamps per
   cell or the whole bank strobes as one lamp — right for sixteen lawn lights,
   wrong here. These are two organs on one animal, symmetric either side of it,
   and breathing out of phase would read as two independent lights rather than
   as one thing breathing. Same reason the same runbook gives the shockwave ring
   a single clock.

   THE TROUGH IS NOT ZERO. A pulse to black reads as a fault; ASAG_BOIL_PULSE_LO
   keeps them lit at three-quarters, so what breathes is the intensity and not
   the existence. */
#define ASAG_BOIL_R_GAIN      255   /* fully lit: 128 is neutral, 255 is x2    */
#define ASAG_BOIL_GB_GAIN     168   /* G/B rise less, so the hue goes redder   */
#define ASAG_BOIL_PULSE_TICKS 210   /* 3.5 s a breath                          */
#define ASAG_BOIL_PULSE_LO    192   /* trough, in 256ths of `level`            */

/* boil_level and boil_clock are declared at the head of the file; see there. */

void asag_arena_set_boil_glow(int32_t level) {
    if (level < 0) level = 0;
    if (level > ASAG_BOIL_LEVEL_MAX) level = ASAG_BOIL_LEVEL_MAX;
    boil_level = level;
}

int32_t asag_arena_boil_glow(void) { return boil_level; }

/* `level` with the breath applied, 0..256. isin gives -4096..4096 over a full
   turn, so (isin + 4096) / 8192 is the 0..1 the lerp wants; done in 256ths to
   stay in integers. */
static int32_t boil_lit(void) {
    if (boil_level <= 0) return 0;
    int32_t phase = ((boil_clock * 4096) / ASAG_BOIL_PULSE_TICKS) & 4095;
    int32_t s     = (isin(phase) + 4096) / 32;      /* 0..256 */
    int32_t env   = ASAG_BOIL_PULSE_LO
                    + ((256 - ASAG_BOIL_PULSE_LO) * s) / 256;
    return (boil_level * env) / 256;
}

static void draw_asag_arena_smd(RenderContext *ctx) {
    if (!asag_arena_smd) return;

    /* THE PULSE CLOCK IS TICKED HERE, in the draw, which wants justifying. It
       is not a general licence: this room's draw runs exactly once per game
       frame, the clock drives nothing but a colour, and the alternative is an
       asag_arena_update() that exists solely to add 1 — one more call for
       main.c's two branches to keep in step, and a silently frozen pulse the
       day one of them forgets it. rbs_glow_clock is ticked in
       update_rabisus() because the Rabisu HAS an update; this room does not.
       It runs on under the inventory menu, which is harmless and arguably
       right: the boils do not hold their breath while the player reads. */
    boil_clock++;
    int32_t boil = boil_lit();

    uint8_t *p = (uint8_t *)asag_arena_smd->p_prims;
    int i, n = asag_arena_smd->n_prims;

    /* Hoisted out of the loop: constant for the whole frame, and every one of
       them would otherwise be recomputed 674 times. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = AA_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);
        int stride  = pt->len;   /* bytes; the walk advances by it */

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &asag_arena_smd->p_verts[vi[0]];
        SVECTOR *v1 = &asag_arena_smd->p_verts[vi[1]];
        SVECTOR *v2 = &asag_arena_smd->p_verts[vi[2]];

        int32_t kdx = (int32_t)v0->vx - cam_x;
        int32_t kdz = (int32_t)v0->vz - cam_z;
        if ((kdx < 0 ? -kdx : kdx) + (kdz < 0 ? -kdz : kdz) > cull)
            { p += stride; continue; }

        /* ---- SIDE-PLANE FRUSTUM CULL ---------------------------------------
           Maze One's test, carried over unchanged and load-bearing for the same
           reason: in a closed room the geometry within the cull radius is all
           AROUND the camera rather than behind it, so the behind-the-camera
           test rejects almost nothing on its own, and everything else would be
           transformed by the GTE and then queued as a primitive the GPU takes
           in and throws away.

           With gte_SetGeomScreen(256) on a 320-wide screen the half-field is
           160/256, so a point is outside the right plane when
               side > (5/8) * fwd,  i.e.  8*side > 5*fwd.
           Both sides are world units (the >>12 undoes isin/icos).

           >>> A POLY IS ONLY CULLED WHEN EVERY VERTEX IS OUTSIDE THE SAME
           PLANE. <<< Testing v0 alone would slice through polys that straddle
           the screen edge, and this room's walls are single large quads - that
           would open holes in them. The exact test is what makes it safe to
           carry the cull over to a new mesh unmeasured.

           Y is not tested: the camera stands on a flat y=0 floor under a
           1000-tall roofline, so nothing inside the side planes is ever outside
           the top or bottom ones. */
        {
            int32_t fwd = kdx * sn + kdz * cs;
            if (fwd < -(1200 << 12)) { p += stride; continue; }
            if (!no_frustum) {
                int32_t f0 = fwd >> 12;
                int32_t s0 = (kdx * cs - kdz * sn) >> 12;
                int     sign = 0;
                if      ( s0 * 8 > f0 * 5) sign =  1;
                else if (-s0 * 8 > f0 * 5) sign = -1;
                if (sign) {
                    int cnt = is_quad ? 4 : 3, k, all_out = 1;
                    for (k = 1; k < cnt; k++) {
                        SVECTOR *vk = &asag_arena_smd->p_verts[vi[k]];
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

        /* The GTE clamps screen coordinates to +/-1023 and a primitive with a
           corner past it comes out folded. Drop the whole thing. */
        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        /* Backface cull, honouring both the SMD's own per-primitive nocull flag
           and the generated table. asag_arena_nocull[] rescues degenerate
           "triangle-shaped" quads, whose fourth corner is collinear and whose
           winding the GTE therefore cannot judge. This mesh currently has ZERO
           of them, so the table is all zeroes - it costs 522 bytes of rodata to
           stay honest the next time the mesh is re-exported. */
        int no_cull = pt->nocull ||
                      (i < ASAG_ARENA_PRIM_COUNT && asag_arena_nocull[i]);
        if (!no_cull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
        if (is_quad) {
            v3 = &asag_arena_smd->p_verts[vi[3]];
            gte_ldv0(v3);
            gte_rtps();
            gte_stsxy(&sv[3]);
            gte_stsz(&sz[3]);
            if (sv[3].vx <= -1023 || sv[3].vx >= 1023 ||
                sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
            if (sz[3] == 0) { p += stride; continue; }
            gte_avsz4();
        } else {
            gte_avsz3();
        }

        gte_stotz(&otz);
        /* Horizontal polys sort by their farthest corner, not their average, so
           the floor stays behind whatever stands on it (see render.h). */
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        /* Fog toward the CLEAR COLOUR, not toward the garden's purple sky: this
           is a pit, it fades to near-black, and the clear is what the far wall
           has to meet or the cull line becomes visible. */
        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog = dist < AA_FOG_NEAR ? AA_FOG_NEAR
                                         : (dist > AA_FOG_FAR ? AA_FOG_FAR : dist);
        int32_t ff  = ((AA_FOG_FAR - fog) << 8) / (AA_FOG_FAR - AA_FOG_NEAR);
        uint8_t r = (uint8_t)(((int32_t)col[0] * ff + AA_CLEAR_R * (256 - ff)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * ff + AA_CLEAR_G * (256 - ff)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * ff + AA_CLEAR_B * (256 - ff)) >> 8);

        /* THE BOILS, lit AFTER the fog for the reason given where boil_lit() is
           defined: they are a light source, and a light source does not dim
           with the wall it sits in. Lerp from whatever the fog left toward the
           two gains, so level 0 is exactly the old colour and there is no seam
           at the moment the encounter starts the ramp. */
        if (boil && i < ASAG_ARENA_PRIM_COUNT && asag_arena_boil[i]) {
            r = (uint8_t)(r + ((ASAG_BOIL_R_GAIN  - r) * boil) / 256);
            g = (uint8_t)(g + ((ASAG_BOIL_GB_GAIN - g) * boil) / 256);
            b = (uint8_t)(b + ((ASAG_BOIL_GB_GAIN - b) * boil) / 256);
        }

        /* Per-prim texture index; SMD prim order matches the generated map.
           UVs come straight from the SMD primitive (offset 20+) and wrap via the
           128 texture window set in asag_arena_draw. */
        uint8_t tex_idx = (i < ASAG_ARENA_PRIM_COUNT) ? asag_arena_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < ASAG_ARENA_TEX_COUNT);

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

void asag_arena_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the room itself. */
    g_fog_near = AA_FOG_NEAR; g_fog_far = AA_FOG_FAR;

    /* Background in the colour the fog saturates to, painted by the HARDWARE
       CLEAR rather than a full-screen tile — the draw environments already clear
       the framebuffer before anything is drawn, so a tile on top is a second
       full-screen fill of the same 77k pixels every frame. */
    render_set_clear_colour(ctx, AA_CLEAR_R, AA_CLEAR_G, AA_CLEAR_B);

    /* 128x128 texture window so per-poly UVs tile within each texture's page.
       All THREE textures this room and this boss use are 128x128 at Voff 0 on a
       64-aligned x, so ONE window serves them and no primitive needs a bracket
       of its own (tools/TEXTURING_NOTES.txt PART 5). That is not luck - it is
       why the three are resampled to 128 in the first place, since the SMX's
       UVs are normalised to a 128-texel tile whatever the source art measured.
       The window is also what the sprite renderers and the weapon overlay are
       handed, so it must be set whether or not the mesh needs it - a window
       that was never established carries in from whatever ran last. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* >>> camera_build_view(), NOT THE HAND-ROLLED YAW-ONLY BLOCK. <<< This is
       mandatory here and not a preference. Most rooms build their view from
       {0,-cam_rot,0} through RotMatrix, which silently DISCARDS cam_pitch — and
       a crane shot through such a room is not a tilted camera, it is a high
       camera staring level at the far wall, with nothing to warn you. A room
       that hosts a boss reveal must use this, and so must the boss's own draw,
       which builds its own copy of the view before composing the model matrix.
       Miss the second one and the boss renders through a different projection
       than the room it stands in. tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 3. */
    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    int exp = DEBUG_EXPERIMENT();
    if (DEBUG_CULL_DIST()) g_fog_far = DEBUG_CULL_DIST();

    if (exp != DBG_EXP_NO_MESH) draw_asag_arena_smd(ctx);

    /* NO EXIT SIGN, because there is no exit - see the header. */

    /* THE BODY. Already in this room's coordinate space, so it draws under the
       view built above and loads no matrix of its own - see the long note in
       src/asag.h about why this boss needs no model matrix and the Rabisu does.
       It goes after the room so its primitives sort against the room's. */
    /* DBG_EXP_NO_ENTITIES (debug level 8) is the counterpart to level 4's
       DBG_EXP_NO_MESH: D read at 4 and at 8 splits the draw section between the
       room's 522 primitives and the boss's 79, in one sitting rather than one
       rebuild per hypothesis. tools/DIAGNOSING_FRAME_RATE.txt STEP 1 - add the
       switch BEFORE spending an afternoon on either half. */
    if (exp != DBG_EXP_NO_ENTITIES) asag_draw(ctx);

    /* THE OPENING SCENE'S SUBTITLES. Screen space, sorted into the
       menu-reserved OT range, so LAST of all — and a no-op outside the two
       speaking phases. The scene's other visual, the boils, needs no call of
       its own: they are polygons of this room's own mesh, brightened in the
       draw loop above.

       >>> THE REST OF THE ENCOUNTER'S DRAWS GO BETWEEN asag_draw AND THIS, IN
       THIS ORDER. <<< tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 10:
         asag_projectiles_draw(ctx);     wants the plain view matrix, which
                                         asag_draw above leaves untouched
         asag_boss_draw(ctx);            the lights: additive world geometry */
    asag_boss_draw_overlay(ctx);
}
