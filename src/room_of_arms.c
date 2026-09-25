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
#include "room_of_arms.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "room_of_arms_mesh_collision.h"
#include "room_of_arms_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the three narrow uploaders this room borrows */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Room of Arms — see room_of_arms.h for the layout and the door list. */

static SMD  *roa_smd  = NULL;
static void *roa_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY, unchanged: cull and fog-far are equal (so nothing is
   dropped until the fog has already faded it into the background), the base is
   scaled by what the player is carrying, and the scale eases rather than jumping.

   450/1600 — THE BURIAL HALL'S, THE INCINERATOR ROOM'S AND THE TOMB'S. The
   argument the Tomb had to make (is this room's plan its puzzle, the way the Up
   Down Maze's is?) does not even arise here: this room has no plan to give away.
   It is one octagonal chamber with a screen wall across its north-west third and
   a single door, and the player can see the whole walkable floor from anywhere
   in it whatever the fog is set to.

   >>> BUT THE FOG IS DOING SOMETHING IN THIS ROOM THAT IT IS NOT DOING IN THE
   OTHER FOUR, AND IT IS WHY THESE NUMBERS ARE NOT PULLED IN. <<< The only thing
   in here is what is BEHIND the screen wall: a floor of grasping arms in a
   pocket the player can never walk into, seen through the 634-wide gap in that
   wall (see the header). The gap is at roughly x=-270 z=130 and the arms run
   back from it to the octagon's far corner at (-1293,1293) — so the far end of
   the field is about 2400 from the gap's mouth and the WHOLE of it is past 1600.
   At rest the player stands at the gap and sees arms fading into the dark a
   third of the way in. That is the room.

   THE HELLUMINATOR IS WHAT OPENS IT. Raising the lantern and then burning it
   each add 50% of the base, so the three distances are 1600, 2400 and 3200: lit
   and burning, and only then, the back wall of the pocket is inside the fog and
   the field reads as finite. The chapter's bargain, and here it is the room's
   one piece of content rather than a convenience — which is the case for
   spending the light, not for making the room bright.

   AND IT IS ALSO THIS ROOM'S FRAME BUDGET. `cull` and fog-far being equal means
   these numbers decide how much MESH is walked, transformed and queued. 578
   primitives here against the Incinerator Room's 708, the Tomb's 936 and the Up
   Down Maze's 1990 — still the smallest room in the chapter, which is why 3200
   burning is affordable at all.

   >>> THAT WAS 290 UNTIL THE MESH WAS SUBDIVIDED, AND THE REASON IT DOUBLED IS
   THE REASON THESE NUMBERS MUST NOT BE RAISED TO COMPENSATE. <<< The room was
   re-exported with its larger faces cut up because the big ones were CLIPPING —
   the draw loop rejects any primitive with a screen vertex outside ±1023 (the
   four tests above the nclip), so a wall quad big enough to overrun the guard
   band vanishes whole the moment the camera gets close to it. Smaller faces fail
   that test less often. The cost of the fix is exactly this line: twice the
   primitives walked, transformed and queued at the same view distance. Anything
   that lengthens these has to be measured BURNING, not at rest, and now on the
   578 (STEP 3J of tools/DIAGNOSING_FRAME_RATE.txt). */
#define ROA_BASE_FOG_NEAR   450
#define ROA_BASE_FOG_FAR   1600

#define ROA_VIEW_UNIT        256
#define ROA_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define ROA_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define ROA_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t roa_view     = ROA_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t roa_fog_near = ROA_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t roa_fog_far  = ROA_BASE_FOG_FAR;

static int32_t roa_view_target(void) {
    int32_t s = ROA_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += ROA_VIEW_HELL_BONUS;
        /* Only asked INSIDE the equipped test: helluminator_burning() cannot be
           true for an unequipped lantern, but nesting it says so rather than
           relying on the weapon layer to keep clearing it. */
        if (helluminator_burning()) s += ROA_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry: there is no previous frame to ease from
   and walking in with the lantern already up must not open the room out over the
   first half second. */
static void roa_view_resolve(int snap) {
    int32_t target = roa_view_target();
    if (snap) {
        roa_view = target;
    } else if (roa_view < target) {
        roa_view += ROA_VIEW_RATE;
        if (roa_view > target) roa_view = target;
    } else if (roa_view > target) {
        roa_view -= ROA_VIEW_RATE;
        if (roa_view < target) roa_view = target;
    }
    roa_fog_near = (ROA_BASE_FOG_NEAR * roa_view) >> 8;
    roa_fog_far  = (ROA_BASE_FOG_FAR  * roa_view) >> 8;
}

/* Underground, so the same near-black-with-a-cold-lift the rest of the chapter
   uses, and for the same reason (src/catacombs_entry.c): a fog that saturates to
   a true 0,0,0 makes the cull line invisible, which sounds ideal and is how you
   lose an hour to "the far end of the arms is missing" — and in THIS room that
   line falls across the one thing worth looking at, so it matters more here than
   anywhere else in the chapter. */
#define ROA_FOG_R             7
#define ROA_FOG_G             6
#define ROA_FOG_B             9

/* Wall standoff. The chapter's 195, and nothing in this room argues with it: the
   narrowest place the player can walk is the mouth between the screen wall's
   inner kink at (-403,-155) and the octagon's west wall, which is over 800
   across. The one tight-ish spot is the 178-wide plinth standing off the south
   wall at x[357,535] — the player walks AROUND it, never between it and
   anything, so 195 costs nothing there either. */
#define ROA_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. Used for the spawn only — apply_height
   settles cam_y every frame afterwards. */
#define ROA_FLOOR_Y            0
#define ROA_EYE_Y           (ROA_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, SPANNING THE WHOLE FOOTPRINT — the Tomb's case, and it is
   worth saying why the proxy's THREE floor faces do not make this a multi-level
   room. They are three slabs of one plane, all at y=0 (see the FLOOR list at the
   foot of src/room_of_arms_mesh_collision.c): the exporter cut the octagon's
   floor into a west, a centre and an east piece and every one of them answers
   the same height. A floor zone answers "how high is the ground at this XZ", so
   three zones would be three identical answers.

   FLOOR_FLAT, not FLOOR_UPPER: nothing in this room sets player_on_upper_floor,
   because there is no upper floor to be on. And with one zone there is no ORDER
   to get wrong, which two rooms along is the whole mechanism (the Up Down Maze's
   ten walkways over its catch-all).

   THE RECT IS THE COLLISION BOUNDS AND IT IS DELIBERATELY BIGGER THAN THE
   WALKABLE FLOOR. An octagon with a screen wall across one corner does not fit a
   rect, and it does not have to: the walls decide where the player may BE and
   the zone only decides how high the ground is once they are there. The corners
   this rect covers and the walls exclude — the four diagonal chamfers, and the
   whole north-west pocket the arms are in — are places apply_height is never
   asked about.

   z stops at 1198, not at the mesh's 1293: 1198 is where the screen wall meets
   the octagon's north-east chamfer at (630,1198), and everything past it is
   pocket. That is the collision generator's max_z and it is the right one. */
static void roa_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1293; floor_zones[i].max_x = 1293;
    floor_zones[i].min_z = -1293; floor_zones[i].max_z = 1198;
    floor_zones[i].y     = ROA_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, AND THIS IS THE FIRST CHAPTER 3 ROOM THAT OWNS ONE.

     0 cobblestones        the outer walls, the floor, the vaulting and the
                           screen wall — 524 of the 578 polys
                           (clsd_drwr page, x384 y0)
     1 arms                the field of grasping arms in the sealed pocket, 52
                           polys, and the only thing in this room
                           (trck_clue/hatch page, x640 y0)   OWNED HERE
     2 catacomb inner door the one doorway, east, to the Tomb
                           (opn_drwr page,  x832 y0)

   Slots 0 and 2 are registered by src/catacombs_entry.c in TEXBANK_CATACOMBS and
   reached through that module's NARROW uploaders, exactly as the Up Down Maze,
   the Incinerator Room and the Tomb reach theirs — the narrow form for the usual
   reason (tools/ADDING_A_ROOM.txt STEP 3b): the Catacombs Entry's FULL uploader
   would also stamp the lamashtu tablet, the loculus, the sconce and the oil
   dispenser, none of which this room draws.

   >>> SLOT 1 IS NEW ART AND THERE WAS NO WAY TO BORROW IT. <<< Every other room
   in this chapter has cost the disc one mesh and nothing else, because every one
   of them is built out of the burial hall's four textures. This one is not: the
   arms are a picture that exists nowhere else in the game, so somebody has to
   hold the registration and it may as well be the only room that draws it. It is
   registered DEFERRED like the rest of the chapter (src/texmgr.h) — the name is
   recorded at startup and the pixels are read at the chapter door by
   area_bank_sync() — so Chapters 1 and 2 pay nothing for it.

   >>> AND IT IS THE 69th REGISTRATION OF 72. <<< TEXMGR_MAX in src/texmgr.c is
   the cap, texmgr_register past it returns -1 SILENTLY, and a silently-failed
   registration breaks that texture in every room that draws it. Three left. The
   way to spend none is the one slots 0 and 2 take above and the one Fountain
   Square and the Garden Courtyard take: if another module already uploads the
   page you need, call ITS narrow uploader and use TIM_SLOT() for the header,
   rather than registering a second RAM copy of the same file. Count with
   py tools/heap_budget.py before adding the seventieth.

   ALL THREE SIT AT Voff 0, so the one 128 texture window set in
   room_of_arms_draw serves them (tools/VRAM_MAP.txt). x640 y0 is the page
   tools/VRAM_MAP_CATACOMBS.txt marks "FREE — costs nothing": nine textures share
   it already and every one of them is re-uploaded by its own room's entry, so
   taking it adds no restore obligation to this room. The arms' CLUT is borrowed
   from asag.tim at (672,501) on the same terms — the pixels it displaces and the
   palette it displaces belong to the same texture, so the two go back together
   in one stream. The whole argument is in tools/vram_map.py beside the pairs. */
#define ROOM_OF_ARMS_TEX_COUNT 3

static uint16_t tex_tpage[ROOM_OF_ARMS_TEX_COUNT];
static uint16_t tex_clut[ROOM_OF_ARMS_TEX_COUNT];

/* The one texmgr entry this room owns (slot 1). -1 until registration, which is
   also what a registration past TEXMGR_MAX leaves behind — texmgr_upload on -1
   is a no-op, so the failure mode is the arms drawing as whatever else is on
   x640 y0 rather than a crash. */
static int arms_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/incinerator_room.c, which took it from src/up_down_maze.c,
   which took it from src/catacombs_entry.c, which has been through that
   document.

   The keys go in the SHARED arena (src/cull_arena.h) and are built on the same
   call that reloads the mesh they describe, which is the one rule that file has.
   A rejected primitive is then ONE sequential 6-byte read and never addresses the
   SMD header, the index array or the vertex array.

   NO BOX KEY AND NO SIDE-PLANE CULL, for the reason the Tomb, the Incinerator
   Room, the Up Down Maze, the Catacombs Entry, the Greenhouse, Maze One,
   Reception and the Master Bedroom all record: cull_boxes pays for itself only
   where a side-plane test would otherwise chase v1..v3 per surviving primitive,
   and there is no such test here to feed. "The room is open so it would cull a
   lot" is wrong turn #2 in DIAGNOSING_FRAME_RATE.txt and it has now been the
   tempting wrong answer eight times. It is the LEAST tempting here of all eight:
   578 primitives is still the smallest mesh in the chapter and a good share of
   them are on screen at once from anywhere in the room, so there is not much of
   a reject path to make cheaper. If a METER ever says otherwise, that is the
   case for adding one, with a box key under it and an offline hole sweep beside
   it — and the subdivision that took this room from 290 to 578 is the kind of
   change that would make such a meter worth re-reading. */
static int roa_key_count = 0;

static void roa_build_cull_keys(void) {
    roa_key_count = 0;
    if (!roa_smd) return;
    uint8_t *p = (uint8_t *)roa_smd->p_prims;
    int i, n = roa_smd->n_prims;
    if (n > ROOM_OF_ARMS_PRIM_COUNT) n = ROOM_OF_ARMS_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &roa_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    roa_key_count = n;
}

void room_of_arms_load_geometry(void) {
    roa_buff = room_arena_load("\\TEXCTCMB\\ARMSROOM.SMD;1");
    roa_smd  = roa_buff ? smdInitData(roa_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    roa_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: three compile-time headers and ONE
   deferred registration. Deferred means the TIM's header is read at startup and
   its 16.5 KB of pixels only when a bank containing it is selected
   (src/texmgr.h), so Chapters 1 and 2 pay nothing for the arms.

   >>> texmgr_set_bank() IS HERE, unlike in the Up Down Maze, the Incinerator
   Room and the Tomb. <<< Those three make no registration at all, so they have
   nothing to tag and the call would be a no-op; this one does. The mask is
   DERIVED and not guessed — py tools/check_tex_banks.py walks the uploader call
   graph, finds this module reached only from room_of_arms_upload_textures(), and
   fails the build if CATACOMBS is not in it. Getting it wrong is SILENT: an
   upload whose bank is out does nothing and the room draws whatever the last
   room left on x640 y0. */
void room_of_arms_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    arms_tex = texmgr_register("\\TEXCTCMB\\ARMS.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, ARMS);
    TIM_SLOT(2, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   NO ORDERING RULE, for the Catacombs Entry's reason: nothing else uploads to
   these three pages while this chapter is resident, because nothing else in the
   game is reachable. If a texmgr entry is somehow not loaded, texmgr_upload is a
   no-op and the slot keeps whatever the previous room left in it — visibly
   wrong, and quietly, which is why area_bank_sync runs before this and not
   after. */
void room_of_arms_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    /* ...and this room's own page. Nothing in the chapter shares x640 y0, so
       there is no ordering rule between this line and the two above either. */
    texmgr_upload(arms_tex);
}

/* ---- THE EAST DOOR ---------------------------------------------------------
   x=1293, z[-107,107], y[-400,0] — in the octagon's east face, and the ONLY door
   this room has. Back into the Tomb, through the door that room's header lists
   as "west, not built".

   A door in the YZ plane at fixed X, approached from -X (wall 2 runs x=1293 with
   nx = -4095, so the walkable side is -X), so TEXT_PLANE_YZ with mirror=1 and
   the sign 11 units proud of the wall along -X. That is the same pairing the
   Tomb's east door takes; the TOMB'S WEST DOOR, the far side of this one, takes
   the OPPOSITE one (mirror=0, +11 on X), because the player stands on the other
   side of that wall. Getting the pair the wrong way round is how a sign comes
   out backwards, and it is the one thing about a new door that cannot be read
   off the mesh — it comes off the wall's inward normal in the collision .c.

   >>> THE READING AXIS FOR A YZ SIGN IS Z, so the -200 door_draw_string_3d wants
   goes on the Z argument and not on the X one. <<< Putting it on X instead does
   not fail loudly: the line simply sits 200 units inside the wall and is
   half-swallowed by it. */
#define ROA_EAST_X           1293
#define ROA_EAST_Z              0     /* the art spans z[-107,107] */
#define ROA_EAST_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define ROA_TEXT_RADIUS      1200
#define ROA_FADE_NEAR         800
#define ROA_TRIGGER_RADIUS    500

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int east_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void room_of_arms_arm(void) {
    east_circle_prev = circle_held();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts —
   hatch_puzzle_update()'s rule, for its reason. */
int room_of_arms_east_door_triggered(int lock) {
    int held = circle_held();
    int just = held && !east_circle_prev;
    int32_t dx, dz, xz;
    east_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - ROA_EAST_X;
    dz = cam_z - ROA_EAST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= ROA_TRIGGER_RADIUS) return 0;
    if (!interact_facing(ROA_EAST_X, ROA_EAST_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within ROA_FADE_NEAR, gone by ROA_TEXT_RADIUS. */
static void roa_east_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - ROA_EAST_X;
    int32_t dz = cam_z - ROA_EAST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= ROA_TEXT_RADIUS) return;

    if (xz > ROA_FADE_NEAR) {
        int range = ROA_TEXT_RADIUS - ROA_FADE_NEAR;
        int prog  = xz - ROA_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* door_draw_string_3d adds 200 to the reading axis before centring, and a YZ
       sign reads along Z — hence the -200 on that argument. mirror=1: a
       YZ-plane door approached from -X. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        ROA_EAST_X - 11, ROA_EAST_TEXT_Y, ROA_EAST_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void room_of_arms_spawn_east(void) {
    /* Clear of the wall push radius so the player is not shoved on their first
       frame, and facing -X — the direction of travel through the door, looking
       west across the chamber at the screen wall and its gap.

       (1073,0) also clears both of the octagon's east chamfers by over 500, so
       the first frame is nowhere near a push boundary in any direction; the
       diagonals are the walls a spawn measured only against the door's own wall
       would miss. */
    cam_x   = ROA_EAST_X - (ROA_WALL_RADIUS + 25);
    cam_y   = ROA_EYE_Y;
    cam_vy  = 0;
    cam_z   = ROA_EAST_Z;
    cam_rot = 3072;
    room_of_arms_arm();
}

void room_of_arms_init(void) {
    room_of_arms_collision_init(&current_collision_room);
    /* The DRAWN ceiling, read off the VISUAL mesh and not only off the collision
       proxy: the vaulting over the whole octagon is at y=-800, which is also
       where every one of the proxy's thirteen walls stops, so the two agree and
       the honest number is available. That is the Tomb's and the Incinerator
       Room's headroom, half the Up Down Maze's; anything hung from it has to be
       authored against 800, not 1800. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(ROA_WALL_RADIUS);

    roa_floor_zones_init();
    room_of_arms_spawn_east();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every one of their collide routines
       — so an instance left over from another room would block the player
       invisibly anywhere its coordinates fall inside this room's bounds
       (x[-1293,1293] z[-1293,1198]).

       >>> AND THIS ROOM'S BOUNDS CONTAIN BOTH OF THE CHAPTER'S SCONCES, not one
       of them. <<< The Catacombs Entry's pair is at (±595,200) and this
       footprint is centred on the origin, so BOTH land in it — in the open floor
       between the door and the screen wall, which is the whole of the walkable
       room. Neither bites, because sconces_collide() gates on
       s->area != current_area and both instances are tagged
       STATE_CATACOMBS_ENTRY; the two arrays that are NOT gated, the save point
       and the dresser, have nothing in Chapter 3 that lands in here. So this is
       still the cheap guarantee the Tomb's four calls are, and not a fix — but
       where that room's margin was one prop moving, this room has no margin at
       all: anything ever placed in the Catacombs Entry within 1293 of ITS origin
       would sit in this room too. That is the case Mistake 5 in
       tools/ADDING_A_ROOM.txt is about.

       Safe to clear: catacombs_entry_init() re-places all four on every entry to
       that room, and this room places none of them. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* Resolve the view distance with no ease: the first frame in the room shows
       whatever the player walked in holding. */
    roa_view_resolve(1);
}

/* ---- The mesh --------------------------------------------------------------
   The standard room draw loop, copied verbatim from src/tomb.c apart from the
   identifiers — and that file took it verbatim from src/incinerator_room.c,
   which took it from src/up_down_maze.c, which took it from
   src/catacombs_entry.c, which has been through
   tools/DIAGNOSING_FRAME_RATE.txt. Like both of theirs it carries no
   render_light_dist() discount, because no light is placed here; put it back in
   BOTH places at once the day this room gets one — the cull's discount and the
   shading's must match, or a lit poly survives the cull and is then shaded as
   though it had not been, i.e. drawn in the clear colour, a hole.

   Do not redesign the rest: the culling, the flat-poly OT sorting, the fog maths
   and the packet-overflow guards are all load-bearing (STEP 1). */
static void draw_room_of_arms_smd(RenderContext *ctx) {
    if (!roa_smd) return;

    uint8_t *p = (uint8_t *)roa_smd->p_prims;
    int i, n = roa_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). None of them can change
       while a frame is being queued: the two trig lookups would otherwise be a
       pair of SDK calls for every primitive that passed the distance cull, the
       cull distance would be re-read all 936 times, and buf_end is a double
       indirection through ctx->active_buffer, which a draw cannot change. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = roa_fog_far;   /* resolved by roa_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< Six sequential
           bytes carry this primitive's first vertex X/Z and its stride, which is
           everything both cheap tests below need AND everything the walk needs to
           advance — so a rejected primitive never touches the SMD header, the
           vertex index array or the vertex array. See roa_build_cull_keys. */
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
        SVECTOR *v0 = &roa_smd->p_verts[vi[0]];
        SVECTOR *v1 = &roa_smd->p_verts[vi[1]];
        SVECTOR *v2 = &roa_smd->p_verts[vi[2]];

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

        /* >>> THIS TABLE IS NON-EMPTY IN THIS ROOM, WHICH IT WAS NOT BEFORE
           THE SUBDIVISION. <<< Cutting the large faces up produced 30
           TRIANGLE-SHAPED QUADS — quads with three collinear corners, which
           gte_nclip reports a winding of zero for and the test below would
           therefore throw away from BOTH sides. gen_room_of_arms_tex_map.py
           detects them and flags them here; the count it prints is the number
           to watch after any re-export, because a face that silently stops
           being flagged comes back as a hole. */
        int nocull = (i < ROOM_OF_ARMS_PRIM_COUNT) && room_of_arms_nocull[i];
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
            v3 = &roa_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner, not their average, so
           floors stay behind whatever stands on them (see render.h). Here that
           is the chamber's floor, its vault AND the 52 arms polys, which lie
           within 110 of the pocket floor and are exactly the case this rule is
           for: sorted by their average they would trade places with the slab
           under them as the camera moved at the gap. Like the Tomb and unlike
           the Up Down Maze, no surface in this room is a floor on one side and a
           ceiling on the other. */
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
        int32_t fog = dist < roa_fog_near ? roa_fog_near : (dist > roa_fog_far ? roa_fog_far : dist);
        int32_t fog_factor = ((roa_fog_far - fog) << 8) / (roa_fog_far - roa_fog_near);

        uint8_t tex_idx = (i < ROOM_OF_ARMS_PRIM_COUNT) ? room_of_arms_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < ROOM_OF_ARMS_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + ROA_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + ROA_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + ROA_FOG_B * (256 - fog_factor)) >> 8);

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

void room_of_arms_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    roa_view_resolve(0);

    /* Anything else that fogs in this room follows the debug view distance when
       one is selected, so levels 6/7 change what the room LOOKS like
       consistently rather than only where the mesh stops. */
    g_fog_near = roa_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : roa_fog_far;

    /* Background in the SAME colour the fog saturates to, as the CLEAR COLOUR
       and not as a primitive: the draw environments carry isbg=1, so DrawOTagEnv
       has already filled the whole framebuffer before the first poly is drawn,
       and a full-screen TILE on top would be a second 77,000-pixel fill every
       frame for the colour alone. Wrong turn #3 in
       tools/DIAGNOSING_FRAME_RATE.txt. */
    render_set_clear_colour(ctx, ROA_FOG_R, ROA_FOG_G, ROA_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All three of this room's textures sit at page-top (Voff 0), so one window
       serves them (tools/VRAM_MAP.txt) — including the arms, whose page was
       chosen on that requirement among others (x640 y0; see the texture block
       above). Voff >= 128 would have made the field sample whatever sits above
       it in the page, silently and only in this room. */
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

    if (exp != DBG_EXP_NO_MESH) draw_room_of_arms_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE DOOR SIGN AND NOTHING ELSE, which is the case STEP
       3D of tools/DIAGNOSING_FRAME_RATE.txt was actually written about. <<< This
       room holds no props and world.c seeds it with no enemies, so the one
       string over the east door is the whole of its entity cost — and that is
       not the negligible half of the reading it sounds like. Reception's frame
       turned out to be its SIGNAGE and not its mesh, because door_draw_string_3d
       has no facing test and queues every glyph in full with the player's back
       to it. Against a 578-primitive mesh one 26-glyph string is a smaller
       share of this frame than it was at 290, but it is still the whole of this
       room's entity cost and still worth splitting out. Read D at levels 1, 4
       and 8 before concluding anything about this room.

       The enemy draws below still run, and they are still free: the loop skips
       every instance whose area is not current_area, so an empty room queues
       nothing. They are here so that placing a crawler or a lumberer in this
       room later is a line in world.c and not an edit to this file. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        roa_east_door_text(ctx);
        /* BOTH CHAPTER 3 ENEMIES, and both are drawn in all five of its rooms
           whether or not world.c places one here. That is deliberate and it is
           what "either enemy may go in any Catacombs room" actually costs: the
           area tag makes an absent enemy free (the loop skips every instance
           whose area is not current_area), and a placement then starts drawing
           without this file having to be touched again.

           They were mutually exclusive until each got a VRAM block of its own —
           see src/lumberer.h and tools/VRAM_MAP_CATACOMBS.txt.

           Both sheets sit at Voff 128 (VRAM y=128), so unlike this room's mesh
           art they cannot live under the 128 texture window set at the top of
           this function: each is handed that window to RESTORE after its sprite
           and draws itself unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
    }
}
