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
#include "tomb.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "tomb_mesh_collision.h"
#include "tomb_tex_map.h"
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

/* The Tomb — see tomb.h for the layout and the door list. */

static SMD  *tomb_smd  = NULL;
static void *tomb_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY, unchanged: cull and fog-far are equal (so nothing is
   dropped until the fog has already faded it into the background), the base is
   scaled by what the player is carrying, and the scale eases rather than jumping.

   >>> 450/1600 — THE BURIAL HALL'S AND THE INCINERATOR ROOM'S, NOT THE UP DOWN
   MAZE'S, AND THIS IS THE ROOM WHERE THAT CHOICE HAS TO BE ARGUED. <<< Every
   other Chapter 3 room picks between those two on a difference you can see from
   the doorway; this one cannot. It has nine blocks on a 600 grid, which is the
   Up Down Maze's pitch exactly, and that room pulled its base in to 1300 for a
   reason that sounds like it applies here: a longer sight line hands the player
   a plan of the room from the doorway.

   It does not apply, because the two rooms hide different things. The maze's
   plan IS its puzzle — which gaps connect, which do not — and seeing it from the
   door is seeing the answer. This grid is REGULAR: three rows by three columns,
   600 aisles, no dead ends and nothing to solve. The player reads the whole
   arrangement from the first aisle whatever the fog is set to, so pulling it in
   would cost the room its scale and conceal nothing. What it should feel like is
   the burial hall, which is what it is, so it gets the burial hall's numbers.

   1600 IS ALSO WELL SHORT OF ANY WALL FROM ANYWHERE. The chamber is 4200 square
   and the east door is at z=1500, so the far corner is over 4400 away and the
   long aisles run the full 4200. The room is never on screen at once.

   THE HELLUMINATOR IS THE WAY ACROSS IT. Raising the lantern and then burning it
   each add 50% of the base, so the three distances are 1600, 2400 and 3200 —
   and even lit that is 1000 short of the far wall down the longest aisle. The
   chapter's bargain, and deliberately still not enough to see the room whole.

   AND IT IS ALSO THIS ROOM'S FRAME BUDGET. `cull` and fog-far being equal means
   these numbers decide how much MESH is walked, transformed and queued. 936
   primitives here against the Incinerator Room's 708 and the Up Down Maze's
   1990 — the middle of the chapter, at the chapter's longest sight line, and the
   nine blocks occlude nothing the cull knows about. Anything that lengthens
   these has to be measured BURNING, not at rest (STEP 3J of
   tools/DIAGNOSING_FRAME_RATE.txt). */
#define TOMB_BASE_FOG_NEAR   450
#define TOMB_BASE_FOG_FAR   1600

#define TOMB_VIEW_UNIT        256
#define TOMB_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define TOMB_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define TOMB_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t tomb_view     = TOMB_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t tomb_fog_near = TOMB_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t tomb_fog_far  = TOMB_BASE_FOG_FAR;

