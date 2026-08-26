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
#include "greenhouse.h"
#include "collision.h"
#include "player.h"
#include "greenhouse_mesh_collision.h"
#include "greenhouse_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "cdaudio.h"           /* suspend/resume around the entry-time read */
#include "dresser.h"
#include "stables.h"             /* stables_upload_textures - the whole set  */
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

/* The Greenhouse: the glasshouse west of the Stables. See greenhouse.h for the
   layout, the shared-world offset between the two, and the VRAM bank. */

static SMD  *greenhouse_smd  = NULL;
static void *greenhouse_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE BANK'S AND THE GARDEN'S: 575/2500, the purple SKY_FOG_* colour, and the
   cull equal to the fog-far so nothing is dropped until the fog has already
   faded it into the background. The Stables next door runs exactly this, as do
   the Rear Gate, Fountain Square and both mazes, so stepping through the
   greenhouse door changes the plan of the place but not the weather.

   It earns its keep more here than in any of them. The footprint is 4500 x 5300
   — the largest in the game — so from the door the far end of the nave is a good
   thousand units PAST the cull, and the beds and the pipework resolve out of the
   purple as the player walks up it. A longer view would show the whole room from
   the doorway and cost the frame rate for the privilege. */
#define GH_CULL_DIST      2500
#define GH_FOG_NEAR        575
#define GH_FOG_FAR        2500

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, and honestly one: the collision generator reported FIVE planes and
   every one of them is at y=0 — the nave, the west annexe, the two north bays
   and the slot joining them. A single rect over the collision bounds covers the
   lot with nothing left over that the player can reach.

   It covers more than the walkable area: the insides of the beds, and the solid
   corners between the nave and the north bays, fall inside it. The player is
   kept out of those by the wall list, and giving them zones of their own would
   only help someone who had already clipped a corner. The Stables, Fountain
   Square, Maze One and the Rear Gate's flat half all do the same. */
