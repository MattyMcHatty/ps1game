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
#include "up_down_maze.h"
#include "collision.h"
#include "up_down_maze_mesh_collision.h"
#include "up_down_maze_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */
#include "crawler.h"            /* Chapter 3's monster: five on the lower maze  */

/* Up Down Maze — see up_down_maze.h for the layout and the two-storey note. */

static SMD  *up_down_maze_smd  = NULL;
static void *up_down_maze_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CATACOMBS ENTRY'S NUMBERS AND ITS MACHINERY, deliberately unchanged: cull
   and fog-far are equal (so nothing is dropped until the fog has already faded
   it into the background), the base is scaled by what the player is carrying,
   and the scale eases rather than jumping.

   1600 IS ALSO THE RIGHT BASE FOR THIS ROOM ON ITS OWN TERMS, which is worth
   saying because "copied from next door" is not a reason. The blocks are on a
   600 grid and the room is 4200 long; at 1600 the player sees three or four
   cells of corridor and no further, which is the whole point of a maze. Up on
   the walkways the drop-offs are the hazard and 1600 still shows the next
   junction. Lengthening it would hand the player a plan of the maze from the
   doorway.

   THE HELLUMINATOR IS THE WAY ROUND IT, exactly as in the entry chamber: raising
   the lantern is +50% and burning it is +50% more, so a burst buys a 3200 sight
   line down one corridor and costs oil. That is the chapter's bargain, and this
   is the first room built around it. */
#define UDM_BASE_FOG_NEAR   450
#define UDM_BASE_FOG_FAR   1600

#define UDM_VIEW_UNIT        256
#define UDM_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define UDM_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define UDM_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t udm_view     = UDM_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t udm_fog_near = UDM_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t udm_fog_far  = UDM_BASE_FOG_FAR;