static int32_t tomb_view_target(void) {
    int32_t s = TOMB_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += TOMB_VIEW_HELL_BONUS;
        /* Only asked INSIDE the equipped test: helluminator_burning() cannot be
           true for an unequipped lantern, but nesting it says so rather than
           relying on the weapon layer to keep clearing it. */
        if (helluminator_burning()) s += TOMB_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry: there is no previous frame to ease from
   and walking in with the lantern already up must not open the room out over the
   first half second. */
static void tomb_view_resolve(int snap) {
    int32_t target = tomb_view_target();
    if (snap) {
        tomb_view = target;
    } else if (tomb_view < target) {
        tomb_view += TOMB_VIEW_RATE;
        if (tomb_view > target) tomb_view = target;
    } else if (tomb_view > target) {
        tomb_view -= TOMB_VIEW_RATE;
        if (tomb_view < target) tomb_view = target;
    }
    tomb_fog_near = (TOMB_BASE_FOG_NEAR * tomb_view) >> 8;
    tomb_fog_far  = (TOMB_BASE_FOG_FAR  * tomb_view) >> 8;
}

/* Underground, so the same near-black-with-a-cold-lift the rest of the chapter
   uses, and for the same reason (src/catacombs_entry.c): a fog that saturates to
   a true 0,0,0 makes the cull line invisible, which sounds ideal and is how you
   lose an hour to "the end of the aisle is missing". */
#define TOMB_FOG_R             7
#define TOMB_FOG_G             6
#define TOMB_FOG_B             9

/* Wall standoff. The chapter's 195, and here it is a real fit rather than a
   default: the aisles between the loculus blocks are 600 wide, so the player
   walks down the middle of a 210-unit channel. That is comfortable — wider than
   the Incinerator Room's alcove mouth, which clears by 10 — and it is why the
   garden rooms' 260 was not even considered: it would leave 80. */
#define TOMB_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. Used for the spawn only — apply_height
   settles cam_y every frame afterwards. */
#define TOMB_FLOOR_Y            0
#define TOMB_EYE_Y           (TOMB_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, SPANNING THE WHOLE FOOTPRINT — the simplest case in the
   game, and it is worth saying why the nine blocks do not complicate it.

   A floor zone answers "how high is the ground at this XZ", and the answer here
   is 0 everywhere the player can stand. The blocks are not standable: they are
   solid walls floor-to-vault (every one of their 36 proxy faces runs y[-800,0],
   see src/tomb_mesh_collision.c), so the player is pushed out of their
   footprints by the wall routine and never asks the height inside one. Cutting
   the zone up to avoid them would be describing a case that cannot arise.

   FLOOR_FLAT, not FLOOR_UPPER: nothing in this room sets
   player_on_upper_floor, because there is no upper floor to be on. And with one
   zone there is no ORDER to get wrong, which next door but one is the whole
   mechanism (the Up Down Maze's ten walkways over its catch-all). */
static void tomb_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -4199; floor_zones[i].max_x =    0;
    floor_zones[i].min_z =     0; floor_zones[i].max_z = 4200;
    floor_zones[i].y     = TOMB_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, AND THE ROOM OWNS NONE OF THEM.

     0 cobblestones        the outer walls, the floor and the vaulting
                           (clsd_drwr page, x384 y0)
     1 loculus             the nine blocks' burial niches — 138 of the 936 polys
                           (kchn_wl page,   x512 y0)
     2 catacomb inner door the three doorways in the outer walls
                           (opn_drwr page,  x832 y0)

   All three are registered by src/catacombs_entry.c, in TEXBANK_CATACOMBS, and
   all three sit at Voff 0, so the one 128 texture window set in tomb_draw serves
   them. This room therefore spends NOTHING: no texmgr registration, no permanent
   RAM, no VRAM page of its own. What it has instead is a call to each of that
   module's three NARROW uploaders — the conservatory_upload_con_tile pattern —
   and the narrow form matters for the usual reason (tools/ADDING_A_ROOM.txt
   STEP 3b): the Catacombs Entry's FULL uploader would also stamp the lamashtu
   tablet, the sconce and the oil dispenser, none of which this room draws.

   >>> THE LOCULUS UPLOADER IS NEW AND THE OTHER TWO ARE NOT. <<< cobble and the
   inner door already had one each, for the Up Down Maze and then the Incinerator
   Room; this is the first room besides the burial hall itself to draw the
   loculus, so catacombs_entry_upload_loculus() was added beside them rather than
   this room reaching for the full uploader. That is the cheaper of the two by
   one page and it keeps the rule the other two state: a borrower has no business
   putting back pages it never draws.

   >>> AND BORROWING IS NOT MERELY THRIFTY HERE, IT IS THE ONLY THING THAT WORKS.
   <<< Three registrations of the same three files would be three RAM copies of
   art the chapter already holds, at the same three VRAM addresses, and which
   pixels were up would then depend on whichever room's uploader ran last. One
   entry, one copy, one address. */
#define TOMB_TEX_COUNT 3

static uint16_t tex_tpage[TOMB_TEX_COUNT];
static uint16_t tex_clut[TOMB_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/incinerator_room.c, which took it from src/up_down_maze.c,
   which took it from src/catacombs_entry.c, which has been through that
   document.

   The keys go in the SHARED arena (src/cull_arena.h) and are built on the same
   call that reloads the mesh they describe, which is the one rule that file has.
   A rejected primitive is then ONE sequential 6-byte read and never addresses the
   SMD header, the index array or the vertex array.

   NO BOX KEY AND NO SIDE-PLANE CULL, for the reason the Incinerator Room, the Up
   Down Maze, the Catacombs Entry, the Greenhouse, Maze One, Reception and the
   Master Bedroom all record: cull_boxes pays for itself only where a side-plane
   test would otherwise chase v1..v3 per surviving primitive, and there is no
   such test here to feed. "The room is open so it would cull a lot" is wrong
   turn #2 in DIAGNOSING_FRAME_RATE.txt and it has now been the tempting wrong
   answer seven times — and this room, an open square with the chapter's longest
   sight line, is the most tempting of the seven. If a METER says it is
   different, that is the case for adding one, with a box key under it and an
   offline hole sweep beside it. */
static int tomb_key_count = 0;

static void tomb_build_cull_keys(void) {
    tomb_key_count = 0;
    if (!tomb_smd) return;
    uint8_t *p = (uint8_t *)tomb_smd->p_prims;
    int i, n = tomb_smd->n_prims;
    if (n > TOMB_PRIM_COUNT) n = TOMB_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &tomb_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    tomb_key_count = n;
}

void tomb_load_geometry(void) {
    tomb_buff = room_arena_load("\\TEXCTCMB\\TOMB.SMD;1");
    tomb_smd  = tomb_buff ? smdInitData(tomb_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    tomb_build_cull_keys();
}

/* STARTUP, and it does nothing at all at runtime: three compile-time headers,
   not one registration and not one CD read. The registrations this room draws
   through belong to src/catacombs_entry.c and are already deferred into
   TEXBANK_CATACOMBS.

   NO texmgr_set_bank() HERE, and that is correct rather than an omission: that
   call tags the registrations a module MAKES, and this module makes none. Its
   place in the bank graph is established by the three uploader calls below,
   which py tools/check_tex_banks.py walks. */
void tomb_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, LOCULUS);
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
void tomb_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_loculus();
    catacombs_entry_upload_inner_door();
}

/* ---- THE EAST DOOR ---------------------------------------------------------
   x=0, z[1400,1600], y[-400,0] — in the outer east wall, and the only one of
   this room's three drawn doors that goes anywhere. Back into the Incinerator
   Room, through the door that room's header lists as "west, not built".

   A door in the YZ plane at fixed X, approached from -X (wall 39 runs x=0 with
   nx = -4096, so the walkable side is -X), so TEXT_PLANE_YZ with mirror=1 and
   the sign 11 units proud of the wall along -X. That is the pairing
   src/catacombs_entry.c's inner door uses and the one the Incinerator Room's
   conveyor sign uses; the room on the far side of THIS wall takes the opposite
   one, because it stands on the other side of it.

   >>> THE READING AXIS FOR A YZ SIGN IS Z, so the -200 door_draw_string_3d wants
   goes on the Z argument and not on the X one. <<< Putting it on X instead does
   not fail loudly: the line simply sits 200 units inside the wall and is
   half-swallowed by it. */
#define TOMB_EAST_X              0
#define TOMB_EAST_Z           1500     /* the art spans z[1400,1600] */
#define TOMB_EAST_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define TOMB_TEXT_RADIUS      1200
#define TOMB_FADE_NEAR         800
#define TOMB_TRIGGER_RADIUS    500

/* ---- THE WEST DOOR ---------------------------------------------------------
   x=-4200, z[2600,2800], y[-400,0] — in the outer west wall, and the second of
   this room's three drawn doors to be wired up. Through it is the ROOM OF ARMS
   (src/room_of_arms.h), the chapter's fifth room.

   >>> ITS MIRROR IS THE OPPOSITE OF THE EAST DOOR'S, AND THAT IS THE WHOLE
   REASON THIS BLOCK IS NOT A COPY OF THAT ONE. <<< Wall 38 runs x=-4199 with
   nx = +4096, so the walkable side is +X: the player stands EAST of this door
   and approaches it from +X, where the east door is approached from -X. A
   YZ-plane door approached from +X takes mirror=0 and its sign goes 11 units
   proud of the wall along +X. Copy the east door's mirror=1 and -11 here and the
   text comes out backwards AND buried in the wall, which are two symptoms of one
   mistake. The Room of Arms' east door, the far side of this wall, takes the
   mirror=1 form for the same reason in reverse.

   THE READING AXIS IS STILL Z, so the -200 door_draw_string_3d wants still goes
   on the Z argument. That part does not change with the side. */
#define TOMB_WEST_X         (-4199)
#define TOMB_WEST_Z           2700     /* the art spans z[2600,2800] */
#define TOMB_WEST_TEXT_Y      (-186)   /* eye level on the y=0 floor */

/* Circle edge-detect, one seed per door. Seeded "held" by the arm below so a
   press carried in through the transition cannot fire on the arrival frame. */
static int east_circle_prev = 1;
static int west_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void tomb_arm(void) {
    int held = circle_held();
    east_circle_prev = held;
    west_circle_prev = held;
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts —
   hatch_puzzle_update()'s rule, for its reason. Both doors go through this one
   body, which is STEP 5's instruction for a room with two or more of them
   (tools/ADDING_A_ROOM.txt): the per-door state is the (x, z, &prev) triple and
   nothing else, so the two cannot drift apart. */
static int door_triggered(int lock, int32_t door_x, int32_t door_z, int *prev) {
    int held = circle_held();
    int just = held && !*prev;
    int32_t dx, dz, xz;
    *prev = held;
    if (lock || !just) return 0;
    dx = cam_x - door_x;
    dz = cam_z - door_z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= TOMB_TRIGGER_RADIUS) return 0;
    if (!interact_facing(door_x, door_z)) return 0;
    return 1;
}

int tomb_east_door_triggered(int lock) {
    return door_triggered(lock, TOMB_EAST_X, TOMB_EAST_Z, &east_circle_prev);
}

int tomb_west_door_triggered(int lock) {
    return door_triggered(lock, TOMB_WEST_X, TOMB_WEST_Z, &west_circle_prev);
}

/* A door's floating sign. Same shape as every other sign in the game: opaque
   within TOMB_FADE_NEAR, gone by TOMB_TEXT_RADIUS.

   `standoff` is the sign's offset off the wall toward the player and `mirror` is
   which way the glyphs read, and THE TWO ALWAYS AGREE: -11 with mirror=1 for a
   door approached from -X, +11 with mirror=0 for one approached from +X. They
   are passed as a pair rather than derived here because there is no third case
   in this room and spelling them out at each call site is what makes the two
   doors' difference visible in tomb_draw(). */
static void tomb_door_text(RenderContext *ctx, int32_t door_x, int32_t door_z,
                           int32_t text_y, int32_t standoff, int mirror) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - door_z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= TOMB_TEXT_RADIUS) return;

    if (xz > TOMB_FADE_NEAR) {
        int range = TOMB_TEXT_RADIUS - TOMB_FADE_NEAR;
        int prog  = xz - TOMB_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* door_draw_string_3d adds 200 to the reading axis before centring, and a YZ
       sign reads along Z — hence the -200 on that argument. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        door_x + standoff, text_y, door_z - 200,
                        50, 255, 50, fade, mirror, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void tomb_spawn_west(void) {
    /* Arriving from the Room of Arms. Clear of the wall push radius on the +X
       side — the walkable side of wall 38 — and facing +X, the direction of
       travel through the door, looking east down the aisle between the west wall
       and the western column of loculus blocks (x[-3600,-3000]). z=2700 is
       inside that column's 1800..2400 / 3000..3600 gap, so the walk in is down
       open floor and not into a block face. */
    cam_x   = TOMB_WEST_X + (TOMB_WALL_RADIUS + 25);
    cam_y   = TOMB_EYE_Y;
    cam_vy  = 0;
    cam_z   = TOMB_WEST_Z;
    cam_rot = 1024;
    tomb_arm();
}

void tomb_spawn_east(void) {
    /* Clear of the wall push radius so the player is not shoved on their first
       frame, and facing -X — the direction of travel through the door, looking
       west down the middle aisle between the second and third rows of blocks. */
    cam_x   = TOMB_EAST_X - (TOMB_WALL_RADIUS + 25);
    cam_y   = TOMB_EYE_Y;
    cam_vy  = 0;
    cam_z   = TOMB_EAST_Z;
    cam_rot = 3072;
    tomb_arm();
}

void tomb_init(void) {
    tomb_collision_init(&current_collision_room);
    /* The DRAWN ceiling, read off the VISUAL mesh and not only off the collision
       proxy: the vaulting over the whole chamber is at y=-800, which is also
       where the outer walls' and the blocks' proxy faces stop, so the two agree
       and the honest number is available. That is the Incinerator Room's
       headroom, half the Up Down Maze's; anything hung from it has to be
       authored against 800, not 1800. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(TOMB_WALL_RADIUS);

    tomb_floor_zones_init();
    tomb_spawn_east();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every one of their collide routines
       — so an instance left over from another room would block the player
       invisibly anywhere its coordinates fall inside this room's bounds
       (x[-4199,0] z[0,4200]).

       >>> AND THIS IS THE FIRST CHAPTER 3 ROOM WHOSE BOUNDS ACTUALLY CONTAIN
       ONE. <<< The Catacombs Entry's two sconces are at (±595,200), and
       (-595,200) is inside this footprint — in the aisle just inside the east
       door. It does not bite, because sconces_collide() gates on
       s->area != current_area and that instance is tagged STATE_CATACOMBS_ENTRY;
       the two arrays that are NOT gated, the save point and the dresser, have
       nothing in Chapter 3 that lands in here (that room's save point is at
       (4606,2106), off the +X side). So this is still the cheap guarantee the
       Incinerator Room's four calls are, and not a fix — but the margin is now
       one prop moving rather than a whole room's width, which is the case
       Mistake 5 in tools/ADDING_A_ROOM.txt is about.

       Safe to clear: catacombs_entry_init() re-places all four on every entry to
       that room, and this room places none of them. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* Resolve the view distance with no ease: the first frame in the room shows
       whatever the player walked in holding. */
    tomb_view_resolve(1);
}

/* ---- The mesh --------------------------------------------------------------
   The standard room draw loop, copied verbatim from src/incinerator_room.c apart
   from the identifiers — and that file took it verbatim from
   src/up_down_maze.c, which took it from src/catacombs_entry.c, which has been
   through tools/DIAGNOSING_FRAME_RATE.txt. Like both of theirs it carries no
   render_light_dist() discount, because no light is placed here; put it back in
   BOTH places at once the day this room gets one — the cull's discount and the
   shading's must match, or a lit poly survives the cull and is then shaded as
   though it had not been, i.e. drawn in the clear colour, a hole.

   Do not redesign the rest: the culling, the flat-poly OT sorting, the fog maths
   and the packet-overflow guards are all load-bearing (STEP 1). */
static void draw_tomb_smd(RenderContext *ctx) {
    if (!tomb_smd) return;

    uint8_t *p = (uint8_t *)tomb_smd->p_prims;
    int i, n = tomb_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). None of them can change
       while a frame is being queued: the two trig lookups would otherwise be a
       pair of SDK calls for every primitive that passed the distance cull, the
       cull distance would be re-read all 936 times, and buf_end is a double
       indirection through ctx->active_buffer, which a draw cannot change. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = tomb_fog_far;   /* resolved by tomb_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< Six sequential
           bytes carry this primitive's first vertex X/Z and its stride, which is
           everything both cheap tests below need AND everything the walk needs to
           advance — so a rejected primitive never touches the SMD header, the
           vertex index array or the vertex array. See tomb_build_cull_keys. */
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
        SVECTOR *v0 = &tomb_smd->p_verts[vi[0]];
        SVECTOR *v1 = &tomb_smd->p_verts[vi[1]];
        SVECTOR *v2 = &tomb_smd->p_verts[vi[2]];

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

        int nocull = (i < TOMB_PRIM_COUNT) && tomb_nocull[i];
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
            v3 = &tomb_smd->p_verts[vi[3]];
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
           floors stay behind whatever stands on them (see render.h). Here that is
           the chamber's floor and its vault and nothing else: like the
           Incinerator Room and unlike the Up Down Maze, no surface in this room
           is a floor on one side and a ceiling on the other. */
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
        int32_t fog = dist < tomb_fog_near ? tomb_fog_near : (dist > tomb_fog_far ? tomb_fog_far : dist);
        int32_t fog_factor = ((tomb_fog_far - fog) << 8) / (tomb_fog_far - tomb_fog_near);

        uint8_t tex_idx = (i < TOMB_PRIM_COUNT) ? tomb_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < TOMB_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + TOMB_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + TOMB_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + TOMB_FOG_B * (256 - fog_factor)) >> 8);

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