static void greenhouse_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -4400; floor_zones[0].max_x =   100;
    floor_zones[0].min_z = -3900; floor_zones[0].max_z =  1400;
    floor_zones[0].y     = 0;

    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   TEN mesh textures — more than any other room — and this room OWNS FIVE, which
   is also a record. Both numbers are only affordable because of the GARDEN-WEST
   VRAM BANK: this room and the Stables are reached only through the Rear Gate's
   west gate, nothing else is drawn while the player is in them, and so the whole
   room-art half of VRAM is theirs to lay out rather than something to squeeze
   into. See tools/VRAM_MAP_GARDEN_WEST.txt.

     0 grss              the ground, inside and out       (stn_stl page, x320 y0)
                                                                       (borrowed)
     1 brick_wall        the shell and the west annexe    (x768 y0)     (borrowed)
     2 greenhouse        the glazing, frames and roof  (trck_clue page, x640 y0)
                                                                       (borrowed)
     3 stables wood      the bed frames and benches     (opn_drwr page, x832 y0)
                                                                       (borrowed)
     4 greenhouse door   the door in the east wall — and the panel this room's
                         only transition is drawn with (frnt_dr page, x320 y256)
                                                                       (borrowed)
     5 flowerbed         the planting in the beds         (hedge page,   x384 y0)
                                                                           OWNED
     6 cuniform pipes    the marked pipework           (grdn_gte page, x512 y0)
                                                                           OWNED
     7 poison_flower_base the bed soil, a 4bpp CLONE (stable glyphs page, x704 y0)
                                                                           OWNED
     8 pipe              the standing pipe in the north bay, a 4bpp CLONE
                                                         (stove page,   x384 y256)
                                                                           OWNED
     9 pipe_button_off   the button on it            (wd_dr_crk page, x512 y256)
                                                                           OWNED

   Slots 0-4 cost this room NOTHING. Two come from the Garden Courtyard's
   uploader (which itself runs the Garden Stairs' first, and it is that call
   which puts brick_wall up) and three from the STABLES', which owns them; this
   room calls stables_upload_textures() wholesale, so all five arrive together.
   That call is also what puts the greenhouse door's own texture in VRAM, which
   matters beyond the mesh: it is the panel DOOR_PANEL_GREENHOUSE is drawn with,
   and both ends of that transition are in this bank (see src/door_anim.c).

   THREE REDIRECTIONS, all for one reason — the texture's own TIM sits on a page
   this room already draws something else from:

     grss               -> the grss_gs CLONE at x320 y0. grss.tim is at x768 y0,
                           which is brick_wall's page. Every garden room since the
                           Garden Stairs makes this same swap.
     poison_flower_base -> a new 4bpp CLONE, PSNFLGH, at x704 y0. Its own TIM is
                           at x640 y0, which is where `greenhouse` lives.
     pipe               -> a new 4bpp CLONE, PIPEGH, at x384 y256. Its own TIM is
                           on brick_wall's page again.

   The five this room owns went to pages it draws NOTHING from, the same test
   every room applies:

     flowerbed       -> x384 y0, the page the courtyard's uploader has just put
                        HEDGE on. There is no hedge in a greenhouse. 8bpp FULL
                        page; the rest of its occupants (chnlnk_dl, clsd_drwr,
                        cncrte, kchn_tile, piano_keys, xt_dr_outr) are mansion
                        art this room never draws and every one is re-uploaded
                        by its own room on entry.
     cuniform pipes  -> x512 y0, likewise GRDN_GTE's page — no gate here — and
                        shared with dresser and kchn_wl, both streamed.
     poison_flower_base_gh -> x704 y0, likewise STABLE GLYPHS'. 4bpp, so it
                        occupies only x[704,736) and stays clear of anzu3/anzu6
                        at x736 — the trap Maze Two fell into by putting an 8bpp
                        texture on this page.
     pipe_gh         -> x384 y256, the prpl_wlppr/stove page. 4bpp, left half
                        only, clear of stnd_rnds at x416.
     pipe_button_off -> x512 y256, the wd_dr_crk page. 4bpp, left half only,
                        clear of wx_cb at x544. THE ONE SLOT IN THIS ROOM THAT
                        COST SOMEBODY A RESTORE: wd_dr_crk is the cracked
                        fat-door texture, read off the CD once at startup by
                        src/fatdoor.c and never re-uploaded, so taking its page
                        would have left every last-hit kitchen door showing a
                        garden button for the rest of the run. It is now the
                        seventh entry in KITCHEN_SHARED_TEX and comes back on
                        kitchen entry with the rest. There was no obligation-free
                        page left to take instead — every other Voff-0 slot in
                        the bank holds the kitchen's resident set, the Anzu
                        tiles, wd_dr, Rabisu or inr_dbl_dr.

   >>> AND THE CLUTS TIME-SHARE, WHICH NO ROOM HAD DONE BEFORE. <<< A CLUT is
   only words in VRAM and texmgr_upload puts pixels and palette up together, so
   the same "displaced, and restored by whoever needs it" argument that governs
   pixels governs palettes. That mattered here: there is exactly ONE free
   256-word CLUT run left in the whole map (y=511, x[288,544)), and two 8bpp
   textures would have eaten it and left the next room with none. Instead each
   takes the palette row of the texture whose pixels it is already replacing —
   flowerbed on hedge's at (672,489), cuniform pipes on grdn_gte's at (672,490),
   poison_flower_base_gh on stable glyphs' at (256,511). One swap, pixels and
   palette, restored by one uploader. Only the two small 4bpp clones needed new
   palette space, and they took 32 words out of a 128-word gap at y=502 that
   could never have held a 256-word CLUT anyway.

   All ten sit at Voff 0, so the one 128 texture window in greenhouse_draw serves
   them all. */
#define GREENHOUSE_TEX_COUNT 10

static uint16_t tex_tpage[GREENHOUSE_TEX_COUNT];
static uint16_t tex_clut[GREENHOUSE_TEX_COUNT];

