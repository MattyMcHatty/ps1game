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
#include "sound.h"              /* SFX_SELECT / SFX_BACK — the examine's two beats */

/* The examine at the foot of this file reads Cross straight off the pad: it is
   the one button in this room that is not an interact tap, and camera.h's
   interact_tapped() covers Circle only. */
extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

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

/* The overhead examine (bottom of this file) borrows this room's fog for the
   length of its shot; declared here because roa_view_resolve() resolves it.
   The mix is 0 in normal play, 256 while the shot is held, and RIDES THE SAME
   EASE AS THE CAMERA on the way up and the way down — see the note in
   roa_view_resolve. */
static int roa_examine_fog_mix(void);

/* The shot's own two distances, and then HALF AS MUCH AGAIN on top of both.
   2200/4000 was the pair that made the FIELD read: the near plane out past the
   far corner of the arms so no part of the pattern is fogged, the far plane out
   past the far wall of the octagon so the chamber is drawn whole. It did nothing
   for the chamber, though — the octagon's far corner is 3818 (Manhattan, in XZ)
   from the camera, which at 2200/4000 lands nine tenths of the way down the fog
   ramp, so the walls the field sits inside came back near-black and the shot
   read as a lit rectangle floating in nothing.

   x1.5 is what opens THAT out: the same 3818 now sits at 3300/6000, i.e. four
   fifths of the way UP the ramp, and the octagon reads as a room with a floor of
   arms in it. The arms themselves do not change — they were already inside the
   near plane at 2200 and are still inside it at 3300 — so this is a change to
   what surrounds the pattern and not to the pattern.

   >>> AND IT COSTS NOTHING, which is only true because of where the numbers
   fall. <<< ROA_EX_FOG_FAR is also the CULL distance, so raising it normally
   means more mesh walked and queued — but 4000 was already past the 3818 that is
   the furthest anything in this room can be, so both values draw the same 578
   primitives and 6000 buys reach that does not exist. Anything that makes this
   room bigger makes that stop being true. */
#define ROA_EX_FOG_NEAR    2200
#define ROA_EX_FOG_FAR     4000
#define ROA_EX_FOG_BOOST    384   /* x1.5, in 1/256ths */

#define ROA_EX_FOG_NEAR_LIT  ((ROA_EX_FOG_NEAR * ROA_EX_FOG_BOOST) >> 8)
#define ROA_EX_FOG_FAR_LIT   ((ROA_EX_FOG_FAR  * ROA_EX_FOG_BOOST) >> 8)

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

    /* >>> THE EXAMINE OVERRIDES BOTH, AND IT HAS TO. <<< The overhead shot at
       the foot of this file sits 1500 above the pocket and 1314 (Manhattan, in
       XZ) from the far corner of the arms, which at the ROOM'S distances is
       most of the way into the fog and past nothing at all at rest — the whole
       field would arrive as the same near-black the player already sees through
       the gap. The point of the shot is that the pattern reads, so while it
       owns the camera the near plane goes out past the field's far corner
       (nothing in frame is fogged) and the far plane, which is ALSO the cull
       distance, goes out past the far wall of the octagon so the chamber draws
       whole around it — both of them then taken half as much again, which is
       what stops that chamber coming back as black (the note on the constants
       has the arithmetic). That is all 578 primitives for the duration; it is a
       static shot with no enemies in it, and it is the cheapest frame this room
       ever draws in every other respect.

       >>> AND IT IS MIXED IN, NOT SWITCHED ON. <<< Both of these are several
       times the room's own numbers, so flipping between them on the frame the
       rise starts and the frame the descent lands would be two hard pops — the
       chamber flashing bright under a camera that has not moved yet, and going
       black under one that has already stopped. The mix rides the camera's own
       ease-out instead, so the room opens up as the shot climbs and closes again
       as it comes down, and neither end has a frame in it that the move does not
       explain. */
    {
        int32_t mix = roa_examine_fog_mix();
        if (mix) {
            roa_fog_near += ((ROA_EX_FOG_NEAR_LIT - roa_fog_near) * mix) >> 8;
            roa_fog_far  += ((ROA_EX_FOG_FAR_LIT  - roa_fog_far)  * mix) >> 8;
        }
    }
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

/* Slot 1 by name, because the draw loop has to ask "is this an arm?" for the
   sort bias the overhead examine needs (see the OT note in the draw loop). */