static int32_t udm_view_target(void) {
    int32_t s = UDM_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += UDM_VIEW_HELL_BONUS;
        /* Only asked INSIDE the equipped test: helluminator_burning() cannot be
           true for an unequipped lantern, but nesting it says so rather than
           relying on the weapon layer to keep clearing it. */
        if (helluminator_burning()) s += UDM_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry: there is no previous frame to ease from
   and walking in with the lantern already up must not open the room out over the
   first half second. */
static void udm_view_resolve(int snap) {
    int32_t target = udm_view_target();
    if (snap) {
        udm_view = target;
    } else if (udm_view < target) {
        udm_view += UDM_VIEW_RATE;
        if (udm_view > target) udm_view = target;
    } else if (udm_view > target) {
        udm_view -= UDM_VIEW_RATE;
        if (udm_view < target) udm_view = target;
    }
    udm_fog_near = (UDM_BASE_FOG_NEAR * udm_view) >> 8;
    udm_fog_far  = (UDM_BASE_FOG_FAR  * udm_view) >> 8;
}

/* Underground, so the same near-black-with-a-cold-lift the entry chamber uses,
   and for the same reason (src/catacombs_entry.c): a fog that saturates to a
   true 0,0,0 makes the cull line invisible, which sounds ideal and is how you
   lose an hour to "the end of the corridor is missing". */
#define UDM_FOG_R             7
#define UDM_FOG_G             6
#define UDM_FOG_B             9

/* Wall standoff. The DEFAULT 195, like the Catacombs Entry's and for a sharper
   version of its reason: the lower maze's corridors are 600 wide between block
   faces, so 195 leaves 210 to walk down. The garden rooms' 260 would leave 80
   and the maze would be unplayable.

   >>> AND IT DOES NOT HOLD THE PLAYER ON A WALKWAY. <<< Nothing does — the block
   sides are Y-gated to the storey below (see up_down_maze_mesh_collision.c) — so
   up top the walkways are walked right to the lip and the drop is a real one.
   That is the room. */
#define UDM_WALL_RADIUS     195

/* Standing eye on the UPPER floor (y=-1000), which is where the player arrives:
   less GROUND_FLOOR_Y and the 40-unit standoff apply_height applies. Used for
   the spawn and for the door's storey test only — apply_height settles cam_y
   every frame afterwards, and it has to, because a player who walks off a
   walkway falls 1000 units. */
#define UDM_UPPER_Y         (-1000)
#define UDM_EYE_Y           (UDM_UPPER_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ELEVEN: the ten block tops, then the lower floor as a catch-all under all of
   them.

   >>> THE ORDER IS LOAD-BEARING, and it is the DELIVERY AREA's rule rather than
   the Catacombs Entry's. <<< That room's seven zones meet on single lines and
   could be listed in any order; every zone here is stacked directly over the
   catch-all. apply_height() walks the list in order and skips any flat/upper
   zone whose surface is ABOVE the player, so a player up top matches their own
   block first and one down in the corridors falls through every block to the
   floor. Put the catch-all first and the whole upper maze becomes unstandable.

   THE BLOCK TOPS ARE FLOOR_UPPER, NOT FLOOR_FLAT, and the difference is one flag
   the rest of the engine reads: player_on_upper_floor. Nothing in this room acts
   on it yet, but it is what the entity height and collision routines ask, and a
   Chapter 3 enemy that walks these walkways will need it to be true.

   THEY ARE EXACTLY THE TWELVE FACES the collision generator found, less the two
   the lower floor is split into (z<-1500 and z>-1500), merged here into one
   rectangle over the whole footprint. The generator prints two of the block tops
   as y=-999; the SMX has all ten at -1000, and that is the value used — its
   floor list rounds, which is the same class of thing as the ramp-average trap
   in tools/ADDING_A_ROOM.txt STEP 4b. */
static void up_down_maze_floor_zones_init(void) {
    int i = 0;
    int b;

    /* The ten block tops: the UPPER maze, all at y=-1000. In the generator's own
       order, which begins with the landing inside the west door the player
       arrives on. */
    static const int16_t upper[10][4] = {   /* min_x, max_x, min_z, max_z */
        { -300,  300,  -899,   900 },   /* the WEST DOOR's landing          */
        {  300,  899,  -300,   300 },   /* the spur east off it             */
        {  899, 1499,  -300,  1500 },   /* the long north-south block       */
        {  899, 2099, -2100, -1499 },   /* the south block, under its door  */
        {  299, 2100,  1500,  3300 },   /* the big north-west plateau       */
        { 2100, 2700,  2100,  2700 },   /* the centre-north block           */
        { 2699, 3300,  -299,  2700 },   /* the long east block              */
        { 3299, 3899,  -300,   300 },   /* the east spur, at the east door  */
        { -300,  299,  2100,  2700 },   /* the north-west stub              */
        {  899, 1499,  3300,  3899 },   /* the north stub                   */
    };
    for (b = 0; b < 10; b++) {
        floor_zones[i].type  = FLOOR_UPPER;
        floor_zones[i].min_x = upper[b][0]; floor_zones[i].max_x = upper[b][1];
        floor_zones[i].min_z = upper[b][2]; floor_zones[i].max_z = upper[b][3];
        floor_zones[i].y     = UDM_UPPER_Y;
        i++;
    }

    /* The LOWER maze's floor: one plane under the whole room, at y=0. It is the
       catch-all and it must come LAST — see the note above. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -300;  floor_zones[i].max_x = 3900;
    floor_zones[i].min_z = -2100; floor_zones[i].max_z = 3900;
    floor_zones[i].y     = 0;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   TWO, AND THE ROOM OWNS NEITHER.

     0 cobblestones        the blocks, the floor and the vaulting (clsd_drwr page, x384 y0)
     1 catacomb inner door the six doorways in the outer walls    (opn_drwr page,  x832 y0)

   Both are registered by src/catacombs_entry.c, in TEXBANK_CATACOMBS, and both
   sit at Voff 0, so the one 128 texture window set in up_down_maze_draw serves
   them. This room therefore spends NOTHING: no texmgr registration (there were
   61 of 72 before it), no permanent RAM, no VRAM page of its own. What it has
   instead is a call to each of that module's two NARROW uploaders — the
   conservatory_upload_con_tile pattern — and the narrow form matters here for
   the usual reason (tools/ADDING_A_ROOM.txt STEP 3b): the Catacombs Entry's FULL
   uploader would also stamp the lamashtu tablet, the loculus, the sconce and the
   oil dispenser, none of which this room draws.

   >>> AND BORROWING IS NOT MERELY THRIFTY HERE, IT IS THE ONLY THING THAT WORKS.
   <<< Two registrations of the same two files would be two RAM copies of art the
   chapter already holds, at the same two VRAM addresses, and which pixels were up
   would then depend on whichever room's uploader ran last. One entry, one copy,
   one address. */
#define UP_DOWN_MAZE_TEX_COUNT 2

static uint16_t tex_tpage[UP_DOWN_MAZE_TEX_COUNT];
static uint16_t tex_clut[UP_DOWN_MAZE_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/catacombs_entry.c, which has been through that document. This
   room is the bigger of the two — 1990 primitives against 1073, the second
   largest mesh on the disc after Maze One's — so the reject path matters more
   here than it does next door, not less.

   The keys go in the SHARED arena (src/cull_arena.h) and are built on the same
   call that reloads the mesh they describe, which is the one rule that file has.
   A rejected primitive is then ONE sequential 6-byte read and never addresses the
   SMD header, the index array or the vertex array.

   NO BOX KEY AND NO SIDE-PLANE CULL, for the reason the Catacombs Entry, the
   Greenhouse, Maze One, Reception and the Master Bedroom all record: cull_boxes
   pays for itself only where a side-plane test would otherwise chase v1..v3 per
   surviving primitive, and there is no such test here to feed. "The room is open
   so it would cull a lot" is wrong turn #2 in DIAGNOSING_FRAME_RATE.txt and it
   has now been the tempting wrong answer five times. If a METER says this room
   is different, that is the case for adding one — with a box key under it and an
   offline hole sweep beside it. */
static int udm_key_count = 0;

static void udm_build_cull_keys(void) {
    udm_key_count = 0;
    if (!up_down_maze_smd) return;
    uint8_t *p = (uint8_t *)up_down_maze_smd->p_prims;
    int i, n = up_down_maze_smd->n_prims;
    if (n > UP_DOWN_MAZE_PRIM_COUNT) n = UP_DOWN_MAZE_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &up_down_maze_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    udm_key_count = n;
}

void up_down_maze_load_geometry(void) {
    up_down_maze_buff = room_arena_load("\\TEXCTCMB\\UPDNMAZE.SMD;1");
    up_down_maze_smd  = up_down_maze_buff
                        ? smdInitData(up_down_maze_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    udm_build_cull_keys();
}

/* STARTUP, and it does nothing at all at runtime: two compile-time headers, not
   one registration and not one CD read. The registrations this room draws
   through belong to src/catacombs_entry.c and are already deferred into
   TEXBANK_CATACOMBS.

   NO texmgr_set_bank() HERE, and that is correct rather than an omission: that
   call tags the registrations a module MAKES, and this module makes none. Its
   place in the bank graph is established by the two uploader calls below, which
   py tools/check_tex_banks.py walks. */
void up_down_maze_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   NO ORDERING RULE, for the Catacombs Entry's reason: nothing else uploads to
   these two pages while this chapter is resident, because nothing else in the
   game is reachable. If a texmgr entry is somehow not loaded, texmgr_upload is a
   no-op and the slot keeps whatever the previous room left in it — visibly wrong,
   and quietly, which is why area_bank_sync runs before this and not after. */
void up_down_maze_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
}

/* ---- THE WEST DOOR ---------------------------------------------------------
   x=-300, z[-100,100], y[-1400,-1000] — in the outer west wall, on the UPPER
   floor, and the only one of this room's six drawn doors that goes anywhere.
   Back to the Catacombs Entry's burial hall.

   A door in the YZ plane at fixed X, approached from +X (wall 23 runs x=-300
   with nx = +4096, so the walkable side is +X), so TEXT_PLANE_YZ with mirror=0
   and the sign 11 units proud of the wall along +X.

   >>> IT NEEDS A STOREY TEST, WHICH NO OTHER DOOR IN THE GAME DOES. <<< Every
   trigger in this engine measures Manhattan distance in XZ alone, which is
   correct everywhere the walkable surface is a function of XZ. Here it is not:
   the corridor at y=0 runs directly beneath this doorway at y=-1000, well inside
   500 in plan, so without the Y test below the player could open this door — and
   arrive in the Catacombs Entry — by standing in the maze underneath it. Both
   the trigger and the SIGN take the test, so the prompt does not hang in the air
   over a player walking the lower maze either. */
#define UDM_WEST_X           (-300)
#define UDM_WEST_Z              0      /* the art spans z[-100,100] */
#define UDM_WEST_TEXT_Y      (-1186)   /* eye level on the y=-1000 walkway */
#define UDM_TEXT_RADIUS      1200
#define UDM_FADE_NEAR         800
#define UDM_TRIGGER_RADIUS    500

/* HOW FAR OFF THE DOOR'S OWN STOREY STILL COUNTS AS "AT" IT. The two floors are
   1000 apart and the player's eye sits 189 above whichever one they are on, so
   any bound between roughly 250 and 750 separates them cleanly; 500 is the
   middle of that range, and the same number the trigger uses in plan. */
#define UDM_WEST_Y_REACH      500

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int west_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void up_down_maze_arm(void) {
    west_circle_prev = circle_held();
}

/* Is the player at the door — in plan AND on its storey? */
static int udm_west_door_in_reach(void) {
    int32_t dx = cam_x - UDM_WEST_X;
    int32_t dz = cam_z - UDM_WEST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int32_t dy;
    if (xz >= UDM_TRIGGER_RADIUS) return 0;
    /* cam_y is the EYE; standing on the door's own storey puts it at UDM_EYE_Y. */
    dy = cam_y - UDM_EYE_Y;
    if (dy < 0) dy = -dy;
    return dy < UDM_WEST_Y_REACH;
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts —
   hatch_puzzle_update()'s rule, for its reason. */
int up_down_maze_west_door_triggered(int lock) {
    int held = circle_held();
    int just = held && !west_circle_prev;
    west_circle_prev = held;
    if (lock || !just) return 0;
    if (!udm_west_door_in_reach()) return 0;
    if (!interact_facing(UDM_WEST_X, UDM_WEST_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within UDM_FADE_NEAR, gone by UDM_TEXT_RADIUS — plus the storey test above. */
static void udm_west_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - UDM_WEST_X;
    int32_t dz = cam_z - UDM_WEST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;
    int32_t dy;

    if (xz >= UDM_TEXT_RADIUS) return;
    dy = cam_y - UDM_EYE_Y;
    if (dy < 0) dy = -dy;
    if (dy >= UDM_WEST_Y_REACH) return;

    if (xz > UDM_FADE_NEAR) {
        int range = UDM_TEXT_RADIUS - UDM_FADE_NEAR;
        int prog  = xz - UDM_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* door_draw_string_3d adds 200 to the reading axis before centring, hence
       the -200. mirror=0: a YZ-plane door approached from +X. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        UDM_WEST_X + 11, UDM_WEST_TEXT_Y, UDM_WEST_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void up_down_maze_spawn_west(void) {
    /* Clear of the wall push radius so the player is not shoved on their first
       frame, and facing +X — the direction of travel through the door, looking
       east along the landing into the maze. */
    cam_x   = UDM_WEST_X + UDM_WALL_RADIUS + 25;
    cam_y   = UDM_EYE_Y;
    cam_vy  = 0;
    cam_z   = UDM_WEST_Z;
    cam_rot = 1024;
    up_down_maze_arm();
}

void up_down_maze_init(void) {
    up_down_maze_collision_init(&current_collision_room);
    /* The DRAWN ceiling, read off the VISUAL mesh and not off the collision
       proxy: the vaulting over the whole room is at y=-1800, which is also where
       the outer walls' proxy faces stop. One value is all collision_set_ceiling_y
       takes and nothing hangs from a ceiling here yet, so the real one is the
       honest answer — and note it is the LOWER maze's headroom, 1800 above its
       floor. Anything hung over the walkways has only the 800 above them; state
       that then, per placement. */
    collision_set_ceiling_y(-1800);
    collision_set_wall_radius(UDM_WALL_RADIUS);

    up_down_maze_floor_zones_init();

    up_down_maze_spawn_west();

    /* Save points, dressers, sconces and oil dispensers are global arrays that
       are neither room-swapped in world.c nor area-gated in every one of their
       collide routines, so an instance left over from the room next door would
       block the player invisibly anywhere its coordinates fall inside this
       room's bounds — and this room spans x[-300,3900] z[-2100,3900], which
       overlaps the Catacombs Entry's x[-750,4800] z[0,4792] almost entirely. The
       hall's save point at (4606,2106) falls outside; its two sconces at
       (±595,200) and its oil dispenser at (4798,503) do NOT, and the sconces
       would stand in the middle of the lower maze.

       Clearing is safe: catacombs_entry_init() re-places all four on every entry
       to that room, and this room places none of them. (Mistake 5 in
       tools/ADDING_A_ROOM.txt.) */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* Resolve the view distance with no ease: the first frame in the room shows
       whatever the player walked in holding. */
    udm_view_resolve(1);
}

/* ---- The mesh --------------------------------------------------------------
   The standard room draw loop, copied verbatim from src/catacombs_entry.c apart
   from the identifiers and the one thing that room has and this one does not:
   the braziers' render_light_dist() discount. There are no lights placed here,
   so the call would be a box-reject against an empty list on the hottest path in
   the room. Put it back in BOTH places at once the day this room gets a light —
   the cull's discount and the shading's must match, or a lit poly survives the
   cull and is then shaded as though it had not been, i.e. drawn in the clear
   colour, a hole.

   Do not redesign the rest: the culling, the flat-poly OT sorting, the fog maths
   and the packet-overflow guards are all load-bearing (STEP 1). */
static void draw_up_down_maze_smd(RenderContext *ctx) {
    if (!up_down_maze_smd) return;

    uint8_t *p = (uint8_t *)up_down_maze_smd->p_prims;
    int i, n = udm_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). None of them can change
       while a frame is being queued: the two trig lookups would otherwise be a
       pair of SDK calls for every primitive that passed the distance cull, the
       cull distance would be re-read all 1990 times, and buf_end is a double
       indirection through ctx->active_buffer, which a draw cannot change. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = udm_fog_far;   /* resolved by udm_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< Six sequential
           bytes carry this primitive's first vertex X/Z and its stride, which is
           everything both cheap tests below need AND everything the walk needs to
           advance — so a rejected primitive never touches the SMD header, the
           vertex index array or the vertex array. See udm_build_cull_keys. */
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
        SVECTOR *v0 = &up_down_maze_smd->p_verts[vi[0]];
        SVECTOR *v1 = &up_down_maze_smd->p_verts[vi[1]];
        SVECTOR *v2 = &up_down_maze_smd->p_verts[vi[2]];

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

        int nocull = (i < UP_DOWN_MAZE_PRIM_COUNT) && up_down_maze_nocull[i];
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
            v3 = &up_down_maze_smd->p_verts[vi[3]];
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
           floors stay behind whatever stands on them (see render.h). THIS ROOM
           HAS MORE OF THEM THAN ANY OTHER: every block top is a floor to the
           storey above it and a ceiling to the corridor beneath it. */
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
        int32_t fog = dist < udm_fog_near ? udm_fog_near : (dist > udm_fog_far ? udm_fog_far : dist);
        int32_t fog_factor = ((udm_fog_far - fog) << 8) / (udm_fog_far - udm_fog_near);

        uint8_t tex_idx = (i < UP_DOWN_MAZE_PRIM_COUNT) ? up_down_maze_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < UP_DOWN_MAZE_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + UDM_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + UDM_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + UDM_FOG_B * (256 - fog_factor)) >> 8);

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

void up_down_maze_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    udm_view_resolve(0);

    /* Anything else that fogs in this room follows the debug view distance when
       one is selected, so levels 6/7 change what the room LOOKS like
       consistently rather than only where the mesh stops. */
    g_fog_near = udm_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : udm_fog_far;

    /* Background in the SAME colour the fog saturates to, as the CLEAR COLOUR
       and not as a primitive: the draw environments carry isbg=1, so DrawOTagEnv
       has already filled the whole framebuffer before the first poly is drawn,
       and a full-screen TILE on top would be a second 77,000-pixel fill every
       frame for the colour alone. Wrong turn #3 in
       tools/DIAGNOSING_FRAME_RATE.txt. */
    render_set_clear_colour(ctx, UDM_FOG_R, UDM_FOG_G, UDM_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       Both of this room's textures sit at page-top (Voff 0), so one window serves
       them (tools/VRAM_MAP.txt). */
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

    if (exp != DBG_EXP_NO_MESH) draw_up_down_maze_smd(ctx);

    /* >>> LEVEL 8 NOW REMOVES FIVE CRAWLERS AND THE SIGN. <<< This used to say
       the sign was all there is, and that the room held no monsters because
       Chapter 3 had none. It has one now (src/crawler.h), and world.c seeds five
       of them along the lower maze — so the D reading at levels 1, 4 and 8 is
       finally splitting this room's frame between something and something else
       rather than between the mesh and one string. That is exactly the case
       STEP 3D of tools/DIAGNOSING_FRAME_RATE.txt was written about: Reception's
       frame turned out to be its signage and not its mesh. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        udm_west_door_text(ctx);
        /* THE CRAWLERS, and the texture window above is precisely the trap they
           have to be bracketed against: their sheet sits at VRAM y=128, i.e.
           Voff 128, so drawn under a 128-tall window its V would wrap mod-128
           and it would sample the wrong half of the page. This hands the
           renderer the window to put BACK after each sprite — the sprite itself
           goes down unmasked. Same contract the zombie and the spider have with
           the mansion rooms. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
    }
}
