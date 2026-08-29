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
#include "btn_glyph.h"
#include "door.h"
#include "cdaudio.h"          /* suspend/resume around the entry-time read */
#include "dresser.h"
#include "save_point.h"

/* ASAG'S ARENA. See asag_arena.h for the whole rationale — this file is the
   mechanism, that one is the argument. */

static SMD  *asag_arena_smd  = NULL;
static void *asag_arena_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   The room is 4000 square, so its far corner is 8000 Manhattan from the near
   one — well past any of the garden's fog settings. These are the garden's
   numbers stretched to fit an indoor space the player must be able to see
   across in one glance: a boss fight where the boss fades out at the far wall is
   not a boss fight, it is a guessing game.

   >>> RE-DERIVE THESE FROM THE REAL MESH. <<< 3000/4500 is chosen against the
   PLACEHOLDER box and against nothing else. Once the arena is modelled, set the
   far distance so the whole playable floor is inside it from any standing
   position, and the near distance so the fog is doing something rather than
   nothing. tools/DIAGNOSING_FRAME_RATE.txt is the file to read if the finished
   room is heavy — measure U/D/G before shortening either of these. */
#define AA_CULL_DIST      4500
#define AA_FOG_NEAR       3000
#define AA_FOG_FAR        4500

/* The arena is UNDERGROUND — the player fell 1200 units to get here — so it
   does NOT take the garden's purple sky. Near-black is both the honest colour
   for a pit and the one that makes additive light (which is what a boss reveal
   is made of, see tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 7) read at full
   strength: additive geometry adds to whatever is behind it, so a dark ground
   is what buys the contrast. Not pure black — a fog that saturates to 0,0,0
   makes the cull line invisible, which sounds good and is actually how you lose
   an hour to "the far wall is missing". */
#define AA_CLEAR_R  6
#define AA_CLEAR_G  4
#define AA_CLEAR_B 10

/* ---- Floor zones -----------------------------------------------------------
   ONE zone over the whole footprint: the placeholder box has a single flat
   plane at y=0, so there is nothing to describe. A real arena with a terrace,
   a step or a pit in it needs one zone per level here AND multi_level set in
   asag_arena_mesh_collision.c — and if it gains a low lip, it needs
   collision_shoot_over_short_walls() in asag_arena_init() too, or every
   projectile in the fight dies on it. See the note at the top of that file. */
static void asag_arena_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -2000; floor_zones[0].max_x = 2000;
    floor_zones[0].min_z = -2000; floor_zones[0].max_z = 2000;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
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
#define ASAG_ARENA_STREAM_TEX 0
static const char *stream_tex_file[] = {
    /* e.g. "\\TEX\\ASAGFLR.TIM;1",     slot 0 */
    /* e.g. "\\TEX\\ASAGWALL.TIM;1",    slot 1 */
    0   /* the array may not be empty; ASAG_ARENA_STREAM_TEX is the real count */
};

static uint16_t tex_tpage[ASAG_ARENA_TEX_COUNT];
static uint16_t tex_clut[ASAG_ARENA_TEX_COUNT];

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

/* ---- The shaft mouth and the exit ------------------------------------------
   The drop lands at the NORTH edge; the exit is the middle of the SOUTH wall.
   Both are 300 in from their wall — 220 is the minimum that clears the default
   COLLISION_WALL_RADIUS of 195 plus the arrival margin, and 300 leaves the
   player's first frame of free play clear of the push-out boundary. Handing
   control back ON the boundary makes the first frame a shove, which is the
   trap tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 2E is about. */
#define AA_SHAFT_X            0
#define AA_SHAFT_Z       (-1700)

#define AA_EXIT_X             0
#define AA_EXIT_Z          2000    /* on the south wall itself */

#define AA_TEXT_Y        (-186)    /* eye level on the y=0 floor */
#define AA_TEXT_RADIUS     1500
#define AA_FADE_NEAR       1000
#define AA_TRIGGER_RADIUS   500

/* ---- Geometry --------------------------------------------------------------
   Read on ENTRY into the shared 116 KB arena, not at startup — the invariant
   every room in this game keeps (src/room_arena.h). A MISSING FILE IS NOT AN
   ERROR HERE: room_arena_load returns NULL, asag_arena_smd stays NULL, and the
   draw falls back to the placeholder box below. That is what lets the room be
   walked and the encounter be scripted before the mesh exists.

   >>> RUN tools/gen_room_arena.py WHEN THE MESH LANDS. <<< The arena is sized to
   the largest .smd on the disc (Maze One, 118 KB / 58 sectors). A bigger mesh
   than that is refused at load time and the room draws empty — which looks
   exactly like the placeholder state below and will waste an afternoon. */
void asag_arena_load_geometry(void) {
    asag_arena_buff = room_arena_load("\\TEX\\ASAGARNA.SMD;1");
    asag_arena_smd  = asag_arena_buff ? smdInitData(asag_arena_buff) : NULL;
}