#define ROA_TEX_ARMS           1

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
    /* ...and the gap examine, which arms its own two buttons and also undoes a
       shot left half-played by a debug jump out of this room (pitch flattened,
       player anchor released). */
    room_of_arms_examine_arm();
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

    /* >>> THE OVERHEAD EXAMINE BREAKS THE TIE THE FLAT-Y SORT LEAVES, AND ONLY
       WHILE IT IS UP. <<< Horizontal polys sort by their FARTHEST corner (the
       rule applied below, and the rule that keeps the arms off the slab for the
       player standing at the gap). Looked at from the floor that separates them
       cleanly. Looked at from 1500 DIRECTLY ABOVE it does not: depth becomes the
       drop to the floor, every arms poly rises to y=0 at its far corner, and
       that corner is then at EXACTLY the depth of the cobble slab underneath —
       the same otz bucket, decided by insertion order, which for 16 floor polys
       under 52 arms is a shot with arms missing out of the middle of it. Three
       buckets of bias is a hair of the ~370 the field sits at and is enough to
       put every arm in front of the floor it lies on.

       ZERO IN NORMAL PLAY, so nothing about the room the player walks around in
       changes; hoisted out of the loop like the four above it. */
    int32_t arms_bias = roa_examine_fog_mix() ? 3 : 0;

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
        if (arms_bias && i < ROOM_OF_ARMS_PRIM_COUNT &&
            room_of_arms_tex_map[i] == ROA_TEX_ARMS) {
            otz -= arms_bias;
            if (otz < 1) otz = 1;
        }

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
        /* ...and the second sign this room now has: the one in the gap in the
           screen wall. It draws itself away while the overhead shot is up. */
        room_of_arms_examine_text(ctx);
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

    /* SCREEN SPACE, so it goes last and OUTSIDE the entity gate: it is the only
       way out of the examine and a debug level that hides entities must not be
       able to strand the player in the shot. */
    room_of_arms_examine_prompt(ctx);
}

/* ============================================================================
   THE GAP EXAMINE — the overhead shot of the arms field
   ============================================================================

   THE ROOM'S ONE PIECE OF CONTENT IS BEHIND A WALL THE PLAYER CANNOT GET PAST,
   AND AT REST THEY SEE A THIRD OF IT. Screen wall segment 12 — (-144,424) to
   (-403,-155) — is the 634-wide opening that is walled in the proxy and not
   drawn in the mesh (see the header). The player stands at it, looks north-west
   into the pocket, and the fog eats the field about a third of the way in; the
   Helluminator buys the rest of it, and only at a flat, oblique, floor-level
   angle where 52 near-horizontal polys overlap into mush.

   So: a prompt in the gap, and a shot that answers the question the gap asks.
   Circle takes the camera 1500 straight up over the middle of the field and
   pitches it fully down, the arms read as the PATTERN they are laid out in, the
   log says so, and Cross puts the camera back on the player's shoulders.

   >>> THERE IS NO CEILING IN THIS ROOM, WHICH IS WHY THE SHOT IS POSSIBLE. <<<
   The header calls y=-800 the vault and collision_set_ceiling_y agrees, but that
   is where the WALLS STOP — there is not one flat poly at that height in "Room
   of Arms.smx" (196 flat polys, every one of them at y=0: the floor slabs and
   the arms). A camera 700 above the wall tops therefore looks straight down into
   an open box with nothing between it and the field. Re-export this room with an
   actual vault over it and this shot goes dark; the fix would then be to put the
   camera UNDER the new ceiling, which at this focal length cannot frame the
   field (see the arithmetic below) — i.e. it would have to become an angled shot
   and stop being the thing that was asked for. Worth knowing before adding a
   roof.

   ---- THE FRAMING, AND WHERE EVERY NUMBER CAME FROM ------------------------
   The 52 arms polys occupy x[-1293,148] z[107,1293] y[-70,0]. gte_SetGeomScreen
   is 256 on a 320x240 screen, so at distance D the half-field is 160D/256 =
   0.625D across and 120D/256 = 0.469D down. Pitched fully down (cam_pitch =
   1024 = 90deg) at yaw 0, screen X is world X and screen Y is world Z, and D is
   the drop from the camera to the floor.

     ACROSS (X)  the field is 1442 wide; centred at x=-572 the worst half is 721,
                 so D >= 721/0.625 = 1154.
     DOWN   (Z)  this is the binding one, because the screen is the short way up.
                 The camera does NOT sit on the field's z midpoint (700) — see
                 the yaw cull below — it sits at 660, so the worst half is
                 1293-660 = 633 and D >= 633/0.469 = 1350.

   D = 1500 takes the larger and adds 11%, which is the margin that keeps the
   outermost arms off the edge of the frame rather than bleeding through it.

   >>> AND THE CAMERA IS PULLED 40 SHORT OF THE FIELD'S CENTRE ON PURPOSE. <<<
   draw_room_of_arms_smd's second cheap cull is a BEHIND test built for a camera
   that looks along the floor: it drops any primitive more than 700 behind the
   camera's YAW, and pitch does not enter into it. Pointing the camera at the
   floor does not stop that test from running, so the shot has to be aimed so
   that no part of the field is more than 700 behind the yaw. At yaw 0 (looking
   +Z) the arms reach back to z=107; from the field's own centre at z=700 that is
   593 behind and the whole field survives with 107 to spare, which is not a
   margin, it is a coincidence. Dropping the camera to z=660 makes it 553 and
   buys back real room, at the cost of 60 units of framing that the 11% above
   already paid for. YAW 0 IS ALSO NOT FREE: at yaw 1024 the long axis of the
   field would run up the short axis of the screen and its west end would be 721
   behind — past the test — so the field would lose a strip to something that
   looks nothing like a cull. If this shot is ever re-aimed, re-check that test
   before re-checking the frame.

   ---- WHAT THE SIGN IS DOING AT THAT ANGLE ---------------------------------
   Wall 12 runs diagonally and its inward normal is (3738,-1673)/4096, so
   TEXT_PLANE_XY/_YZ could only put the sign edge-on or flat to the approach.
   door_draw_string_3d_yaw takes an arbitrary facing: the face of a yaw sign
   points along (-sin(yaw), -cos(yaw)), so the yaw that faces the sign back out
   of the gap along that normal is atan2(-0.9126, 0.4085) = 294.1deg = 3346 of
   4096. As a check, that puts the reading direction (cos(yaw), -sin(yaw)) at
   (0.409, 0.913), which is the wall's own direction from (-403,-155) to
   (-144,424) — the line lies IN the gap rather than across it. The string is 19
   characters at DOOR_PIXEL_SIZE, i.e. 456 wide, inside the opening's 634. */

