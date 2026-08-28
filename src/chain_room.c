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
#include "chain_room.h"
#include "collision.h"
#include "chain_room_mesh_collision.h"
#include "chain_room_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "cdaudio.h"             /* suspend/resume around the entry-time read */
#include "dresser.h"
#include "garden_courtyard.h"    /* garden_courtyard_upload_textures      */
#include "delivery_area.h"       /* delivery_upload_gravel                */
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
#include "valve_puzzle.h"      /* the lock on both of this room's gates */
#include "valve_handle.h"      /* the wheel on this room's standpipe */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Chain Room: the walled yard between Maze Two and the Keystone Maze. See
   chain_room.h for the layout. */

static SMD  *chain_room_smd  = NULL;
static void *chain_room_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   MAZE ONE'S EXACTLY, which is Fountain Square's exactly: 575/2500, the same
   purple SKY_FOG_* colour, and the cull equal to the fog-far so nothing is
   dropped until the fog has already faded it into the background. The whole
   garden from the courtyard's gate onwards is one continuous outdoors, and the
   step through a gate changes the plan of the place but not the weather.

   Here the distance cull never fires: the room is 2000 x 2000 and its far corner
   is 2400 Manhattan from the near one, so every one of the 243 primitives is
   inside 2500 from anywhere the player can stand. That is not a reason to
   shorten it — the fog would then visibly eat the far wall of a room the player
   can see across in one glance, which is the opposite of what it does in the
   mazes. The cull-key path costs one 6-byte read per primitive and stays for the
   frustum test that follows it, which does do work here. */
#define CR_CULL_DIST      2500
#define CR_FOG_NEAR        575
#define CR_FOG_FAR        2500

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, as both mazes. The collision generator found three floor planes and
   every one is at y=0 — the yard and the two gate necks — so this room has no
   step, ramp or storey anywhere in it. One rect over the collision bounds covers
   all three plus the corners between them, which are hedge and brick and which
   the player is kept out of by the 500-tall wall runs in the wall list. */
static void chain_room_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x =  -200; floor_zones[0].max_x = 1800;
    floor_zones[0].min_z = -1100; floor_zones[0].max_z =  900;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Seven mesh textures and the room owns NONE of them. Every one is a TIM that is
   already on the disc for another room, so this module holds no texture RAM at
   all: it takes the tpage/clut headers as compile-time constants and delegates
   the entry-time LoadImage to whichever module holds the RAM copy.

     0 hedge          the N/S perimeter        x384 y0    courtyard's uploader
     1 grdn_gte       the two gates            x512 y0    courtyard's uploader
     2 grss_gs        the border strips        x320 y0    courtyard's uploader
     3 gravel_texture the yard floor           x640 y0    delivery_upload_gravel
     4 brick_wall     the east wall (y=-700)   x768 y0    courtyard, via Stairs
     5 pipe_gh        the NE standpipe         x384 y256  STREAMED on entry
     6 chain          the four chains (y=-700) x704 y0    maze_one_upload_chain

   >>> ADDING A TIM TO THE DISC IS NOT FREE, AND IT IS NOT VRAM THAT RUNS OUT
   FIRST. <<< The \TEX\ directory holds ~130 files and its ISO9660 directory
   record was 112 bytes short of filling its fourth 2048-byte sector. This room
   first shipped with two new TIMs of its own — retargeted copies of the pipe and
   the chain — which pushed that record into a FIFTH sector, and the game then
   died on the loading screen inside sound_init, in the first CdSearchFile that
   switched from \TEX\ to \SND\. Nothing about the crash pointed at a directory:
   the heap had 200 KB spare and the VRAM map was clean. So this room takes NO
   new files, and the next room to want one must check the TEX record first —
   tools/check_disc_root.py now reports every directory's sector count.

   That constraint is what drives the two substitutions below, and they are the
   opposite hand from every other garden room's:

     'gravel_texture' -> ITS OWN PAGE at x640 y0, NOT the gravel_gs clone at
                         x704 y0 that Fountain Square, both mazes, the Keystone
                         Maze and the Rear Gate all use. That is deliberate: it
                         frees x704 for the chain below. GRAVEL.TIM lives in the
                         disc ROOT, not in \TEX\, so borrowing it costs no
                         directory bytes either; delivery_area.c holds the only
                         RAM copy and delivery_upload_gravel() is its narrow
                         accessor, added for this room on the
                         delivery_upload_brick_wall pattern.
     'grss'           -> the grss_gs CLONE at x320 y0, as everywhere else in the
                         garden. Forced rather than tidy here: grss.tim's own
                         home is x768 y0, which is this room's brick wall.

   With gravel moved, Maze One's own pipe and chain are BOTH reachable:

     chain    x704 y0, where gravel_gs would otherwise be. maze_one.c holds the
              RAM copy; maze_one_upload_chain() is its narrow accessor, added
              for this room alongside the existing maze_one_upload_pipe(). It is
              the 4x4-TILED chain_128.png, so the links keep the 32-texel period
              this mesh is UV'd for (TEXTURING_NOTES PART 7).
     pipe     NOT Maze One's PIPE.TIM, which is 8bpp at x768 y0 — that is the
              brick wall. The Greenhouse already made a 4bpp clone of it at
              x384 y256 for exactly this clash, PIPEGH.TIM, and this room draws
              nothing from that page. Nobody holds a RAM copy of it (the
              Greenhouse streams it), so this room streams it too — see
              chain_room_upload_textures.

   All three slots this room overwrites are already time-shared and every
   occupant is re-uploaded by its own room on entry, so none of this adds a
   restore obligation anyone has to write: x640 y0 goes back to chnlnk through
   the courtyard's uploader, x704 y0 to gravel_gs through the same, and
   x384 y256 to stove / prpl_wlppr through the kitchen's and piano room's.

   All seven sit at Voff 0, so the one 128 texture window set in chain_room_draw
   serves them all. */