/* Startup. NOTHING. No CD access, no LoadImage, no RAM copy, no texmgr
   registration — the shape west_corridor.c and garden_courtyard.c already have,
   and the reason this room costs zero permanent bytes.

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
       also why NOTHING NEEDS PUTTING BACK on the way out: the exit is a
       transition like any other, and the destination room's own uploader runs
       on the far side of it. */
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

/* ---- The exit --------------------------------------------------------------
   One door, in the middle of the south wall, in the XZ sense: it faces -Z (back
   into the room), so its sign lies in TEXT_PLANE_XY and stands 11 units north
   of the wall. */
static int circle_held(void) {
    return interact_tapped();
}

static int32_t exit_dist(void) {
    int32_t dx = cam_x - AA_EXIT_X;
    int32_t dz = cam_z - AA_EXIT_Z;
    return (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
}

/* Circle edge state, seeded by the arm below. Starts "held" so a press carried
   in through the transition cannot fire on the arrival frame. */
static int exit_circle_prev = 1;

void asag_arena_exit_arm(void) {
    exit_circle_prev = circle_held();
}

/* >>> WHEN THE ENCOUNTER EXISTS, THIS RETURNS ITS SEAL PREDICATE. <<<
   asag_boss_seals_door() — true from the moment the reveal arms until the death
   sequence has finished, covering the reveal AND the death and not just the
   fight. main.c must test it BEFORE the trigger so the trigger is never polled
   while sealed, and must call asag_arena_exit_arm() on the frame it lifts. See
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 9. */
int asag_arena_exit_sealed(void) {
    return 0;
}

int asag_arena_exit_triggered(void) {
    int held = circle_held();
    int just = held && !exit_circle_prev;
    exit_circle_prev = held;
    if (!just) return 0;
    /* The seal goes AFTER the edge state is updated and not before, the way the
       Chain Room's valve lock does: a press held through a sealed door must
       still be consumed, or it fires the instant the seal comes off. */
    if (asag_arena_exit_sealed()) return 0;
    return exit_dist() < AA_TRIGGER_RADIUS &&
           interact_facing(AA_EXIT_X, AA_EXIT_Z);
}

static void exit_text(RenderContext *ctx) {
    /* Suppressed while sealed. Offering "Press O to leave" on a door that will
       not answer is worse than offering nothing. */
    if (asag_arena_exit_sealed()) return;

    int32_t xz = exit_dist();
    if (xz >= AA_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > AA_FADE_NEAR) {
        int range = AA_TEXT_RADIUS - AA_FADE_NEAR;
        int prog  = xz - AA_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to leave",
                        AA_EXIT_X, AA_TEXT_Y, AA_EXIT_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* ---- Arrival ---------------------------------------------------------------
   The one way in: dropped down the shaft. Faces +Z, across the arena at the
   exit — which is what puts the fight between the player and the way out. */
void asag_arena_spawn_shaft(void) {
    cam_x   = AA_SHAFT_X;
    cam_y   = AA_EYE_Y;
    cam_vy  = 0;
    cam_z   = AA_SHAFT_Z;
    cam_rot = 0;      /* facing +Z */
    asag_arena_exit_arm();
}

void asag_arena_init(void) {
    asag_arena_collision_init(&current_collision_room);

    /* The placeholder box's walls run y[-900,0], and with no mesh to disagree
       that IS the roofline. >>> RE-READ THIS OFF THE .smx WHEN THE MESH LANDS.
       <<< A collision proxy wall is routinely SHORTER than the drawn geometry
       above it, and a ceiling probe wants the height over the walkable ground,
       not the tallest thing in the room. Anything hung higher than this — and a
       boss reveal camera very likely is — takes its own literal. */
    collision_set_ceiling_y(-900);

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
       room spans x[-2000,2000] z[-2000,2000] in its own space, which contains
       plenty of them. Clearing is safe: every room that owns one re-places it on
       entry.

       >>> AND THE ABSENCE OF A SAVE POINT IS A DESIGN CONSTRAINT, NOT AN
       OVERSIGHT. <<< A sealed arena is only sealed if there is no other way out
       of the state. Placing one here would make "no re-entry handling needed"
       false, and the encounter would have to replay or skip its own reveal.
       tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 9. */
    save_points_clear();
    dressers_clear();
}

/* ---- The placeholder box ---------------------------------------------------
   >>> DELETE THIS WHOLE SECTION WHEN THE MESH LANDS. <<< It draws the arena's
   collision footprint as flat-shaded quads so the room can be walked, framed and
   camera-tested with no art on the disc: a floor grid and four walls, 21
   primitives, untextured, no VRAM at all.

   It is NOT a rendering pattern to copy. The real draw is chain_room.c's
   draw_chain_room_smd() — the per-poly tex map, the one 128 texture window, the
   cull-key reject path and the frustum test — and that is what replaces this
   once there is a mesh and a generated <room>_tex_map.h to index. */
#define AA_GRID 4      /* 4x4 floor cells over the 4000 square = 1000 apiece */

static void aa_quad(RenderContext *ctx, const SVECTOR v[4],
                    uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

    DVECTOR sv[4];
    int32_t sz[4], otz;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_stsz4c(sz);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz(&sz[3]);
    if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) return;

    /* The GTE clamps screen coordinates to +/-1023 and a quad with a corner past
       it comes out folded. Dropping the whole quad is the cheap, correct answer
       for a debug grid — see the same guard, and the same reason, in
       lightswitch_puzzle.c's ls_quad. */
    for (int k = 0; k < 4; k++)
        if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
            sv[k].vy <= -1023 || sv[k].vy >= 1023) return;

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= SCENE_OT_MIN) return;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
    setPolyF4(p);
    setRGB0(p, r, g, b);
    p->x0 = sv[0].vx; p->y0 = sv[0].vy;
    p->x1 = sv[1].vx; p->y1 = sv[1].vy;
    p->x2 = sv[2].vx; p->y2 = sv[2].vy;
    p->x3 = sv[3].vx; p->y3 = sv[3].vy;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], p);
    ctx->next_packet += sizeof(POLY_F4);
}