#define ROA_GAP_X            (-274)   /* midpoint of wall 12                  */
#define ROA_GAP_Z              135
#define ROA_GAP_NX            3738    /* its inward normal, 4096 = 1.0        */
#define ROA_GAP_NZ           (-1673)
#define ROA_GAP_TEXT_Y       (-186)   /* = the east door's: eye level at y=0  */
#define ROA_GAP_TEXT_YAW      3346
#define ROA_GAP_TEXT_RADIUS   1100
#define ROA_GAP_FADE_NEAR      700
#define ROA_GAP_TRIGGER_RAD    500

/* The sign sits 11 proud of the wall along that normal — the standoff every
   other door sign in the game uses. */
#define ROA_GAP_SIGN_X   (ROA_GAP_X + ((ROA_GAP_NX * 11) >> 12))
#define ROA_GAP_SIGN_Z   (ROA_GAP_Z + ((ROA_GAP_NZ * 11) >> 12))

#define ROA_EX_CAM_X         (-572)
#define ROA_EX_CAM_Y        (-1500)
#define ROA_EX_CAM_Z           660
#define ROA_EX_CAM_ROT           0
#define ROA_EX_CAM_PITCH      1024    /* 90deg: straight down */

#define ROA_EX_IN_FRAMES        36
#define ROA_EX_OUT_FRAMES       30

#define ROA_EX_LINE  "Someone's arranged these arms in a pattern"

typedef enum {
    RAX_IDLE = 0,   /* not in it: proximity sign + Circle trigger */
    RAX_IN,         /* camera rising to the overhead shot         */
    RAX_HOLD,       /* held on the field, waiting for Cross       */
    RAX_OUT         /* coming back down to where the player is    */
} RoaExamineState;

static RoaExamineState rax_state = RAX_IDLE;

/* Pre-examine camera, restored on the way out. */
static int32_t rax_save_x, rax_save_y, rax_save_z, rax_save_rot, rax_save_vy;
/* Where the current glide started, where it ends, and how far the yaw turns. */
static int32_t rax_src_x, rax_src_y, rax_src_z, rax_src_rot, rax_src_pitch;
static int32_t rax_rot_delta, rax_dst_rot, rax_dst_pitch;
static int32_t rax_t = 0;