/* The five textures this room OWNS. >>> THEY ARE STREAMED OFF THE CD ON ENTRY,
   NOT HELD IN RAM. <<< This is the ONE room that does not use texmgr for its own
   art, and the reason is main RAM rather than taste.

   texmgr keeps the WHOLE TIM resident for the life of the run so that a room
   entry is a pure LoadImage. That is the right trade almost everywhere, but it
   is not free, and by the time this room was added it had stopped being cheap:
   67 registrations were holding 902 KB of a 1196 KB heap, and the permanent
   total across texmgr and the props' model buffers came within about 140 KB of
   filling it. Five more here (66 KB) took it past the edge — and what that looks
   like is NOT a tidy allocation failure. PSn00bSDK's InitHeap is handed
   everything from _end up to 0x801FFFF8, which is the STACK; there is no guard
   between them. So a late malloc SUCCEEDS, hands back memory the stack is
   already using, the CdRead DMAs over the return address, and the console jumps
   into nothing. It surfaced inside rabisus_load_assets — the next large read
   after this room's — and read as a boot hang with nothing to do with the
   Greenhouse.

   So this room reads its five on the transition instead, into ONE scratch buffer
   that is freed again: the shape kitchen_stream_textures already uses. Zero
   permanent bytes, against about 57 KB of extra CD read on the one transition
   that reaches this room.

   THAT IS SAFE HERE, AND IT IS NOT A NEW KIND OF OPERATION. A data read issued
   while CD-DA streams hangs the drive, so it must be bracketed by
   cdaudio_suspend/resume — and the very same STATE_LOADING frame already makes
   two bracketed reads before this one (room_arena_load for the mesh,
   sound_bank_select for the SPU set). This is the third, on the same idled GPU,
   behind the same bracket. See tools/ADDING_A_ROOM.txt on the upload rules and
   on the heap budget.

   The tpage/clut are NOT captured at read time: all ten of this room's slots are
   compile-time constants from tim_slots.h, set once in greenhouse_load_assets.
   Keep this table in step with the slot numbering above and with NAME_TO_SLOT in
   gen_greenhouse_tex_map.py. */
#define GREENHOUSE_NEW_TEX 5
static const char *new_tex_file[GREENHOUSE_NEW_TEX] = {
    "\\TEX\\FLWRBED.TIM;1",    /* slot 5 */
    "\\TEX\\CUNIPIPE.TIM;1",   /* slot 6 */
    "\\TEX\\PSNFLGH.TIM;1",    /* slot 7 */
    "\\TEX\\PIPEGH.TIM;1",     /* slot 8 */
    "\\TEX\\PIPEBTNO.TIM;1",   /* slot 9 */
};

/* Biggest of the five is 16928 bytes (an 8bpp 128x128 plus its CLUT) = nine
   sectors. One buffer serves all five, in turn. */
#define GH_TEX_SCRATCH  (10 * 2048)

/* ---- Cull keys -------------------------------------------------------------
   The Rear Gate's scheme, by way of the Stables', and for the same reason: the
   cull key is lifted out into its own array, built once at load — the first
   vertex's X and Z plus the primitive's stride — so a rejected primitive costs
   ONE sequential 6-byte read and never touches the mesh or its header at all.
   Identical output; this changes what the reject path READS, not what it
   decides.

   The margin is the best of any room that has it: 1230 primitives over
   4500 x 5300 against a 2500 Manhattan cull, so from most of the nave well over
   half of them are rejected on the key alone. It costs 6 bytes a primitive
   (7.2 KB of BSS). Indices match the draw loop's `i`. */
typedef struct { int16_t x, z; uint8_t stride, pad; } GhCullKey;
static GhCullKey gh_keys[GREENHOUSE_PRIM_COUNT];
static int       gh_key_count = 0;

static void gh_build_cull_keys(void) {
    gh_key_count = 0;
    if (!greenhouse_smd) return;
    uint8_t *p = (uint8_t *)greenhouse_smd->p_prims;
    int i, n = greenhouse_smd->n_prims;
    if (n > GREENHOUSE_PRIM_COUNT) n = GREENHOUSE_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &greenhouse_smd->p_verts[vi[0]];
        gh_keys[i].x      = v0->vx;
        gh_keys[i].z      = v0->vz;
        gh_keys[i].stride = pt->len;
        gh_keys[i].pad    = 0;
        p += pt->len;
    }
    gh_key_count = n;
}

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. At 69 KB this mesh is well
   inside the arena Maze One's 117 KB sizes, so nothing there had to change. */
void greenhouse_load_geometry(void) {
    greenhouse_buff = room_arena_load("\\TEX\\GRNHSE.SMD;1");
    greenhouse_smd  = greenhouse_buff ? smdInitData(greenhouse_buff) : NULL;
    gh_build_cull_keys();
}

/* Startup. NOTHING happens here but ten compile-time constants: no CD read, no
   LoadImage, no RAM copy, no texmgr registration. Slots 0-4 are put in VRAM by
   other modules' uploaders and 5-9 are streamed on entry (see the table above
   and greenhouse_upload_textures), and a tpage/clut is a pair of numbers the
   linker already knows. west_corridor.c and garden_courtyard.c are the same
   shape; this room is only the first to reach it while still owning art. */