static void draw_placeholder_box(RenderContext *ctx) {
    const int32_t lo = -2000, hi = 2000, top = -900, step = 4000 / AA_GRID;
    int i, j;

    /* Floor: a checker so the grid reads as a scale and not as one flat plane. */
    for (j = 0; j < AA_GRID; j++) {
        for (i = 0; i < AA_GRID; i++) {
            int32_t x0 = lo + i * step, x1 = x0 + step;
            int32_t z0 = lo + j * step, z1 = z0 + step;
            SVECTOR v[4] = {
                { (short)x0, 0, (short)z0, 0 }, { (short)x1, 0, (short)z0, 0 },
                { (short)x0, 0, (short)z1, 0 }, { (short)x1, 0, (short)z1, 0 },
            };
            int dark = (i + j) & 1;
            aa_quad(ctx, v, dark ? 26 : 40, dark ? 22 : 34, dark ? 30 : 46);
        }
    }

    /* Four walls, one quad each, a shade lighter than the floor so the corners
       of the box are legible from the middle of it. */
    {
        SVECTOR n[4] = { { (short)lo, (short)top, (short)lo, 0 },
                         { (short)hi, (short)top, (short)lo, 0 },
                         { (short)lo,         0, (short)lo, 0 },
                         { (short)hi,         0, (short)lo, 0 } };
        SVECTOR s[4] = { { (short)hi, (short)top, (short)hi, 0 },
                         { (short)lo, (short)top, (short)hi, 0 },
                         { (short)hi,         0, (short)hi, 0 },
                         { (short)lo,         0, (short)hi, 0 } };
        SVECTOR w[4] = { { (short)lo, (short)top, (short)hi, 0 },
                         { (short)lo, (short)top, (short)lo, 0 },
                         { (short)lo,         0, (short)hi, 0 },
                         { (short)lo,         0, (short)lo, 0 } };
        SVECTOR e[4] = { { (short)hi, (short)top, (short)lo, 0 },
                         { (short)hi, (short)top, (short)hi, 0 },
                         { (short)hi,         0, (short)lo, 0 },
                         { (short)hi,         0, (short)hi, 0 } };
        aa_quad(ctx, n, 52, 44, 60);
        aa_quad(ctx, s, 52, 44, 60);
        aa_quad(ctx, w, 44, 38, 52);
        aa_quad(ctx, e, 44, 38, 52);
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
       Set even though nothing textured is drawn yet: the sprite renderers and
       the weapon overlay are handed this room's window, and one that was never
       established is one that carries in from whatever ran last. */
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

    if (exp != DBG_EXP_NO_MESH) {
        if (asag_arena_smd) {
            /* THE REAL MESH IS NOT DRAWN YET. Nothing renders an SMD generically
               in this engine — every room's draw loop is its own, because the
               per-poly texture map, the fog ramp and the cull are per-room. Copy
               chain_room.c's draw_chain_room_smd() here when the mesh and its
               generated tex map exist; until then the pointer is loaded and
               unused, which is harmless and is what makes the mesh's arrival a
               one-function change. */
        } else {
            draw_placeholder_box(ctx);
        }
    }

    /* The exit sign, after the room so it sorts against it. */
    exit_text(ctx);

    /* >>> THE ENCOUNTER'S DRAWS GO HERE, IN THIS ORDER. <<<
       tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 10:
         asag_draw(ctx);                 the body — restores the plain view
                                         matrix on its way out
         asag_projectiles_draw(ctx);     after it, wanting that plain matrix
         asag_boss_draw(ctx);            the lights: additive world geometry
         asag_boss_draw_overlay(ctx);    LAST of all — screen space, subtitles */
}