#define CHAIN_ROOM_TEX_COUNT 7

static uint16_t tex_tpage[CHAIN_ROOM_TEX_COUNT];
static uint16_t tex_clut[CHAIN_ROOM_TEX_COUNT];

/* The one texture nobody holds in RAM: the Greenhouse's 4bpp pipe clone, which
   that room streams rather than registers, so there is no accessor to borrow.
   This room streams it the same way — greenhouse_upload_textures' pattern, into
   a scratch buffer that is freed again, costing no permanent bytes at all (see
   tools/ADDING_A_ROOM.txt STEP 3f).

   Keep this in step with the slot numbering above and with NAME_TO_SLOT in
   gen_chain_room_tex_map.py. */
#define CHAIN_ROOM_STREAM_TEX 1
static const char *stream_tex_file[CHAIN_ROOM_STREAM_TEX] = {
    "\\TEX\\PIPEGH.TIM;1",     /* slot 5 -> x384 y256 */
};

/* PIPEGH is 4bpp 128x128, 8256 bytes = 5 sectors. 6 gives a sector of slack. */
#define CR_TEX_SCRATCH  (6 * 2048)

/* ---- Cull keys -------------------------------------------------------------
   Maze Two's scheme verbatim, and carried over rather than reasoned about again:
   the cull key is lifted out into its own array, built once at load, holding the
   first vertex's X and Z plus the primitive's stride so the walk can advance
   without reading the header at all.

   It earns much less here than in a maze — at 243 primitives inside a 2500 cull
   the distance test rejects nothing at all — but the array is 6 bytes x 243 =
   1.5 KB of BSS and the loop below is then the mazes' line for line, which is
   worth more than the 1.5 KB. Indices match the draw loop's `i`. */
typedef struct { int16_t x, z; uint8_t stride, pad; } CrCullKey;
static CrCullKey cr_keys[CHAIN_ROOM_PRIM_COUNT];
static int       cr_key_count = 0;

static void cr_build_cull_keys(void) {
    cr_key_count = 0;
    if (!chain_room_smd) return;
    uint8_t *p = (uint8_t *)chain_room_smd->p_prims;
    int i, n = chain_room_smd->n_prims;
    if (n > CHAIN_ROOM_PRIM_COUNT) n = CHAIN_ROOM_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &chain_room_smd->p_verts[vi[0]];
        cr_keys[i].x      = v0->vx;
        cr_keys[i].z      = v0->vz;
        cr_keys[i].stride = pt->len;
        cr_keys[i].pad    = 0;
        p += pt->len;
    }
    cr_key_count = n;
}

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. At 14 KB this is the smallest
   room mesh on the disc and it sizes nothing. */