static int rax_circle_prev = 1;   /* the examine's own Circle edge state */
static int rax_cross_prev  = 1;   /* Cross, read straight off the pad    */

/* 0..256, latched by rax_glide each frame of a move and pinned at either end.
   Doubles as "is the shot up at all" for the draw loop's sort bias. */
static int32_t rax_fog_mix = 0;

static int roa_examine_fog_mix(void) { return rax_fog_mix; }

int room_of_arms_examine_active(void) { return rax_state != RAX_IDLE; }

/* Shortest signed turn from `from` to `to`, in 4096ths — piano_puzzle.c's. */
static int32_t rax_turn_delta(int32_t from, int32_t to) {
    int32_t d = ((to - from) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    return d;
}

/* Latch the camera's current pose as a glide's source, and its angles as the
   glide's target. The POSITION target is passed to rax_glide below instead,
   because the way out aims at a saved spot the caller already holds. */
static void rax_begin_glide(int32_t dst_rot, int32_t dst_pitch) {
    rax_src_x     = cam_x;
    rax_src_y     = cam_y;
    rax_src_z     = cam_z;
    rax_src_rot   = cam_rot;
    rax_src_pitch = cam_pitch;
    rax_rot_delta = rax_turn_delta(cam_rot, dst_rot);
    rax_dst_rot   = dst_rot;
    rax_dst_pitch = dst_pitch;
    rax_t         = 0;
}

/* One frame of a glide toward (tx,ty,tz) over `frames`; returns 1 on the frame
   it arrives, having snapped the camera exactly onto the target. */
static int rax_glide(int32_t tx, int32_t ty, int32_t tz, int frames) {
    int32_t t, inv, e;
    rax_t++;
    t = rax_t * 256 / frames; if (t > 256) t = 256;
    inv = 256 - t;
    e   = 256 - (inv * inv / 256);            /* ease-out 0..256 */
    rax_fog_mix = (rax_state == RAX_OUT) ? 256 - e : e;
    cam_x     = rax_src_x     + ((tx - rax_src_x) * e) / 256;
    cam_y     = rax_src_y     + ((ty - rax_src_y) * e) / 256;
    cam_z     = rax_src_z     + ((tz - rax_src_z) * e) / 256;
    cam_rot   = rax_src_rot   + (rax_rot_delta * e) / 256;
    cam_pitch = rax_src_pitch + ((rax_dst_pitch - rax_src_pitch) * e) / 256;
    cam_vy    = 0;
    if (rax_t < frames) return 0;
    cam_x   = tx; cam_y = ty; cam_z = tz;
    cam_rot = rax_dst_rot; cam_pitch = rax_dst_pitch; cam_vy = 0;
    rax_fog_mix = (rax_state == RAX_OUT) ? 0 : 256;
    return 1;
}

static void rax_start(void) {
    /* A held Circle may have swung the view off the player's true facing, and
       the restore at the far end must not put that offset back — cancelled
       BEFORE the pose is saved, not after (camera.h). */
    camera_look_cancel();

    rax_save_x = cam_x; rax_save_y = cam_y; rax_save_z = cam_z;
    rax_save_rot = cam_rot; rax_save_vy = cam_vy;

    /* The body stays at the gap while the camera goes up, so anything that
       hunts the player keeps hunting the spot they are actually standing on. */
    camera_anchor_player(rax_save_x, rax_save_y, rax_save_z);

    rax_fog_mix = 0;
    rax_begin_glide(ROA_EX_CAM_ROT, ROA_EX_CAM_PITCH);
    rax_state = RAX_IN;
    sound_play(SFX_SELECT);
}

static void rax_finish(void) {
    rax_state   = RAX_IDLE;
    rax_fog_mix = 0;
    cam_x   = rax_save_x; cam_y = rax_save_y; cam_z = rax_save_z;
    cam_rot = rax_save_rot; cam_vy = rax_save_vy;
    cam_pitch = 0;
    camera_release_player();
    /* BOTH of this room's Circle interactions are seeded "held" on the way out,
       so a Circle still down as the camera lands can neither re-open the shot
       nor walk the player through the east door on the frame control returns. */
    rax_circle_prev  = 1;
    east_circle_prev = 1;
}

/* ---- Per-frame -------------------------------------------------------------
   Called UNCONDITIONALLY from main.c's Room of Arms branch, `lock` passed in
   rather than tested out there: like the east door, this keeps its Circle edge
   state current while locked and does nothing, so a press held across a menu
   closing cannot read as a fresh one on the frame the lock lifts.

   Returns 1 on any frame the examine owns the camera OR has just consumed the
   Circle press — main.c's door test runs after this one and must not act on a
   press this one took. The two cannot in fact collide (the gap and the east
   door are 1567 apart with 500 radii), but the veto is what every other room's
   interaction chain does and it costs a branch. */
int room_of_arms_examine_update(int lock) {
    uint16_t btn = 0;
    int cross, cross_just;

    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        btn = ~pad->btn;
    }
    cross      = (btn & PAD_CROSS) ? 1 : 0;
    cross_just = cross && !rax_cross_prev;
    rax_cross_prev = cross;

    if (rax_state == RAX_IDLE) {
        int held = interact_tapped();
        int just = held && !rax_circle_prev;
        int32_t dx, dz, xz;
        rax_circle_prev = held;
        if (lock || !just) return 0;
        dx = cam_x - ROA_GAP_X;
        dz = cam_z - ROA_GAP_Z;
        xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz >= ROA_GAP_TRIGGER_RAD) return 0;
        if (!interact_facing(ROA_GAP_X, ROA_GAP_Z)) return 0;
        rax_start();
        return 1;
    }

    /* From here the examine owns the camera and main.c has already routed the
       frame to it, so `lock` cannot be set — but the Circle edge state above
       has been kept current either way. */
    if (rax_state == RAX_IN) {
        if (rax_glide(ROA_EX_CAM_X, ROA_EX_CAM_Y, ROA_EX_CAM_Z,
                      ROA_EX_IN_FRAMES)) {
            rax_state = RAX_HOLD;
            /* Cross is armed ON ARRIVAL and not on entry: it is also the sprint
               button, so one held down through the rise must not read as the
               press that ends the shot before it has been seen. */
            rax_cross_prev = cross;
            show_pickup_msg_raw(ROA_EX_LINE);
        }
        return 1;
    }

    if (rax_state == RAX_HOLD) {
        if (cross_just) {
            rax_begin_glide(rax_save_rot, 0);
            rax_state = RAX_OUT;
            sound_play(SFX_BACK);
        }
        return 1;
    }

    /* RAX_OUT */
    if (rax_glide(rax_save_x, rax_save_y, rax_save_z, ROA_EX_OUT_FRAMES))
        rax_finish();
    return 1;
}