void tomb_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    tomb_view_resolve(0);

    /* Anything else that fogs in this room follows the debug view distance when
       one is selected, so levels 6/7 change what the room LOOKS like
       consistently rather than only where the mesh stops. */
    g_fog_near = tomb_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : tomb_fog_far;

    /* Background in the SAME colour the fog saturates to, as the CLEAR COLOUR
       and not as a primitive: the draw environments carry isbg=1, so DrawOTagEnv
       has already filled the whole framebuffer before the first poly is drawn,
       and a full-screen TILE on top would be a second 77,000-pixel fill every
       frame for the colour alone. Wrong turn #3 in
       tools/DIAGNOSING_FRAME_RATE.txt. */
    render_set_clear_colour(ctx, TOMB_FOG_R, TOMB_FOG_G, TOMB_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All three of this room's textures sit at page-top (Voff 0), so one window
       serves them (tools/VRAM_MAP.txt). */
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

    if (exp != DBG_EXP_NO_MESH) draw_tomb_smd(ctx);

    /* >>> LEVEL 8 NOW REMOVES A LUMBERER AND *TWO* SIGNS. <<< This used to say
       the room held no props and no enemies and that the east door's floating
       string was all there is. It has a lumberer now (src/lumberer.h), walking
       the aisle between the middle and eastern block columns, and a second sign
       over the west door since the Room of Arms was built behind it — so the D
       reading at levels 1, 4 and 8 is splitting this room's frame between
       something and something else rather than between the mesh and one string,
       which is the case STEP 3D of tools/DIAGNOSING_FRAME_RATE.txt was written
       about (Reception's frame turned out to be its SIGNAGE and not its mesh,
       because door_draw_string_3d has no facing test and queues every glyph in
       full with the player's back to it — and with two signs in a 4200 room
       there is always at least one of them behind the camera). */
    if (exp != DBG_EXP_NO_ENTITIES) {
        tomb_door_text(ctx, TOMB_EAST_X, TOMB_EAST_Z, TOMB_EAST_TEXT_Y,
                       -11, 1);   /* east: approached from -X */
        tomb_door_text(ctx, TOMB_WEST_X, TOMB_WEST_Z, TOMB_WEST_TEXT_Y,
                       +11, 0);   /* west: approached from +X */
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