void chain_room_load_geometry(void) {
    chain_room_buff = room_arena_load("\\TEX\\CHNROOM.SMD;1");
    chain_room_smd  = chain_room_buff ? smdInitData(chain_room_buff) : NULL;
    cr_build_cull_keys();
}

/* Startup. NO CD ACCESS AT ALL, and no texmgr registration: every slot here is
   a pair of compile-time constants out of tim_slots.h. Five of the seven are
   uploaded by another module; the other two are streamed on entry by
   chain_room_upload_textures below, and the tpage/clut a stream would compute
   are the same constants TIM_SLOT puts here, so they are not captured at upload
   time either. Geometry moved to chain_room_load_geometry above. */
void chain_room_load_assets(void) {
    /* Uploaded by another module; header only — no LoadImage, no RAM copy. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRSSGS);
    TIM_SLOT(3, GRAVEL);      /* its OWN page at x640 y0, not the gravel_gs
                                 clone — see the note above */
    TIM_SLOT(4, BRIKWLL);
    TIM_SLOT(6, CHAIN);       /* Maze One's, at x704 y0 where gravel_gs was */

    /* Streamed on entry; the header is still a compile-time constant. */
    TIM_SLOT(5, PIPEGH);
}

/* Upload this room's textures on entry.

   ORDER MATTERS, AND MORE THAN USUAL HERE. The courtyard's uploader (via the
   Garden Stairs') puts chnlnk on x640 y0 and gravel_gs on x704 y0 — which are
   exactly where this room's gravel and chain go — so both of those must follow
   it, never precede it. Both are NARROW accessors rather than their owners' full
   uploaders, for the usual reason: delivery_restore_textures() would also stamp
   rusty_fence, brick_wall and double_door, and maze_one_upload_textures() would
   also drop the drain and the flower bed and its own 8bpp pipe on x768 y0,
   straight over the east wall.

   THE CD READ. Like the Greenhouse's uploader and unlike every other room's,
   this one touches the drive, because PIPEGH is resident nowhere. The
   cdaudio bracket is mandatory rather than defensive: a data read issued while
   CD-DA streams hangs the drive. suspend/resume are no-ops when nothing is
   playing, and both gate triggers into here stop the music first — but a
   title-screen load or a debug level-select jump can arrive with a track
   running, and that is the case the bracket is for. LoadImage itself is safe
   here because main's STATE_LOADING has already done a DrawSync(0). */