void room_of_arms_examine_arm(void) {
    rax_state       = RAX_IDLE;
    rax_fog_mix     = 0;
    rax_circle_prev = interact_tapped();
    rax_cross_prev  = 1;
    cam_pitch       = 0;
    camera_release_player();
}

/* ---- Draws ----------------------------------------------------------------
   The sign, in the gap, at the wall's own angle — and nothing at all while the
   shot is up, because the camera is then 1500 above that sign looking down on
   its edge. Caller must have the room's view matrix loaded in the GTE, which
   room_of_arms_draw does. */
void room_of_arms_examine_text(RenderContext *ctx) {
    int32_t dx, dz, xz;
    int fade = 256;

    if (rax_state != RAX_IDLE) return;

    dx = cam_x - ROA_GAP_X;
    dz = cam_z - ROA_GAP_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= ROA_GAP_TEXT_RADIUS) return;

    if (xz > ROA_GAP_FADE_NEAR) {
        int range = ROA_GAP_TEXT_RADIUS - ROA_GAP_FADE_NEAR;
        int prog  = xz - ROA_GAP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to examine",
                            ROA_GAP_SIGN_X, ROA_GAP_TEXT_Y, ROA_GAP_SIGN_Z,
                            50, 255, 50, fade, ROA_GAP_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

/* The way out, in screen space, for the length of the shot. Only once the rise
   has LANDED: a prompt riding up the screen with the camera reads as part of
   the move, and taking the press mid-glide would cut the shot before the log
   line it exists to deliver has been posted. OT index 1 is the front of the
   menu-reserved range, so it sits over everything the room queued. */
void room_of_arms_examine_prompt(RenderContext *ctx) {
    if (rax_state != RAX_HOLD && rax_state != RAX_OUT) return;
    btn_prompt_draw(ctx, 8, 216, BTN_CROSS " - Return", 1);
}
