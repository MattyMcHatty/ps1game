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
#include "the_pit.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "the_pit_mesh_collision.h"
#include "the_pit_tex_map.h"
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

/* The Pit — see the_pit.h for the layout, the two levels and the door list. */

static SMD  *pit_smd  = NULL;
static void *pit_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY, unchanged: cull and fog-far are equal (so nothing is
   dropped until the fog has already faded it into the background), the base is
   scaled by what the player is carrying, and the scale eases rather than jumping.

   450/1600 — the burial hall's, the Incinerator Room's, the Tomb's and the Room
   of Arms'. The Tomb had to argue this pair against the Up Down Maze's pulled-in
   1300 (is this room's plan its puzzle?) and the Room of Arms did not have to
   argue it at all. THIS ROOM IS THE ROOM OF ARMS' CASE, for the same reason and
   about a different thing: the fog here is not hiding a plan, it is hiding a
   DEPTH.

   >>> WHAT 1600 ACTUALLY DOES IN HERE IS TAKE THE BOTTOM AWAY. <<< The player
   arrives on the south ledge at (300,220) standing 1000 units above the pit
   floor. The reachable gallery is all inside 1600 of them — the ledge is 210 deep
   and the two arms run 2400 north, so the arms fade out halfway along and the
   shaft's far wall at z=3300 is comfortably outside. The PIT FLOOR is at ring
   distance 880 at the near rim and 3080 at the far one, so at rest the player
   sees perhaps a third of it and the rest is black. Leaning over the rim of a
   hole whose bottom you cannot see is the room.

   THE HELLUMINATOR IS WHAT GIVES IT A BOTTOM. Raising the lantern and then
   burning it each add 50% of the base, so the three distances are 1600, 2400 and
   3200: lit, the near two thirds of the floor; burning, the whole shaft end to
   end. That is the chapter's bargain and here it buys the one thing in the room,
   exactly as the arms field does next door — which is the case for spending the
   light, not for making the room bright.

   AND IT IS ALSO THIS ROOM'S FRAME BUDGET. `cull` and fog-far being equal means
   these numbers decide how much MESH is walked, transformed and queued. 581
   primitives, against the Room of Arms' 578, the Incinerator Room's 708, the
   Tomb's 936 and the Up Down Maze's 1990 — the second smallest mesh in the
   chapter, which is what makes 3200 burning affordable at all.

   >>> BUT MEASURE IT FROM THE SOUTH LEDGE, AND BURNING. <<< This is the one
   Chapter 3 room where the player can have the whole mesh in front of them at
   once: there is nothing between the ledge and the far wall — no blocks, no
   screen, no corner — so the ring cull rejects almost nothing from that stance.
   It is the worst case and it is also the first thing the player sees (STEP 3J of
   tools/DIAGNOSING_FRAME_RATE.txt). */
#define PIT_BASE_FOG_NEAR   450
#define PIT_BASE_FOG_FAR   1600

#define PIT_VIEW_UNIT        256
#define PIT_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define PIT_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define PIT_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t pit_view     = PIT_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t pit_fog_near = PIT_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t pit_fog_far  = PIT_BASE_FOG_FAR;

static int32_t pit_view_target(void) {
    int32_t s = PIT_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += PIT_VIEW_HELL_BONUS;
        /* Only asked INSIDE the equipped test: helluminator_burning() cannot be
           true for an unequipped lantern, but nesting it says so rather than
           relying on the weapon layer to keep clearing it. */
        if (helluminator_burning()) s += PIT_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry: there is no previous frame to ease from
   and walking in with the lantern already up must not open the shaft out over the
   first half second. */
static void pit_view_resolve(int snap) {
    int32_t target = pit_view_target();
    if (snap) {
        pit_view = target;
    } else if (pit_view < target) {
        pit_view += PIT_VIEW_RATE;
        if (pit_view > target) pit_view = target;
    } else if (pit_view > target) {
        pit_view -= PIT_VIEW_RATE;
        if (pit_view < target) pit_view = target;
    }
    pit_fog_near = (PIT_BASE_FOG_NEAR * pit_view) >> 8;
    pit_fog_far  = (PIT_BASE_FOG_FAR  * pit_view) >> 8;
}

/* Underground, so the same near-black-with-a-cold-lift the rest of the chapter
   uses, and for the same reason (src/catacombs_entry.c): a fog that saturates to
   a true 0,0,0 makes the cull line invisible, which sounds ideal and is how you
   lose an hour to "the bottom of the pit is missing". In THIS room that line is
   what the player is looking at on purpose, so it has to read as darkness rather
   than as an absence. */
#define PIT_FOG_R             7
#define PIT_FOG_G             6
#define PIT_FOG_B             9

/* Wall standoff. The chapter's 195, and it is a measured fit rather than a
   default in both halves of the room. The GALLERY is the tight one: the south
   ledge runs z[0,600] between wall 19 and the rim (wall 0), so the walkable band
   is z[195,405] — 210 across, which is the Tomb's aisle exactly. The two arms are
   600 wide and give the same 210. The pit floor below is 1984 by 2801 and argues
   with nothing.

   >>> 195 IS ALSO WHAT KEEPS THE PLAYER OFF THE RIM, and that is load-bearing
   here in a way it is nowhere else. <<< Walls 0-4 are the gallery's inner edge and
   there is no railing modelled on them, because there does not need to be: the
   standoff holds the player 195 back from a 1000-unit drop. Lowering it for this
   room would put them on the lip. */
#define PIT_WALL_RADIUS     195

/* The two floor heights, as world Y. -Y is up, so the gallery is 1000 ABOVE the
   pit floor. */
#define PIT_GALLERY_Y      (-1000)
#define PIT_FLOOR_Y             0

/* Standing eye on the GALLERY: less GROUND_FLOOR_Y and the 40-unit standoff
   apply_height applies. Used for the arrival spawn only — apply_height settles
   cam_y every frame afterwards. There is deliberately no pit-floor eye height:
   nothing spawns down there, because nothing can get down there yet. */
#define PIT_GALLERY_EYE_Y  (PIT_GALLERY_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   FOUR, IN TWO LEVELS, AND THIS IS THE THIRD SHAPE tools/ADDING_A_ROOM.txt
   STEP 5 lists rather than either of the first two.

   It is NOT the Tomb's or the Room of Arms' one flat plane, obviously. It is also
   NOT the Up Down Maze's ten-walkways-over-a-catch-all, and the difference is the
   whole reason the order below does not matter: THE TWO LEVELS HAVE DISJOINT XZ
   FOOTPRINTS. The gallery has nothing inside x[-692,1292] past z=910; the pit has
   nothing before z=1099. No point in this room has two walkable heights, so
   apply_height()'s "skip a zone whose surface is above the player" never has to
   choose — it is the Catacombs Entry's and the Delivery Area's case, where the
   zones meet in plan and their ORDER is at worst a nicety.

   THE UPPER ZONES ARE LISTED FIRST ANYWAY. It costs nothing and it is the Up Down
   Maze's rule, which is the one to be holding the day a later change stacks
   something over something else in here — the way down, when it is built, is
   exactly that change.

   >>> THE THREE GALLERY RECTS ARE DELIBERATELY BIGGER THAN THE WALKABLE FLOOR,
   AND THAT IS THE SAFE DIRECTION. <<< A no-zone XZ falls through to
   apply_height's `target = 0` fallback, which on the gallery would be a silent
   1000-unit drop into the pit — the worst failure this room can have. The proxy's
   seven gallery faces are a staircase of rects around two chamfered corners, and
   rects cannot follow a chamfer exactly, so each zone is widened out to the next
   wall line instead. What that over-covers is void the player cannot reach: walls
   0-4 seal the rim and walls 6-13 and 19-21 the outside, so every XZ these rects
   add is somewhere the push has already made unreachable. Over-covering costs
   nothing; under-covering is the drop.

     A  the SOUTH LEDGE and BOTH chamfered corners, as one band across the full
        width: x[-2104,2699] z[0,910]. This merges the proxy's FLOOR 4, 5, 6, 7
        and 8 and fills the diagonal gaps between them. z stops at 910 because
        that is where the two arms begin, and it is clear of the pit's z=1099.
     B  the EAST ARM: x[1998,2699] z[910,3300]. FLOOR 0 is x[2099,2699]; the west
        edge is pulled out to 1998 (FLOOR 6's extent) so a frame spent inside wall
        12's push still stands on something.
     C  the WEST ARM: x[-2104,-1402] z[910,3300]. FLOOR 1, with its east edge
        pulled out to FLOOR 7's -1402 for the same reason against wall 13.

   THE PIT IS ONE ZONE FOR TWO PROXY FACES, merged on the Room of Arms' argument:
   FLOOR 2 (the north alcove) and FLOOR 3 (the main floor) are two slabs of one
   plane at y=0, and two zones would be two identical answers. Merging them also
   closes the sliver at their z=3300 seam.

     D  x[-692,1292] z[1099,3900], y=0.

   FLOOR_UPPER for the gallery, FLOOR_FLAT for the pit. The flag the engine reads
   off that is player_on_upper_floor, and nothing in this room acts on it yet —
   but it is what the entity height and collision routines ask, and it is TRUE
   here in a way it is not in the Tomb: there really is a floor underneath. The Up
   Down Maze's note says the same of its walkways. */
static void the_pit_floor_zones_init(void) {
    int i = 0;

    /* ---- THE GALLERY, y=-1000. Listed first; see the note above. ---------- */
    floor_zones[i].type  = FLOOR_UPPER;          /* A: south ledge + corners */
    floor_zones[i].min_x = -2104; floor_zones[i].max_x =  2699;
    floor_zones[i].min_z =     0; floor_zones[i].max_z =   910;
    floor_zones[i].y     = PIT_GALLERY_Y;
    i++;

    floor_zones[i].type  = FLOOR_UPPER;          /* B: east arm              */
    floor_zones[i].min_x =  1998; floor_zones[i].max_x =  2699;
    floor_zones[i].min_z =   910; floor_zones[i].max_z =  3300;
    floor_zones[i].y     = PIT_GALLERY_Y;
    i++;

    floor_zones[i].type  = FLOOR_UPPER;          /* C: west arm              */
    floor_zones[i].min_x = -2104; floor_zones[i].max_x = -1402;
    floor_zones[i].min_z =   910; floor_zones[i].max_z =  3300;
    floor_zones[i].y     = PIT_GALLERY_Y;
    i++;

    /* ---- THE PIT FLOOR, y=0. Unreachable for now; see the header. -------- */
    floor_zones[i].type  = FLOOR_FLAT;           /* D: pit + north alcove    */
    floor_zones[i].min_x =  -692; floor_zones[i].max_x =  1292;
    floor_zones[i].min_z =  1099; floor_zones[i].max_z =  3900;
    floor_zones[i].y     = PIT_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, AND THE ROOM OWNS ONE — the Room of Arms' arrangement exactly.

     0 cobblestones        the gallery, the shaft's outer walls, the vault and the
                           pit floor — 451 of the 581 polys
                           (clsd_drwr page, x384 y0)
     1 rusty               the corroded ironwork lining the shaft, 126 polys, and
                           the reason the room looks like anything
                           (rusty_fence / anzu3 page, x704 y0)   OWNED HERE
     2 catacomb inner door the two doorways: south on the gallery, north on the
                           pit floor
                           (opn_drwr page,  x832 y0)

   Slots 0 and 2 are registered by src/catacombs_entry.c in TEXBANK_CATACOMBS and
   reached through that module's NARROW uploaders, as every Chapter 3 room since
   the Up Down Maze reaches theirs — narrow for the usual reason
   (tools/ADDING_A_ROOM.txt STEP 3b): the Catacombs Entry's FULL uploader would
   also stamp the lamashtu tablet, the loculus, the sconce and the oil dispenser,
   none of which this room draws.

   >>> SLOT 1 IS 128x128 STRETCHED FROM A 64x64 SOURCE, AND THAT WAS MEASURED
   RATHER THAN ASSUMED. <<< textures/catacombs/rusty.png is 64 square, so
   tools/TEXTURING_NOTES.txt PART 7 applies and it has two answers: STRETCH, if
   the art is one whole thing per UV tile, or TILE NxN, if it is a small repeating
   unit the artist UV'd at a 32- or 64-texel period. Choosing wrong renders
   perfectly and is simply the wrong SIZE on the wall. The test is world units per
   UV tile, against a texture in the SAME model that already reads correctly:

       cobblestones   median 534 world units per tile   (the reference, 128x128)
       rusty          median 584                        <- the same order
       catacomb door  median 342

   584 on top of 534 says one copy per tile is what was drawn, so it is the
   stretch case and rusty_128.png is the source scaled up. A 4x4 tiling would have
   put the ironwork's period at ~146 units and made it read as chainmail.

   >>> SLOT 1'S VRAM PAGE IS x704 y0, THE LAST WHOLE MESH-ART PAGE IN THIS BANK,
   AND IT IS THE EXPENSIVE ONE. <<< tools/VRAM_MAP_CATACOMBS.txt lists x320 y0,
   x704 y0 and x768/x832 y256 as what is left, and they are not equal. The crib
   took x576 y0 in September 2026 and owed nothing for it, because that page's
   occupants are anzu2/anzu5 (put back by anzu_tex_stream) and red_wlppr (put back
   by kitchen_stream_owned_textures) and no more. x704's LEFT half is SIX 4bpp
   garden textures that the catacombs map does not print at all, and an 8bpp 128
   texture takes all 64 columns of a page, so it lands on every one of them. The
   crib's own note in tools/vram_map.py records going here first and being caught
   by py tools/vram_map.py.

   It is taken deliberately this time, and every occupant was walked back to a
   LIVE restore before the pairs were written:

       anzu3, anzu6            -> anzu_tex_stream(),               piano room
       rusty_fence             -> delivery_restore_textures(),     delivery area
       upstairs                -> hall_2f_upload_upstairs(),       2F hall
       gravel_gs               -> garden_stairs' uploader,         garden stairs
       chain                   -> chain_room's uploader,           chain room
       stable glyphs           -> stables' uploader,               stables
       poison_flower_base_gh   -> greenhouse's uploader,           greenhouse

   All seven are rooms the player walks INTO, so a title load into a Chapter 1 or
   2 save puts the right pixels up on arrival. SEVEN CALLS ARE NOW LOAD-BEARING
   FOR THIS ONE WALL TEXTURE. That is six more than the crib's page cost, and it
   is the price of the last page: the next Chapter 3 texture has no page like this
   left and should expect to borrow ART rather than a SLOT.

   Its CLUT is BORROWED, from anzu3.tim at (256,485), on the arms field's and the
   crib's argument — a palette belonging to a texture whose PIXELS it is already
   displacing on this very page, so the two go back together in the one anzu
   stream. The 256-word runs left in tools/VRAM_MAP.txt are not what a wall
   spends.

   It is registered DEFERRED like the rest of the chapter (src/texmgr.h): the
   header is read at startup and the 16.5 KB of pixels only at the chapter door,
   by area_bank_sync(), so Chapters 1 and 2 pay nothing for it.

   >>> AND IT WAS THE 72nd REGISTRATION OF 72. <<< The array was EXACTLY full.
   TEXMGR_MAX in src/texmgr.c is 80 now and the comment there records why; a
   register past the cap returns -1 SILENTLY and breaks that texture in every room
   that draws it. Count with py tools/heap_budget.py before adding the 73rd, and
   prefer a narrow uploader on an owning module to a second RAM copy of art the
   game already holds.

   ALL THREE SIT AT Voff 0, so the one 128 texture window set in the_pit_draw
   serves them (tools/VRAM_MAP.txt). */
#define THE_PIT_TEX_COUNT 3

static uint16_t tex_tpage[THE_PIT_TEX_COUNT];
static uint16_t tex_clut[THE_PIT_TEX_COUNT];

/* The one texmgr entry this room owns (slot 1). -1 until registration, which is
   also what a registration past TEXMGR_MAX leaves behind — texmgr_upload on -1 is
   a no-op, so the failure mode is the ironwork drawing as whatever else is on
   x704 y0 rather than a crash. */
static int rusty_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/tomb.c, which took it from src/incinerator_room.c, which took
   it from src/up_down_maze.c, which took it from src/catacombs_entry.c, which has
   been through that document.

   The keys go in the SHARED arena (src/cull_arena.h) and are built on the same
   call that reloads the mesh they describe, which is the one rule that file has.
   A rejected primitive is then ONE sequential 6-byte read and never addresses the
   SMD header, the index array or the vertex array.

   NO BOX KEY AND NO SIDE-PLANE CULL, for the reason the Room of Arms, the Tomb,
   the Incinerator Room, the Up Down Maze, the Catacombs Entry, the Greenhouse,
   Maze One, Reception and the Master Bedroom all record: cull_boxes pays for
   itself only where a side-plane test would otherwise chase v1..v3 per surviving
   primitive, and there is no such test here to feed. "The room is open so it
   would cull a lot" is wrong turn #2 in DIAGNOSING_FRAME_RATE.txt and it has now
   been the tempting wrong answer nine times.

   >>> THIS ROOM IS TEMPTING ON A NEW GROUND, AND IT IS WORTH NAMING SO IT IS NOT
   MISTAKEN FOR A NEW ARGUMENT. <<< It is the only Chapter 3 room where the whole
   mesh can be in front of the camera at once, which sounds like the case for a
   frustum test and is the opposite: the ring cull already rejects nearly nothing
   from the ledge, so there is barely a reject path left to make cheaper, and a
   side-plane test would be paid for on every one of the 581 primitives to throw
   away almost none. If a METER says otherwise, that is the case for adding one,
   with a box key under it and an offline hole sweep beside it. */
static int pit_key_count = 0;

static void pit_build_cull_keys(void) {
    pit_key_count = 0;
    if (!pit_smd) return;
    uint8_t *p = (uint8_t *)pit_smd->p_prims;
    int i, n = pit_smd->n_prims;
    if (n > THE_PIT_PRIM_COUNT) n = THE_PIT_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &pit_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    pit_key_count = n;
}

void the_pit_load_geometry(void) {
    pit_buff = room_arena_load("\\TEXCTCMB\\THEPIT.SMD;1");
    pit_smd  = pit_buff ? smdInitData(pit_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    pit_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: three compile-time headers and ONE
   deferred registration.

   >>> texmgr_set_bank() IS HERE, unlike in the Up Down Maze, the Incinerator Room
   and the Tomb. <<< Those three make no registration at all, so they have nothing
   to tag and the call would be a no-op; this one does, like the Room of Arms'. The
   mask is DERIVED and not guessed — py tools/check_tex_banks.py walks the uploader
   call graph, finds this module reached only from the_pit_upload_textures(), and
   fails if CATACOMBS is not in it. Getting it wrong is SILENT: an upload whose
   bank is out does nothing and the room draws whatever the last room left on
   x704 y0. */
void the_pit_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    rusty_tex = texmgr_register("\\TEXCTCMB\\RUSTY.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, RUSTY);
    TIM_SLOT(2, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   NO ORDERING RULE, for the Catacombs Entry's reason: nothing else uploads to
   these three pages while this chapter is resident, because nothing else in the
   game is reachable. If a texmgr entry is somehow not loaded, texmgr_upload is a
   no-op and the slot keeps whatever the previous room left in it — visibly wrong,
   and quietly, which is why area_bank_sync runs before this and not after. */
void the_pit_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    /* ...and this room's own page. Nothing else in the chapter draws x704 y0, so
       there is no ordering rule between this line and the two above either. */
    texmgr_upload(rusty_tex);
}

/* ---- THE SOUTH DOOR --------------------------------------------------------
   z=0, x[200,400], y[-1400,-1000] — in the gallery's south wall, at the near end
   of the ledge the player arrives on, and the only one of this room's two drawn
   doors that goes anywhere. Back into the TOMB, through the door that room's
   header has been listing as drawn-but-sealed since it landed.

   A door in the XY plane at fixed Z, approached from +Z (wall 19 runs z=0 with
   nz=+4096, so the walkable side is +Z), so TEXT_PLANE_XY with mirror=1 and the
   sign 11 units proud of the wall along +Z. The Tomb's north door, the far side of
   this wall, takes mirror=0 and -11 because it stands on the other side of it.

   >>> THE READING AXIS FOR AN XY SIGN IS X, so the -200 door_draw_string_3d wants
   goes on the X argument and not on the Z one. <<< That is the opposite limb from
   the Tomb's two doors, which are both YZ and both put it on Z, so the two are
   easy to transpose when copying. Putting it on the wrong axis does not fail
   loudly: the line simply sits 200 units inside the wall and is half-swallowed by
   it.

   ITS Y IS A GALLERY Y AND NOT A GROUND-FLOOR ONE. Eye level on the y=-1000
   walkway is -1000 - 186 = -1186, where every other sign in the chapter is at
   -186. Copying -186 here would hang the string a whole storey below the ledge,
   in mid-air over the pit. */
#define PIT_SOUTH_X            300     /* the art spans x[200,400] */
#define PIT_SOUTH_Z              0
#define PIT_SOUTH_TEXT_Y    (-1186)    /* eye level on the y=-1000 gallery */
#define PIT_TEXT_RADIUS       1200
#define PIT_FADE_NEAR          800
#define PIT_TRIGGER_RADIUS     500

/* The NORTH door (z=3900, x[158,442], y[-500,0]) is drawn and nothing else — no
   #define block, no sign, no trigger. It is in the PIT, which the player cannot
   reach, so it is sealed twice over; wiring it up is a block like this one plus
   the STEP 6 edits in tools/ADDING_A_ROOM.txt, and it should wait for the way
   down, because a door nobody can walk to is not a door. */

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in through
   the transition cannot fire on the arrival frame. One door, so this is one
   variable rather than the Tomb's pair — but it is still routed through the shared
   body below, because the north door will want the second one. */
static int south_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void the_pit_arm(void) {
    south_circle_prev = circle_held();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts —
   hatch_puzzle_update()'s rule, for its reason.

   NO Y TEST, AND THE HEADER ARGUES IT AT LENGTH: this room's walkable surface IS
   a function of XZ, because the gallery and the pit have disjoint footprints, so
   the plain Manhattan test every other door in the game makes is correct here.
   The Up Down Maze's two doors are the only ones that need more. Re-read that the
   day the two sections are connected. */
static int door_triggered(int lock, int32_t door_x, int32_t door_z, int *prev) {
    int held = circle_held();
    int just = held && !*prev;
    int32_t dx, dz, xz;
    *prev = held;
    if (lock || !just) return 0;
    dx = cam_x - door_x;
    dz = cam_z - door_z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= PIT_TRIGGER_RADIUS) return 0;
    if (!interact_facing(door_x, door_z)) return 0;
    return 1;
}

int the_pit_south_door_triggered(int lock) {
    return door_triggered(lock, PIT_SOUTH_X, PIT_SOUTH_Z, &south_circle_prev);
}

/* A door's floating sign. Same shape as every other sign in the game: opaque
   within PIT_FADE_NEAR, gone by PIT_TEXT_RADIUS.

   `standoff` is the sign's offset off the wall toward the player and `mirror` is
   which way the glyphs read, and THE TWO ALWAYS AGREE: for an XY-plane door, +11
   with mirror=1 for one approached from +Z, -11 with mirror=0 for one approached
   from -Z. (That is the opposite pairing from the Tomb's YZ doors.) They are
   passed as a pair rather than derived here so that the north door, when it lands,
   states its own at its call site. */
static void pit_door_text(RenderContext *ctx, int32_t door_x, int32_t door_z,
                          int32_t text_y, int32_t standoff, int mirror) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - door_z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= PIT_TEXT_RADIUS) return;

    if (xz > PIT_FADE_NEAR) {
        int range = PIT_TEXT_RADIUS - PIT_FADE_NEAR;
        int prog  = xz - PIT_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* door_draw_string_3d adds 200 to the reading axis before centring, and an XY
       sign reads along X — hence the -200 on THAT argument. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        door_x - 200, text_y, door_z + standoff,
                        50, 255, 50, fade, mirror, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

void the_pit_spawn_south(void) {
    /* Arriving from the Tomb. Clear of the wall push radius on the +Z side — the
       walkable side of wall 19 — and facing +Z, the direction of travel through
       the door, which puts the shaft and its far wall straight ahead and the drop
       just past the player's feet. That view is the room's introduction, and it is
       why the rotation is not turned along the ledge.

       220 OFF THE WALL, AND THE LEDGE IS ONLY 210 WIDE. The walkable band between
       wall 19 (z=0, pushing to z>=195) and the rim (wall 0 at z=600, pushing to
       z<=405) is z[195,405], so 220 is 25 clear of the back wall and 185 clear of
       the drop: inside the band with room on both sides, which is the whole of
       what "at least 215 off the wall" is asking for. There is no room to be more
       generous than that without standing the player on the lip.

       cam_y is the GALLERY eye, not the ground-floor one. apply_height settles it
       on the next frame out of zone A, but a spawn at the pit floor's height would
       put the arrival frame 1000 units down the shaft. */
    cam_x   = PIT_SOUTH_X;
    cam_y   = PIT_GALLERY_EYE_Y;
    cam_vy  = 0;
    cam_z   = PIT_SOUTH_Z + (PIT_WALL_RADIUS + 25);
    cam_rot = 0;                       /* facing +Z, north across the shaft */
    the_pit_arm();
}

void the_pit_init(void) {
    the_pit_collision_init(&current_collision_room);
    /* The DRAWN ceiling, read off the VISUAL mesh and not only off the collision
       proxy: the vault over the whole shaft is at y=-1800, which is also where the
       gallery's proxy walls (6-13, 19-21) stop, so the two agree and the honest
       number is available.

       >>> IT IS 1800 FROM THE PIT FLOOR AND ONLY 800 FROM THE LEDGE THE PLAYER
       STANDS ON. <<< This is the one room in the chapter where those are
       different numbers, and collision_set_ceiling_y reports a single value.
       Anything hung from the vault is 800 over the gallery; anything authored
       against "the ceiling of this room" from down in the pit is 1800 up. The
       Tomb's, the Room of Arms' and the Incinerator Room's 800 is the GALLERY
       figure here, not this one. */
    collision_set_ceiling_y(-1800);
    collision_set_wall_radius(PIT_WALL_RADIUS);

    the_pit_floor_zones_init();
    the_pit_spawn_south();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every one of their collide routines —
       so an instance left over from another room would block the player invisibly
       anywhere its coordinates fall inside this room's bounds (x[-2104,2699]
       z[0,3900]).

       BOTH OF THE CHAPTER'S SCONCES ARE INSIDE THOSE BOUNDS, as they are inside
       the Room of Arms': the Catacombs Entry's pair is at (±595,200), and both
       (595,200) and (-595,200) land on this gallery's south ledge — in the 800 of
       it the player walks in on. Neither bites, because sconces_collide() gates on
       s->area != current_area and both instances are tagged
       STATE_CATACOMBS_ENTRY; the two arrays that are NOT gated, the save point and
       the dresser, have nothing in Chapter 3 that lands in here. So this is the
       cheap guarantee the Tomb's and the Room of Arms' four calls are, and not a
       fix — but it is a room with NO margin: this footprint is the largest in the
       chapter and it starts at the origin, so anything ever placed in the
       Catacombs Entry within 2100 of ITS origin sits in here too. That is the case
       Mistake 5 in tools/ADDING_A_ROOM.txt is about.

       THE CRIB IS NOT ON THIS LIST and does not need to be: cribs_collide() gates
       on c->area != current_area, and the Room of Arms' cot is at (-973,-356),
       which is outside these bounds anyway (z<0).

       Safe to clear: catacombs_entry_init() re-places all four on every entry to
       that room, and this room places none of them. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* Resolve the view distance with no ease: the first frame in the room shows
       whatever the player walked in holding. */
    pit_view_resolve(1);
}

static void draw_the_pit_smd(RenderContext *ctx) {
    if (!pit_smd) return;

    uint8_t *p = (uint8_t *)pit_smd->p_prims;
    int i, n = pit_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). None of them can change
       while a frame is being queued: the two trig lookups would otherwise be a
       pair of SDK calls for every primitive that passed the distance cull, the
       cull distance would be re-read all 581 times, and buf_end is a double
       indirection through ctx->active_buffer, which a draw cannot change. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = pit_fog_far;   /* resolved by pit_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< Six sequential
           bytes carry this primitive's first vertex X/Z and its stride, which is
           everything both cheap tests below need AND everything the walk needs to
           advance — so a rejected primitive never touches the SMD header, the
           vertex index array or the vertex array. See pit_build_cull_keys.

           BOTH CULLS ARE XZ-ONLY, WHICH IS THE RIGHT ANSWER IN A ROOM 1800 DEEP
           and worth saying once: a key holds x and z and no y, so the pit floor
           directly below the player is at ring distance ~0 and is never rejected
           however far down it is. That is what makes looking over the rim work at
           all, and it means this room's 1800 of vertical extent costs the reject
           path nothing. */
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
        SVECTOR *v0 = &pit_smd->p_verts[vi[0]];
        SVECTOR *v1 = &pit_smd->p_verts[vi[1]];
        SVECTOR *v2 = &pit_smd->p_verts[vi[2]];

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

        int nocull = (i < THE_PIT_PRIM_COUNT) && the_pit_nocull[i];
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
            v3 = &pit_smd->p_verts[vi[3]];
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
           floors stay behind whatever stands on them (see render.h). >>> THIS
           ROOM HAS THREE STACKED SETS OF THEM AND THAT IS WHERE THE RULE EARNS
           ITS KEEP. <<< The gallery's walkway, the pit floor 1000 below it and
           the vault 800 above it are all horizontal, and from the south ledge the
           player sees along the gallery AND down into the pit in one frame — so
           the sort separates three planes here rather than the Tomb's two.
           Unlike the Up Down Maze, still no surface is a floor on one side and a
           ceiling on the other: the gallery is solid to y=-1800 in the proxy and
           nothing is drawn underneath it. */
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
        int32_t fog = dist < pit_fog_near ? pit_fog_near : (dist > pit_fog_far ? pit_fog_far : dist);
        int32_t fog_factor = ((pit_fog_far - fog) << 8) / (pit_fog_far - pit_fog_near);

        uint8_t tex_idx = (i < THE_PIT_PRIM_COUNT) ? the_pit_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < THE_PIT_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + PIT_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + PIT_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + PIT_FOG_B * (256 - fog_factor)) >> 8);

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

void the_pit_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    pit_view_resolve(0);

    /* Anything else that fogs in this room follows the debug view distance when
       one is selected, so levels 6/7 change what the room LOOKS like consistently
       rather than only where the mesh stops. */
    g_fog_near = pit_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : pit_fog_far;

    /* Background in the SAME colour the fog saturates to, as the CLEAR COLOUR and
       not as a primitive: the draw environments carry isbg=1, so DrawOTagEnv has
       already filled the whole framebuffer before the first poly is drawn, and a
       full-screen TILE on top would be a second 77,000-pixel fill every frame for
       the colour alone. Wrong turn #3 in tools/DIAGNOSING_FRAME_RATE.txt. */
    render_set_clear_colour(ctx, PIT_FOG_R, PIT_FOG_G, PIT_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page. All
       three of this room's textures sit at page-top (Voff 0), so one window serves
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

    if (exp != DBG_EXP_NO_MESH) draw_the_pit_smd(ctx);

    /* >>> LEVEL 8 REMOVES ONE SIGN AND WHATEVER world.c HAS PUT IN HERE. <<< The
       room holds no props and, today, no enemies, so the D reading at levels 1, 4
       and 8 splits its frame between the mesh and ONE floating string — which is
       exactly the case STEP 3D of tools/DIAGNOSING_FRAME_RATE.txt was written
       about. Reception's frame turned out to be its SIGNAGE and not its mesh,
       because door_draw_string_3d has no facing test and queues every glyph in
       full with the player's back to it. With one sign in a 4800 room that is
       cheaper here than it was there, but it is still the second thing to measure
       and not the last. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        pit_door_text(ctx, PIT_SOUTH_X, PIT_SOUTH_Z, PIT_SOUTH_TEXT_Y,
                      +11, 1);   /* south: XY plane, approached from +Z */
        /* BOTH CHAPTER 3 ENEMIES, and both are drawn in all six of its rooms
           whether or not world.c places one here. That is deliberate and it is
           what "either enemy may go in any Catacombs room" actually costs: the
           area tag makes an absent enemy free (the loop skips every instance whose
           area is not current_area), and a placement then starts drawing without
           this file having to be touched again.

           >>> AND IN THIS ROOM A PLACEMENT HAS TO PICK A STOREY, which is new.
           <<< world_seed_room() authors coordinates as literals and cannot read
           this room's floor zones, so an enemy meant for the gallery needs y
           authored against -1000 and one meant for the pit against 0 — and one
           put in the pit can never reach the player until the two sections are
           connected. src/the_pit.h's layout list is the reference for which is
           which.

           Both sheets sit at Voff 128 (VRAM y=128), so unlike this room's mesh art
           they cannot live under the 128 texture window set at the top of this
           function: each is handed that window to RESTORE after its sprite and
           draws itself unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
    }
}