void greenhouse_load_assets(void) {
    /* Borrowed: slots 0-1 from garden_courtyard_upload_textures(), 2-4 from the
       Stables' own uploader, both reached through greenhouse_upload_textures(). */
    TIM_SLOT(0, GRSSGS);
    TIM_SLOT(1, BRIKWLL);
    TIM_SLOT(2, GRNHOUSE);
    TIM_SLOT(3, STBLWOOD);
    TIM_SLOT(4, GRNHSDR);

    /* This room's own five, streamed on entry — the header is still a constant. */
    TIM_SLOT(5, FLWRBED);
    TIM_SLOT(6, CUNIPIPE);
    TIM_SLOT(7, PSNFLGH);
    TIM_SLOT(8, PIPEGH);
    TIM_SLOT(9, PIPEBTNO);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).

   ORDER MATTERS. stables_upload_textures() runs the Garden Courtyard's uploader
   and then its own four, and between them they stamp x384 y0 (hedge), x512 y0
   (grdn_gte) and x704 y0 (stable glyphs) — which are exactly where this room's
   flowerbed, cuniform pipes and flower-bed soil go, palettes included. So all
   five of this room's own textures go up AFTER that call, never before. Three of
   the five would be silently replaced by the Stables' art otherwise, which looks
   exactly like a texture that failed to load.

   The two 4bpp clones' pages (x384 y256 and x512 y256) are touched by nothing in
   this chain, so their order is free; they go up with the rest to keep the set
   together.

   Calling the Stables' uploader WHOLESALE rather than through narrow accessors
   is deliberate. Three of its slots are wanted here as they are; the other three
   are wanted only so their pages are in a known state before this room
   overwrites them. And it is the call that puts the GREENHOUSE DOOR up — the
   texture DOOR_PANEL_GREENHOUSE draws the transition with — so having it in one
   place, on both sides of that door, is worth more than three saved LoadImages
   on a loading screen.

   THE CD READ. Unlike every other room's uploader this one touches the drive,
   because this room's five are not resident (see the table above for why). The
   bracket is mandatory, not defensive: a data read issued while CD-DA streams
   hangs the drive. cdaudio_suspend/resume are no-ops when nothing is playing,
   and the transition into here has already stopped the music at the door
   trigger — but a debug level-select jump can arrive with a track running, and
   that is the case the bracket is for. */