static void cr_stream_tim(const char *filename, uint8_t *buf, int bufcap) {
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

void chain_room_upload_textures(void) {
    garden_courtyard_upload_textures();   /* hedge, grdn_gte, grss_gs, and
                                             brick_wall via the Stairs'        */
    delivery_upload_gravel();             /* gravel -> the chnlnk slot, x640 y0 */
    maze_one_upload_chain();              /* chain  -> the gravel_gs slot, x704 */

    uint8_t *scratch = malloc(CR_TEX_SCRATCH);
    if (!scratch) return;                 /* draw with the courtyard's art rather
                                             than crash — 12 KB is always there
                                             at the point a room is entered    */
    cdaudio_suspend();
    for (int i = 0; i < CHAIN_ROOM_STREAM_TEX; i++)
        cr_stream_tim(stream_tex_file[i], scratch, CR_TEX_SCRATCH);
    cdaudio_resume();
    free(scratch);                        /* pipe -> x384 y256 */
}

/* ---- Shared gate machinery -------------------------------------------------
   Two gates, so the body of the trigger and of the sign is factored out and
   takes the gate's centre and its own edge state, as master_bedroom.c does with
   its pair of doors. Everything else about the two is identical: the same leaf
   in the same wall at the same height, the same radii, the same fade ramp. They
   differ only in the plane the sign is drawn in and in the HAND of it, which is
   what the two wrappers below carry. */
#define CR_TEXT_Y            (-186)   /* eye level on the y=0 yard */
#define CR_TEXT_RADIUS        1500
#define CR_FADE_NEAR          1000
#define CR_TRIGGER_RADIUS      500

/* Standing eye on the ground: floor y=0, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. The same expression Fountain Square, the
   Outside Catacombs and all three mazes use, and for the same reason — this
   whole run of garden rooms has its floor at y=0 while the courtyard below is at
   positive y. */
#define CR_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

static int circle_held(void) {
    return interact_tapped();
}

/* Manhattan distance to a gate centre, which the trigger and the sign both want
   and neither wants differently. */
static int32_t gate_dist(int32_t gx, int32_t gz) {
    int32_t dx = cam_x - gx;
    int32_t dz = cam_z - gz;
    return (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
}

static int gate_triggered(int32_t gx, int32_t gz, int *circle_prev) {
    int held = circle_held();
    int just = held && !*circle_prev;
    *circle_prev = held;
    if (!just) return 0;
    /* THE VALVE LOCK, and it goes AFTER the edge state is updated and not
       before: the press still has to be consumed even when it is refused, or a
       Circle held through a locked gate would fire the moment the lock came
       off. Both gates take it -- see the block above wgate_text. */
    if (!valve_puzzle_gates_unlocked()) return 0;
    return gate_dist(gx, gz) < CR_TRIGGER_RADIUS && interact_facing(gx, gz);
}

/* The floating "Press O to enter" sign. door_draw_string_3d centres the READING
   axis on the coordinate it is handed after adding 200, which is why each caller
   passes that axis minus 200; `mirror` and the 11-unit offset off the wall are
   the hand of the gate and are set per gate at the call site — get either
   backwards and the text comes out reversed or buried in the hedge. */
static void gate_text(RenderContext *ctx, int32_t gx, int32_t gz,
                      int32_t tx, int32_t tz, int mirror, int plane) {
    int32_t xz = gate_dist(gx, gz);
    if (xz >= CR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > CR_FADE_NEAR) {
        int range = CR_TEXT_RADIUS - CR_FADE_NEAR;
        int prog  = xz - CR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    if (!valve_puzzle_gates_unlocked()) {
        door_draw_string_3d(ctx, "Locked by some mechanism",
                            tx, CR_TEXT_Y, tz,
                            255, 50, 50, fade, mirror, plane, DOOR_PIXEL_SIZE);
        return;
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        tx, CR_TEXT_Y, tz,
                        50, 255, 50, fade, mirror, plane, DOOR_PIXEL_SIZE);
}

/* ---- THE CHAIN ROOM'S GATES ARE LOCKED UNTIL MAZE TWO'S VALVE IS TURNED -----
   Both of this room's gates, from all four sides, are shut by the Valve Puzzle
   until FLAG_VALVE_MAZE_TWO is set (src/valve_puzzle.c). There is NO world state
   behind it: the leaves are drawn shut either way, their collision walls never
   move, and the trigger is the only thing that ever let the player through — so
   refusing the trigger IS the lock, and the red sign is the only tell.

   All FOUR sides carry it, not just the two outside approaches. The two inside
   ones can only be seen by a player who is somehow already in the room, which no
   normal route allows -- but a debug level-select jump lands there, and a sign
   that offered "Press O to enter" on a gate that would not open is the kind of
   thing that reads as a bug rather than as a locked door. */

/* ---- The WEST-wall gate, back into Maze Two --------------------------------
   The grdn_gte leaf at x=-200 spanning z[-300,300], y[-600,0], in the YZ plane.
   Its alcove is collision FLOOR 2, x(-200,0) z(-300,300), which is what fixes
   the centre at z=0.

   Collision wall 12 runs across the opening with nx = +4096, so the walkable
   side is +X and the player approaches from inside this room heading WEST. For a
   YZ sign that is mirror=0, and it stands 11 units EAST of the wall (x + 11) —
   the Rear Gate's west gate exactly, and the OPPOSITE hand from Maze Two's side
   of this same gate, whose wall 28 faces -X. */
#define CR_WGATE_X        (-200)
#define CR_WGATE_Z            0     /* (-300 + 300) / 2, and FLOOR 2's centre */

/* Circle edge-detect, one per gate, seeded by the arms below. Starts "held" so a
   press carried in through the transition doesn't bounce the player straight
   back. */
static int wgate_circle_prev = 1;
static int sgate_circle_prev = 1;

void chain_room_wgate_arm(void) {
    wgate_circle_prev = circle_held();
}

int chain_room_wgate_triggered(void) {
    return gate_triggered(CR_WGATE_X, CR_WGATE_Z, &wgate_circle_prev);
}

static void wgate_text(RenderContext *ctx) {
    gate_text(ctx, CR_WGATE_X, CR_WGATE_Z,
              CR_WGATE_X + 11, CR_WGATE_Z - 200, 0, TEXT_PLANE_YZ);
}

/* ---- The SOUTH-wall gate, into the Keystone Maze ---------------------------
   The leaf at z=-1100 spanning x[600,1200], y[-600,0], in the XY plane. Its
   alcove is collision FLOOR 1, x(600,1200) z(-1100,-900), fixing the centre at
   x=900.

   Collision wall 9 runs across the opening with nz = +4096, so the walkable side
   is +Z and the player approaches from inside this room heading SOUTH. For an XY
   sign that is mirror=1, and it stands 11 units NORTH of the wall (z + 11) —
   Maze Two's south gate exactly, and the OPPOSITE hand from the Keystone Maze's
   side of this same gate, whose wall 13 faces -Z. */
#define CR_SGATE_X          900     /* (600 + 1200) / 2, and FLOOR 1's centre */
#define CR_SGATE_Z       (-1100)

void chain_room_sgate_arm(void) {
    sgate_circle_prev = circle_held();
}

int chain_room_sgate_triggered(void) {
    return gate_triggered(CR_SGATE_X, CR_SGATE_Z, &sgate_circle_prev);
}

static void sgate_text(RenderContext *ctx) {
    gate_text(ctx, CR_SGATE_X, CR_SGATE_Z,
              CR_SGATE_X - 200, CR_SGATE_Z + 11, 1, TEXT_PLANE_XY);
}

/* ---- Spawns ----------------------------------------------------------------
   One per gate, each set 220 units off its wall — 25 clear of the 195 push
   radius apply_collision_reception() uses — and facing along the direction of
   travel through the gate. Both land on collision FLOOR 0, just past their own
   alcove, so the floor is under the player on the first frame.

   BOTH ARM BOTH GATES. A Circle held through the transition would otherwise fire
   whichever trigger the arrival stands in front of and bounce the player
   straight back out. */
void chain_room_spawn_west(void) {
    /* x=20: 220 clear of wall 12 at x=-200, and at z=0 the nearest points of the
       neck's own side walls (2 and 5, at x=0 spanning z(-900,-300) and
       z(300,900)) are 300 away. */
    cam_x   = CR_WGATE_X + COLLISION_WALL_RADIUS + 25;
    cam_y   = CR_EYE_Y;
    cam_vy  = 0;
    cam_z   = CR_WGATE_Z;
    cam_rot = 1024;   /* facing +X, into the yard */
    chain_room_wgate_arm();
    chain_room_sgate_arm();
}

void chain_room_spawn_south(void) {
    /* z=-880: 220 clear of wall 9 at z=-1100, and 300 from the nearest ends of
       the south hedge runs (walls 3 and 4 at z=-900) and of the neck's own side
       walls (0 and 1, at x=1200 and x=600). */
    cam_x   = CR_SGATE_X;
    cam_y   = CR_EYE_Y;
    cam_vy  = 0;
    cam_z   = CR_SGATE_Z + COLLISION_WALL_RADIUS + 25;
    cam_rot = 0;      /* facing +Z, into the yard */
    chain_room_wgate_arm();
    chain_room_sgate_arm();
}

void chain_room_init(void) {
    chain_room_collision_init(&current_collision_room);
    /* The perimeter hedge is DRAWN to y=-500 (the gates reach -600, but they are
       the openings, not the roofline); the collision runs are 500 tall here,
       which happens to agree. The brick wall and the chains are drawn to -700 —
       see chain_room.h: this value is the roofline over the walkable yard, not
       the tallest thing in the room, and anything ever hung here wants the -700
       instead (tools/ADDING_A_ROOM.txt on visual-vs-collision heights). */
    collision_set_ceiling_y(-500);
    /* No collision_set_wall_radius: the default 195 is right here, as in all
       three mazes. The tightest space is the two 600-wide gate necks, which at
       195 leave 210 units of walkable width — the same margin the mazes'
       corridors give. main.c resets to the default before every room init, so
       saying nothing is enough. */

    chain_room_floor_zones_init();

    /* Default spawn; main.c overrides it with chain_room_spawn_south() when the
       player arrives from the Keystone Maze. */
    chain_room_spawn_west();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room spans x[-200,1800] z[-1100,900], which contains reception's save
       point at (78,-67), so they certainly do. Clearing is safe: reception_init()
       re-places both on every reception entry. No save point of this room's own;
       the nearest is on the Garden Stairs' top landing. */
    save_points_clear();
    dressers_clear();

    /* The valve pipe's prompt keeps its own Circle edge state, so it is armed
       here for the reason both gates are: a press held through the transition
       must not open the board on the arrival frame. */
    valve_puzzle_arm();
}

static void draw_chain_room_smd(RenderContext *ctx) {
    if (!chain_room_smd) return;


    uint8_t *p = (uint8_t *)chain_room_smd->p_prims;
    int i, n = chain_room_smd->n_prims;

    /* Hoisted: all constant for the whole frame, and every one of them would
       otherwise be recomputed per primitive inside the hottest loop in the room
       — the two trig lookups once for each poly that passed the distance cull,
       the cull distance and the debug test all 243 times. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = CR_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    if (cr_key_count < n) n = cr_key_count;   /* keys not built: draw nothing */

    for (i = 0; i < n; i++) {
        /* ---- The reject path. ONE sequential read, no mesh access ----------
           cr_keys carries this primitive's first vertex and its stride, so a
           primitive outside the view distance is skipped without touching the
           header or the vertex array at all. See cr_build_cull_keys. */
        uint8_t stride = cr_keys[i].stride;
        int32_t kdx = (int32_t)cr_keys[i].x - cam_x;
        int32_t kdz = (int32_t)cr_keys[i].z - cam_z;
        if ((kdx < 0 ? -kdx : kdx) + (kdz < 0 ? -kdz : kdz) > cull)
            { p += stride; continue; }

        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &chain_room_smd->p_verts[vi[0]];
        SVECTOR *v1 = &chain_room_smd->p_verts[vi[1]];
        SVECTOR *v2 = &chain_room_smd->p_verts[vi[2]];

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
               and it does not need to be: at 243 primitives over 2000 x 2000
               units this room is entirely inside the 2500 cull from anywhere in
               it, so the frustum test is the only thing rejecting anything at
               all and it costs a handful of multiplies to do it. The test is
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
                        SVECTOR *vk = &chain_room_smd->p_verts[vi[k]];
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
           build time in chain_room_nocull — same scheme as the other rooms. This
           mesh happens to contain none (the generator reports 0), but the table
           is generated and read the same way regardless. */
        int nocull = (i < CHAIN_ROOM_PRIM_COUNT) && chain_room_nocull[i];
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
            v3 = &chain_room_smd->p_verts[vi[3]];
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
        int32_t fog = dist < CR_FOG_NEAR ? CR_FOG_NEAR : (dist > CR_FOG_FAR ? CR_FOG_FAR : dist);
        int32_t fog_factor = ((CR_FOG_FAR - fog) << 8) / (CR_FOG_FAR - CR_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in chain_room_draw. */
        uint8_t tex_idx = (i < CHAIN_ROOM_PRIM_COUNT) ? chain_room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < CHAIN_ROOM_TEX_COUNT);
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
void chain_room_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = CR_FOG_NEAR; g_fog_far = CR_FOG_FAR;

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
       window serves them (see tools/VRAM_MAP.txt) — including the two clones,
       which is half the reason they were placed where they were. */
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

    if (exp != DBG_EXP_NO_MESH) draw_chain_room_smd(ctx);
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
       sample the hedge (see tools/TEXTURING_NOTES.txt PART 5). NOTHING is seeded
       into this room today — world_seed_room() places no enemy and no pickup
       here — so every one of these calls runs over an empty array and costs
       nothing. They are here so that the first thing ever placed in the yard
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

    /* Last: the two gate signs, and the standpipe's. */
    wgate_text(ctx);
    sgate_text(ctx);
    valve_puzzle_text(ctx);

    /* Dead last, and NOT world-space: the valve board is a 2D overlay on the
       menu's OT range, so it goes on top of everything the room has drawn. */
    valve_puzzle_draw(ctx);
}