static void gh_stream_tim(const char *filename, uint8_t *buf, int bufcap) {
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

void greenhouse_upload_textures(void) {
    stables_upload_textures();            /* the courtyard's four, then the
                                             Stables' own four: this room wants
                                             grss_gs, brick_wall, greenhouse,
                                             stables wood and greenhouse door
                                             out of that set                  */

    uint8_t *scratch = malloc(GH_TEX_SCRATCH);
    if (!scratch) return;                 /* draw with the Stables' art rather
                                             than crash — 20 KB is always there
                                             at the point a room is entered   */
    cdaudio_suspend();
    for (int i = 0; i < GREENHOUSE_NEW_TEX; i++)
        gh_stream_tim(new_tex_file[i], scratch, GH_TEX_SCRATCH);
    cdaudio_resume();
    free(scratch);                        /* flowerbed -> hedge, cuniform pipes ->
                                             grdn_gte, soil -> stable glyphs,
                                             pipe -> stove, button -> wd_dr_crk */
}

/* ---- The east-wall door back to the Stables --------------------------------
   The greenhouse door polys on this side span z[-300,100] at x=100, y[-675,0].
   It is the same door leaf as the one in the Stables' west brick wall, which
   stands at x=-3400 z[-133,133] there — the two rooms' meshes are independent,
   and only this pairing links them, though they were modelled in a shared world
   and greenhouse_x = stables_x + 3500, greenhouse_z = stables_z - 100 maps one
   onto the other exactly (see greenhouse.h).

   Collision wall 29 runs the full east side at x=100 with nx = -4096, so the
   walkable side is -X and the player approaches from inside this room. For a
   sign in the YZ plane that is mirror=1, and it stands 11 units WEST of the wall
   (x - 11). Both are the opposite hand from the Stables' side of the same door,
   whose wall faces +X into that room. Getting either backwards comes out as
   mirrored text or a sign buried in the brickwork.

   The wall STAYS in the collision list: the door is shut as far as collision is
   concerned and it is the trigger, not a hole, that lets the player through. */
#define GH_DOOR_X              100
#define GH_DOOR_Z            (-100)   /* (-300 + 100) / 2                       */
#define GH_TEXT_Y            (-186)   /* eye level on the y=0 floor             */
#define GH_TEXT_RADIUS        1500
#define GH_FADE_NEAR          1000
#define GH_TRIGGER_RADIUS      500

/* Standing eye on the floor: y=0, less GROUND_FLOOR_Y and the 40-unit floor
   standoff apply_height applies. The same expression the Stables, Fountain
   Square, the Outside Catacombs, both mazes and the Rear Gate's lawn use — this
   whole run of garden rooms has its floor at y=0 while the courtyard below is at
   positive y. */
#define GH_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by greenhouse_door_arm(). Starts "held" so a press
   carried in through the transition doesn't bounce the player straight back.
   ONE door is connected here — it is the only opening in the room — so there is
   only one edge state to keep. */
static int door_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void greenhouse_door_arm(void) {
    door_circle_prev = circle_held();
}

int greenhouse_door_triggered(void) {
    int held = circle_held();
    int just = held && !door_circle_prev;
    door_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - GH_DOOR_X;
    int32_t dz = cam_z - GH_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < GH_TRIGGER_RADIUS && interact_facing(GH_DOOR_X, GH_DOOR_Z);
}

/* Floating "Press O to enter" sign on the door. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. Sits just west (x-11) of the wall so it floats in front
   of the door on the side the player is actually on. */
static void door_text(RenderContext *ctx) {
    int32_t dx = cam_x - GH_DOOR_X;
    int32_t dz = cam_z - GH_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GH_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > GH_FADE_NEAR) {
        int range = GH_TEXT_RADIUS - GH_FADE_NEAR;
        int prog  = xz - GH_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        GH_DOOR_X - 11, GH_TEXT_Y, GH_DOOR_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving from the Stables: stand WEST of the x=100 door wall, clear of the
   wall push radius (so the player isn't shoved on their first frame), facing -X
   — the direction of travel through the door, looking straight down the nave
   between the beds.

   x lands at -120 and z on the door's own centre line. The nearest wall from
   there is the door itself, 220 behind; the first bed (wall 9, at x=-500) is 380
   ahead, so nothing pushes the player anywhere on the frame they arrive. */
void greenhouse_spawn_east(void) {
    cam_x   = GH_DOOR_X - COLLISION_WALL_RADIUS - 25;
    cam_y   = GH_EYE_Y;
    cam_vy  = 0;
    cam_z   = GH_DOOR_Z;
    cam_rot = 3072;   /* facing -X, into the room */
    greenhouse_door_arm();
}

void greenhouse_init(void) {
    greenhouse_collision_init(&current_collision_room);
    /* The shell is DRAWN to y=-900 at the eaves and the glass ridge runs on up
       to -1205; the collision runs stop at -675, which is the height of the door
       and of the walls the player can actually be pushed by. STATE THE DRAWN
       VALUE, not the collision one, so anything ceiling-mounted hangs where the
       room really has a ceiling (see tools/ADDING_A_ROOM.txt on visual-vs-
       collision heights) — and state the EAVES rather than the ridge, because
       the ridge is a line down the centre and -900 is the height the glass
       actually has almost everywhere. The beds, at -225, are far lower than
       either and are not what this means. */
    collision_set_ceiling_y(-900);
    /* No collision_set_wall_radius: the default 195 is right here. The tightest
       space is the 599-wide slot joining the north bays to the nave (walls 11
       and 9, at x=-1099 and x=-500), which at 195 leaves 209 units of walkable
       width — tighter than the Stables' lane, but the same margin the Rear
       Gate's hedged corridor runs on. main.c resets to the default before every
       room init, so saying nothing is enough. */

    greenhouse_floor_zones_init();

    /* The only spawn: the east door, back into the Stables. There is nowhere
       else to arrive from, so main.c needs no override. */
    greenhouse_spawn_east();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room spans x[-4400,100] z[-3900,1400], which puts reception's save point
       at (78,-67) just inside the door. Clearing is safe: reception_init()
       re-places both on every reception entry. No save point of this room's own;
       the nearest is on the Garden Stairs' top landing. */
    save_points_clear();
    dressers_clear();
}

static void draw_greenhouse_smd(RenderContext *ctx) {
    if (!greenhouse_smd) return;

    uint8_t *p = (uint8_t *)greenhouse_smd->p_prims;
    int i, n = greenhouse_smd->n_prims;

    /* Hoisted: all constant for the whole frame, and every one of them would
       otherwise be recomputed per primitive inside the hottest loop in the room —
       the two trig lookups once for each poly that passed the distance cull, the
       cull distance and the debug test all 1230 times. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = GH_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    if (gh_key_count < n) n = gh_key_count;   /* keys not built: draw nothing */

    for (i = 0; i < n; i++) {
        /* ---- The reject path. ONE sequential read, no mesh access ----------
           gh_keys carries this primitive's first vertex and its stride, so a
           primitive outside the view distance is skipped without touching the
           header or the vertex array at all. See rg_build_cull_keys. */
        uint8_t stride = gh_keys[i].stride;
        int32_t kdx = (int32_t)gh_keys[i].x - cam_x;
        int32_t kdz = (int32_t)gh_keys[i].z - cam_z;
        if ((kdx < 0 ? -kdx : kdx) + (kdz < 0 ? -kdz : kdz) > cull)
            { p += stride; continue; }

        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &greenhouse_smd->p_verts[vi[0]];
        SVECTOR *v1 = &greenhouse_smd->p_verts[vi[1]];
        SVECTOR *v2 = &greenhouse_smd->p_verts[vi[2]];

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

               Y is not tested, and that is right here: the floor is one flat
               plane at y=0 everywhere, so adding top and bottom planes would
               cost multiplies to reject nothing. The glass ridge at -1205 is the
               tallest thing in the room and the camera never leaves standing
               height under it.
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
                        SVECTOR *vk = &greenhouse_smd->p_verts[vi[k]];
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
           build time in greenhouse_nocull — same scheme as the other rooms. This
           mesh has none either (gen_greenhouse_tex_map.py reports 0), but the
           table is generated and checked all the same. */
        int nocull = (i < GREENHOUSE_PRIM_COUNT) && greenhouse_nocull[i];
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
            v3 = &greenhouse_smd->p_verts[vi[3]];
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
        int32_t fog = dist < GH_FOG_NEAR ? GH_FOG_NEAR : (dist > GH_FOG_FAR ? GH_FOG_FAR : dist);
        int32_t fog_factor = ((GH_FOG_FAR - fog) << 8) / (GH_FOG_FAR - GH_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in greenhouse_draw. */
        uint8_t tex_idx = (i < GREENHOUSE_PRIM_COUNT) ? greenhouse_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < GREENHOUSE_TEX_COUNT);
        /* Purple fog, the same night sky the rest of the garden looks out on,
           at the bank's 575/2500 — the Stables next door, the Rear Gate beyond
           it and Fountain Square all run exactly this. The glass makes no
           difference to it: what you can see through a roof at night is the same
           sky the yard has. */
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


void greenhouse_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = GH_FOG_NEAR; g_fog_far = GH_FOG_FAR;

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
       page. All TEN of this room's textures sit at page-top (Voff 0) — the five
       it owns were placed with that as a hard requirement, and the five it
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

    if (exp != DBG_EXP_NO_MESH) draw_greenhouse_smd(ctx);

    /* Every sprite enemy renderer is handed this room's texture window, because
       all of their sprites live at Voff >= 128 and must bracket it rather than
       sample the flower bed (see tools/TEXTURING_NOTES.txt PART 5). NOTHING is
       placed here yet — the updaters and draws below run over empty arrays and
       cost nothing — but they are wired up so that a future spawn brackets its
       quad correctly instead of drawing this room's planting on a monster.

       THE RAFFLESIA AND THE MUSHROOM HEAD ARE THE TWO THAT ARE READY, exactly
       as in the Stables — they are the bank's two, reserved in
       tools/VRAM_MAP_GARDEN_WEST.txt so that neither room's art can take their
       slots. The mushrooms own their four outright and are resident for the
       whole run, and main.c's pending_area test streams the RAFFLESIA half of
       the shared x320/x384 y128 sprite pair in on entry here rather than the
       spiders': this is a garden room and spiders are house-interior. Both are
       also sound-legal — this room is on SND_BANK_GARDEN (main.c's
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

    /* Last: the door's sign, the only one in the room. */
    door_text(ctx);
}
