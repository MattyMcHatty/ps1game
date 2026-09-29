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
#include "helluminator.h"       /* helluminator_burning â€” a view-distance factor */
#include "sound.h"              /* the descent's footfalls                    */
#include "bars.h"               /* the portcullis over the north alcove       */

/* The Pit â€” see the_pit.h for the layout, the two levels and the door list. */

static SMD  *pit_smd  = NULL;
static void *pit_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY, unchanged: cull and fog-far are equal (so nothing is
   dropped until the fog has already faded it into the background), the base is
   scaled by what the player is carrying, and the scale eases rather than jumping.

   450/1600 â€” the burial hall's, the Incinerator Room's, the Tomb's and the Room
   of Arms'. The Tomb had to argue this pair against the Up Down Maze's pulled-in
   1300 (is this room's plan its puzzle?) and the Room of Arms did not have to
   argue it at all. THIS ROOM IS THE ROOM OF ARMS' CASE, for the same reason and
   about a different thing: the fog here is not hiding a plan, it is hiding a
   DEPTH.

   >>> WHAT 1600 ACTUALLY DOES IN HERE IS TAKE THE BOTTOM AWAY. <<< The player
   arrives on the south ledge at (300,220) standing 1000 units above the pit
   floor. The reachable gallery is all inside 1600 of them â€” the ledge is 210 deep
   and the two arms run 2400 north, so the arms fade out halfway along and the
   shaft's far wall at z=3300 is comfortably outside. The PIT FLOOR is at ring
   distance 880 at the near rim and 3080 at the far one, so at rest the player
   sees perhaps a third of it and the rest is black. Leaning over the rim of a
   hole whose bottom you cannot see is the room.

   THE HELLUMINATOR IS WHAT GIVES IT A BOTTOM. Raising the lantern and then
   burning it each add 50% of the base, so the three distances are 1600, 2400 and
   3200: lit, the near two thirds of the floor; burning, the whole shaft end to
   end. That is the chapter's bargain and here it buys the one thing in the room,
   exactly as the arms field does next door â€” which is the case for spending the
   light, not for making the room bright.

   AND IT IS ALSO THIS ROOM'S FRAME BUDGET. `cull` and fog-far being equal means
   these numbers decide how much MESH is walked, transformed and queued. 581
   primitives, against the Room of Arms' 578, the Incinerator Room's 708, the
   Tomb's 936 and the Up Down Maze's 1990 â€” the second smallest mesh in the
   chapter, which is what makes 3200 burning affordable at all.

   >>> BUT MEASURE IT FROM THE SOUTH LEDGE, AND BURNING. <<< This is the one
   Chapter 3 room where the player can have the whole mesh in front of them at
   once: there is nothing between the ledge and the far wall â€” no blocks, no
   screen, no corner â€” so the ring cull rejects almost nothing from that stance.
   It is the worst case and it is also the first thing the player sees (STEP 3J of
   tools/DIAGNOSING_FRAME_RATE.txt). */
#define PIT_BASE_FOG_NEAR   450
#define PIT_BASE_FOG_FAR   1600

#define PIT_VIEW_UNIT        256
#define PIT_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define PIT_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define PIT_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

/* >>> AND THE AMBUSH OPENS IT ALL THE WAY FOR ITS OWN LENGTH. <<< The bars
   drop across the north alcove at z~3270 and the descent sets the player down
   at z=1355, 1915 away â€” past the 1600 an unlit room shows, so at the base
   distance the one thing the scene is FOR would happen in the dark. From the
   start of the run-out until control comes back the target is held at the
   burning figure (3200), which is already this room's measured worst case, so
   it costs no frame budget that the lantern does not already spend. It eases in
   over the run-out (32 frames of 42, with the pitch still coming up) and eases
   back out over the first half second the player owns, so the shaft closes
   back down around them rather than snapping. See THE AMBUSH below. */
#define PIT_VIEW_SCENE      (PIT_VIEW_UNIT + PIT_VIEW_HELL_BONUS + PIT_VIEW_BURN_BONUS)

static int32_t pit_view     = PIT_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t pit_fog_near = PIT_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t pit_fog_far  = PIT_BASE_FOG_FAR;
static int     pit_view_scene = 0;                 /* the ambush is lit     */

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
    if (pit_view_scene && s < PIT_VIEW_SCENE) s = PIT_VIEW_SCENE;
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
   is z[195,405] â€” 210 across, which is the Tomb's aisle exactly. The two arms are
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

/* Standing eye on each level: less GROUND_FLOOR_Y and the 40-unit standoff
   apply_height applies. The gallery's is the arrival spawn's; the pit floor's is
   where the descent sets the player down, and it has to be exactly apply_height's
   answer or the first free frame would snap the view. apply_height settles cam_y
   every frame after either. */
#define PIT_GALLERY_EYE_Y  (PIT_GALLERY_Y - GROUND_FLOOR_Y - 40)
#define PIT_FLOOR_EYE_Y    (PIT_FLOOR_Y   - GROUND_FLOOR_Y - 40)

/* Halfway between the two floors. A cam_y above this line is standing on the
   gallery, below it in the pit â€” which is all the descend prompt needs to know,
   since the scene is the only thing that ever crosses it. */
#define PIT_STOREY_SPLIT_Y ((PIT_GALLERY_Y + PIT_FLOOR_Y) / 2)

/* ---- Floor zones -----------------------------------------------------------
   FOUR, IN TWO LEVELS, AND THIS IS THE THIRD SHAPE tools/ADDING_A_ROOM.txt
   STEP 5 lists rather than either of the first two.

   It is NOT the Tomb's or the Room of Arms' one flat plane, obviously. It is also
   NOT the Up Down Maze's ten-walkways-over-a-catch-all, and the difference is the
   whole reason the order below does not matter: THE TWO LEVELS HAVE DISJOINT XZ
   FOOTPRINTS. The gallery has nothing inside x[-692,1292] past z=910; the pit has
   nothing before z=1099. No point in this room has two walkable heights, so
   apply_height()'s "skip a zone whose surface is above the player" never has to
   choose â€” it is the Catacombs Entry's and the Delivery Area's case, where the
   zones meet in plan and their ORDER is at worst a nicety.

   THE UPPER ZONES ARE LISTED FIRST ANYWAY. It costs nothing and it is the Up Down
   Maze's rule, which is the one to be holding the day a later change stacks
   something over something else in here â€” the way down, when it is built, is
   exactly that change.

   >>> THE THREE GALLERY RECTS ARE DELIBERATELY BIGGER THAN THE WALKABLE FLOOR,
   AND THAT IS THE SAFE DIRECTION. <<< A no-zone XZ falls through to
   apply_height's `target = 0` fallback, which on the gallery would be a silent
   1000-unit drop into the pit â€” the worst failure this room can have. The proxy's
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
   off that is player_on_upper_floor, and nothing in this room acts on it yet â€”
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

    /* ---- THE PIT FLOOR, y=0. Reached only by the descent. ---------------- */
    floor_zones[i].type  = FLOOR_FLAT;           /* D: pit + north alcove    */
    floor_zones[i].min_x =  -692; floor_zones[i].max_x =  1292;
    floor_zones[i].min_z =  1099; floor_zones[i].max_z =  3900;
    floor_zones[i].y     = PIT_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, AND THE ROOM OWNS ONE â€” the Room of Arms' arrangement exactly.

     0 cobblestones        the gallery, the shaft's outer walls, the vault and the
                           pit floor â€” 451 of the 581 polys
                           (clsd_drwr page, x384 y0)
     1 rusty               the corroded ironwork lining the shaft, 126 polys, and
                           the reason the room looks like anything
                           (rusty_fence / anzu3 page, x704 y0)   OWNED HERE
     2 catacomb inner door the two doorways: south on the gallery, north on the
                           pit floor
                           (opn_drwr page,  x832 y0)

   Slots 0 and 2 are registered by src/catacombs_entry.c in TEXBANK_CATACOMBS and
   reached through that module's NARROW uploaders, as every Chapter 3 room since
   the Up Down Maze reaches theirs â€” narrow for the usual reason
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
   crib's argument â€” a palette belonging to a texture whose PIXELS it is already
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
   also what a registration past TEXMGR_MAX leaves behind â€” texmgr_upload on -1 is
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
   mask is DERIVED and not guessed â€” py tools/check_tex_banks.py walks the uploader
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

/* Pure LoadImage from the RAM copies area_bank_sync() has already read â€” no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   NO ORDERING RULE, for the Catacombs Entry's reason: nothing else uploads to
   these three pages while this chapter is resident, because nothing else in the
   game is reachable. If a texmgr entry is somehow not loaded, texmgr_upload is a
   no-op and the slot keeps whatever the previous room left in it â€” visibly wrong,
   and quietly, which is why area_bank_sync runs before this and not after. */
/* The ironwork alone, for the CLEAVER prop (src/cleaver.c), whose blades are
   modelled in this texture. The narrow-uploader pattern: the full uploader
   would also stamp the bars, which the Cleaver Corridor does not draw. */
void the_pit_upload_rusty(void) {
    texmgr_upload(rusty_tex);
}

void the_pit_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    /* ...and this room's own page. Nothing else in the chapter draws x704 y0, so
       there is no ordering rule between this line and the two above either. */
    the_pit_upload_rusty();
    /* ...and the bars', which this room does not own but is the only room that
       draws. A prop module's narrow uploader, the crib's arrangement with the
       Room of Arms: one LoadImage onto the left half of x512 y256, which
       nothing else in the chapter touches. py tools/check_tex_banks.py reads
       THIS call to derive src/bars.c's CATACOMBS mask. */
    bars_upload_texture();
}

/* ---- THE SOUTH DOOR --------------------------------------------------------
   z=0, x[200,400], y[-1400,-1000] â€” in the gallery's south wall, at the near end
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

/* ---- THE NORTH DOOR --------------------------------------------------------
   z=3900, x[158,442], y[-500,0] -- at the back of the north alcove, on the PIT
   FLOOR, behind the bars. Into the NORTH CHAMBER (src/north_chamber.h), through
   that room's south door.

   An XY-plane door at fixed Z approached from -Z (the alcove is on its -Z
   side), so TEXT_PLANE_XY with mirror=0 and the sign 11 proud of the wall along
   -Z: the opposite pair from the south door, and the -200 still on X. Its Y is
   the PIT FLOOR's eye-level -186, not the gallery's -1186.

   NO STOREY TEST, on the south door's argument: the nearest gallery to it is
   the east arm at x=2099, 1799 away in plan against a 500 trigger radius.

   THE BARS ARE WHAT GATE IT, NOT THIS CODE. Dropped, they stand across the
   alcove's mouth at z~3270 and bars_collide() holds the player 600-odd short
   of the door, outside the trigger radius. The door is reachable exactly when
   the ambush has lifted them. */
#define PIT_NORTH_X            300     /* the art spans x[158,442] */
#define PIT_NORTH_Z           3900
#define PIT_NORTH_TEXT_Y     (-186)    /* eye level on the y=0 pit floor */

/* Circle edge-detect, one per interaction. Seeded "held" by the arm below so a
   press carried in through the transition cannot fire on the arrival frame. */
static int south_circle_prev   = 1;
static int north_circle_prev   = 1;
static int descend_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void the_pit_arm(void) {
    south_circle_prev   = circle_held();
    north_circle_prev   = south_circle_prev;
    descend_circle_prev = south_circle_prev;
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts â€”
   hatch_puzzle_update()'s rule, for its reason.

   NO Y TEST, AND THE HEADER ARGUES IT AT LENGTH: this room's walkable surface IS
   a function of XZ, because the gallery and the pit have disjoint footprints, so
   the plain Manhattan test every other door in the game makes is correct here.
   The Up Down Maze's two doors are the only ones that need more. The descent
   does not change that (the header says why); a walkable ramp would. */
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

int the_pit_north_door_triggered(int lock) {
    return door_triggered(lock, PIT_NORTH_X, PIT_NORTH_Z, &north_circle_prev);
}

/* A floating sign â€” a door's, or the descend prompt's. Same shape as every other
   sign in the game: opaque within PIT_FADE_NEAR, gone by PIT_TEXT_RADIUS.

   `standoff` is the sign's offset off the wall toward the player and `mirror` is
   which way the glyphs read, and THE TWO ALWAYS AGREE: for an XY-plane sign, +11
   with mirror=1 for one approached from +Z, -11 with mirror=0 for one approached
   from -Z. (That is the opposite pairing from the Tomb's YZ doors.) They are
   passed as a pair rather than derived here so each call site states its own. */
static void pit_sign_text(RenderContext *ctx, const char *str,
                          int32_t door_x, int32_t door_z,
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
       sign reads along X â€” hence the -200 on THAT argument. */
    door_draw_string_3d(ctx, str,
                        door_x - 200, text_y, door_z + standoff,
                        50, 255, 50, fade, mirror, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

/* ---- THE DESCENT -------------------------------------------------------------
   How the player gets from the gallery to the pit floor: down the slope, the
   straight chute that forms the shaft's south face in the VISUAL mesh, from the
   ledge's rim (z=600, y=-1000) to the pit's south edge (z=1099, y=0). 1000 down
   over 499 across, about 63 degrees, the same section at every x in
   [-692,1292]. The collision proxy does not have it and does not need it: the
   player never walks on it, because the whole trip is a scene.

   THE PROMPT stands on the rim straight in front of the south door, at the
   door's own x and the gallery's eye height, facing back across the ledge at a
   player who has just walked in. XY plane approached from -Z, so mirror=0.
   There is no wall to stand it off, so its standoff is 0.

   >>> IT TESTS THE STOREY, WHICH THE DOOR DOES NOT. <<< The pit floor is
   inside the prompt's radius: (300,1294), the nearest the player can stand
   down there, is 694 from (300,600) against a 1200 text radius. Without the
   test the sign would hang 1000 units over the head of a player already in the
   pit. The trigger's 500 radius does not reach the pit (694 again), but it
   takes the same test anyway, so the two cannot disagree about a frame.

   THE SHOT, all @ 60fps, 4.0 s:
     0.0 s  PAUSE    1.0 s, nothing moves. The press has been taken.
     1.0 s  ANGLE    1.0 s, eased both ends. The view tilts down to PD_PITCH and
                     the yaw squares up to due north, down the slope. The eye
                     steps up to the front of the ledge (PD_TOP_Z) and settles
                     to the gallery's standing height on the way.
     2.0 s  HOP      0.4 s. Over the lip and onto the slope: straight across in z,
                     a straight drop to PD_LAND_Z's surface, and PD_HOP_RISE of
                     hump on top so it reads as a jump and not as being carried
                     (src/hatch_arrival.c's rule).
     2.4 s  SLIDE    0.9 s down the chute, ACCELERATING. The eye is held
                     standing-height above the slope surface at its z, so it
                     follows the chute exactly.
     3.3 s  RUNOUT   0.7 s across the pit floor, decelerating, starting at the
                     speed the slide ended at. The eye dips PD_LAND_DIP and comes
                     back up (the knees taking the landing) and the pitch eases
                     back to level. The view distance starts opening here.
     4.0 s  ...and THE AMBUSH follows without a cut; it has its own block below
                     (HOLD, DROP, AMBUSH, about 2.3 s). Then the camera is the
                     player's again, some 6.3 s after the press.

   THE SPEEDS MATCH AT THE JOINS, and the numbers are chosen for it. The slide's
   curve is half linear, half quadratic in p (0..256): travel = D*(p/2 + pÂ²/2),
   so it leaves at D/(2T) and arrives at 3D/(2T). D=439 (660 -> 1099) and T=54
   give 4 units a frame at the top and 12.2 at the bottom. The run-out is an
   ease-out, which starts at 2R/T: R=256 over T=42 is 12.2 again, so the body
   carries straight off the chute onto the floor without a hitch in plan. The
   HOP arrives faster than the slide leaves (10.8 a frame against 4), which is
   the feet biting as they land on the slope, and it is meant.

   WHERE IT STOPS: z=1355. Wall 17, the pit's south face at z=1099, pushes to
   z >= 1099 + PIT_WALL_RADIUS = 1294, so 1355 leaves 61 clear. That is the
   arrival-spawn rule: the first frame the player owns must not shove them. x is
   whatever it was at the press, clamped into [PD_MIN_X, PD_MAX_X], which is
   inside the pit's walkable x[-497,1097] with the same margin; the trigger only
   reaches x(-5,605) anyway.

   THE BODY IS ANCHORED AT THE LANDING for the whole scene, as src/hatch_arrival.c
   anchors its own, and for its reason: anything that hunts the player during
   the trip should hunt where they are about to be standing, and a body sliding
   down a wall that has no collision is nothing an enemy should be able to reach.

   THE SOUNDS ARE THE PLAYER'S OWN FOOTSTEPS, both RESIDENT: one on the push-off,
   one as the feet hit the slope and one as they hit the floor. SPU RAM is full
   (tools/ADDING_A_SOUND.txt), so a slide scrape is a budgeting job, not a line
   here. */
#define PIT_DESCEND_X          PIT_SOUTH_X   /* in front of the south door      */
#define PIT_DESCEND_Z          600           /* the rim: the top of the slope   */

#define PD_SLOPE_TOP_Z         600
#define PD_SLOPE_BOT_Z        1099

#define PD_T_PAUSE              60    /* 1.0 s */
#define PD_T_ANGLE              60    /* 1.0 s */
#define PD_T_HOP                24    /* 0.4 s */
#define PD_T_SLIDE              54    /* 0.9 s */
#define PD_T_RUNOUT             42    /* 0.7 s */

#define PD_PITCH               512    /* 45 deg down, in the 4096-unit turn     */
#define PD_TOP_Z               400    /* the front of the ledge's walkable band */
#define PD_LAND_Z              660    /* where the hop meets the slope          */
#define PD_END_Z              1355    /* the run-out's end; see above           */
#define PD_HOP_RISE             60    /* a slight hop                           */
#define PD_LAND_DIP             40    /* the knees, on the floor                */
#define PD_MIN_X             (-400)
#define PD_MAX_X              1000

/* The eye's height over whatever it is standing on. */
#define PD_EYE_OFF            (GROUND_FLOOR_Y + 40)

/* ---- THE AMBUSH --------------------------------------------------------------
   Where the descent used to hand control back, it keeps the camera:

     HOLD     1.0 s  Standing where the run-out stopped, level, facing due north
                     up the pit â€” straight at the north alcove and the BARS
                     hanging over its mouth. Nothing moves. The view distance
                     finishes opening (PIT_VIEW_SCENE, at the top of the file),
                     so the far end of the pit is lit for the first time.
     DROP   ~0.8 s  The bars fall into the alcove's mouth and bounce twice
                     (src/bars.c does the physics and the SFX_SLAM). Runs until
                     bars_settled(), not for a fixed time, so retuning the
                     bounce in bars.h cannot cut it short.
     AMBUSH ~1.0 s  The two Lumberers world.c seeds for this room DROP IN, one
                     either side of the bars: they appear PIT_AMBUSH_DROP above
                     their marks, out of shot, already falling, and land half
                     a second later (their shadows reach the floor first). Each
                     roars as it lands and goes ALERT (lumberers_ambush,
                     LMB_DROP). The camera holds until the last one is down and
                     half a second more, so the landing is seen. Then control.

   THE BARS. Bars.smx was authored IN PLACE in this room's Blender scene, and
   its own coordinates say where it goes: x[-130,730] z[3245,3295], standing on
   the pit floor â€” across the north alcove's mouth, x[-125,725] at z=3301 in
   the visual mesh, with 5 to spare either side and 6 in front of the jambs.
   PIT_BARS_X/Z are that box's centre and nothing else. The alcove's mouth is
   1000 tall (y 0 to -1000) and the bars are 860, so down they leave 140 of dark
   above them and seal the way to the north door completely.

   RAISED, THEY HANG PIT_BARS_LIFT ABOVE THAT â€” "directly up, above head
   height". 450 puts their base at y=-450, 231 over the player's head (eye
   -189, head -219), and their top at -1310, in front of the shaft's north wall
   above the alcove. bars_collide() gates on the CURRENT base, so raised they
   block nothing at all.

   ONE-SHOT PER ENTRY, AND THAT IS THE ONLY SHAPE THE ROOM ALLOWS. The descent
   is one-way and the pit has no exit yet, so the only way to see this twice is
   to die and walk back in, and the_pit_init() re-places the bars raised and
   the_pit_after_world_enter() re-stows whichever Lumberers and Crawlers are
   still alive. Dead ones stay dead (world.c), so a player who killed both
   Lumberers before dying goes straight to the Crawlers once the bars land.
   >>> AND NOW THAT THE NORTH DOOR IS BUILT, IT REMEMBERS. <<< A later descent
   would otherwise drop the bars again on an empty pit with no wave left to lift
   them. The memory is NOT a GameFlag -- that word has one bit left -- but the
   four enemies themselves: world.c keeps the dead dead, so "no Lumberer and no
   Crawler alive in this room" after world_enter IS "the ambush has been won",
   and the_pit_after_world_enter() starts the encounter at PIT_ENC_DONE on
   exactly that test. The descent then skips HOLD/DROP/AMBUSH and the bars are
   never dropped. A player who slips under the bars mid-rise and leaves north
   comes back to the same answer, since all four were dead before the rise
   began. */
#define PIT_BARS_X             300     /* (-130 + 730) / 2 */
#define PIT_BARS_Z            3270     /* (3245 + 3295) / 2 */
#define PIT_BARS_LIFT          450

#define PD_T_HOLD               60    /* 1.0 s */
#define PD_T_AMBUSH             30    /* 0.5 s */
#define PD_T_DROP_MAX          180    /* 3.0 s: a safety net, see PD_DROP */
#define PD_T_LAND_MAX          180    /* 3.0 s: the same net for the landing */

/* How far above its standing anchor each Lumberer appears (lumberers_ambush).
   It has to start OUT OF SHOT: the camera is level at eye -189 and they land
   ~1645 in front of it, where the top edge of a 240-line screen at
   GeomScreen 256 is about y=-960. 1200 puts the feet at -1199 and the head at
   -1719, clear of the frame and under the vault at -1800. Already at
   twice MAX_FALL_VEL (LMB_DROP_BOOST), so the fall takes 30 frames. */
#define PIT_AMBUSH_DROP       1200

static int     pit_bars  = -1;        /* bars_place()'s index, or -1          */

/* Where the encounter is; see THE WAVES, AFTER THE SCENE. */
typedef enum {
    PIT_ENC_WAIT = 0,     /* the descent has not dropped the bars yet          */
    PIT_ENC_LUMBERERS,
    PIT_ENC_CRAWLERS,
    PIT_ENC_OPENING,
    PIT_ENC_DONE
} PitEncounter;
static PitEncounter pit_enc   = PIT_ENC_WAIT;
static int32_t      pit_enc_t = 0;
static int32_t pd_land_t = 0;         /* pd_t at the last frame anything fell */

typedef enum {
    PD_IDLE = 0,
    PD_PAUSE,
    PD_ANGLE,
    PD_HOP,
    PD_SLIDE,
    PD_RUNOUT,
    PD_HOLD,
    PD_DROP,
    PD_AMBUSH
} PdState;

static PdState pd_state = PD_IDLE;
static int32_t pd_t;
static int32_t pd_x;                   /* the x the whole trip runs down      */
static int32_t pd_src_x, pd_src_y, pd_src_z, pd_src_pitch;
static int32_t pd_src_rot, pd_rot_delta;

/* The slope's surface Y at z, clamped to the two floors either side of it. */
static int32_t pd_slope_y(int32_t z) {
    if (z <= PD_SLOPE_TOP_Z) return PIT_GALLERY_Y;
    if (z >= PD_SLOPE_BOT_Z) return PIT_FLOOR_Y;
    return PIT_GALLERY_Y + ((z - PD_SLOPE_TOP_Z) * (PIT_FLOOR_Y - PIT_GALLERY_Y))
                           / (PD_SLOPE_BOT_Z - PD_SLOPE_TOP_Z);
}

/* This phase's progress, 0..256. */
static int32_t pd_progress(int32_t frames) {
    int32_t p = (pd_t * 256) / frames;
    return p > 256 ? 256 : p;
}

static void pd_enter(PdState s) {
    pd_state = s;
    pd_t     = 0;
}

static void pd_start(void) {
    /* A held Circle may have swung the view, and the ANGLE glide starts from
       whatever cam_rot is, so drop the look offset BEFORE it is saved. */
    camera_look_cancel();

    pd_x = cam_x;
    if (pd_x < PD_MIN_X) pd_x = PD_MIN_X;
    if (pd_x > PD_MAX_X) pd_x = PD_MAX_X;

    pd_src_x     = cam_x;
    pd_src_y     = cam_y;
    pd_src_z     = cam_z;
    pd_src_pitch = cam_pitch;
    pd_src_rot   = cam_rot;
    /* The short way round to 0 (due north). The facing test on the trigger
       holds this inside +-60 degrees, but it costs nothing to be right. */
    pd_rot_delta = (0 - cam_rot) & 4095;
    if (pd_rot_delta > 2048) pd_rot_delta -= 4096;

    camera_anchor_player(pd_x, PIT_FLOOR_EYE_Y, PD_END_Z);
    cam_vy = 0;
    pd_enter(PD_PAUSE);
}

static void pd_finish(void) {
    cam_x     = pd_x;
    cam_y     = PIT_FLOOR_EYE_Y;
    cam_z     = PD_END_Z;
    cam_rot   = 0;
    cam_pitch = 0;
    cam_vy    = 0;
    camera_release_player();
    pd_state = PD_IDLE;
    /* The shaft closes back down around the player, on pit_view_resolve's
       ease. */
    pit_view_scene = 0;
    /* Both of the room's Circle tests seeded "held", so nothing fires on the
       frame control comes back (src/room_of_arms.c's rax_finish). */
    south_circle_prev   = 1;
    north_circle_prev   = 1;
    descend_circle_prev = 1;
}

/* One frame of the shot. */
static void pd_tick(void) {
    int32_t p, e;
    pd_t++;
    cam_vy = 0;

    switch (pd_state) {

    case PD_PAUSE:
        if (pd_t >= PD_T_PAUSE) pd_enter(PD_ANGLE);
        break;

    /* Smoothstep, 3pÂ²-2pÂ³: a head turning, which starts and stops. */
    case PD_ANGLE:
        p = pd_progress(PD_T_ANGLE);
        e = (p * p * (768 - 2 * p)) / (256 * 256);
        cam_x     = pd_src_x     + ((pd_x              - pd_src_x)     * e) / 256;
        cam_y     = pd_src_y     + ((PIT_GALLERY_EYE_Y - pd_src_y)     * e) / 256;
        cam_z     = pd_src_z     + ((PD_TOP_Z          - pd_src_z)     * e) / 256;
        cam_pitch = pd_src_pitch + ((PD_PITCH          - pd_src_pitch) * e) / 256;
        cam_rot   = (pd_src_rot  + (pd_rot_delta * e) / 256) & 4095;
        if (pd_t >= PD_T_ANGLE) {
            cam_x = pd_x; cam_y = PIT_GALLERY_EYE_Y; cam_z = PD_TOP_Z;
            cam_pitch = PD_PITCH; cam_rot = 0;
            sound_play(SFX_STEP1);            /* the push-off */
            pd_enter(PD_HOP);
        }
        break;

    /* Linear across and down, plus a 4p(1-p) hump. -Y is up, so the hump is
       subtracted. */
    case PD_HOP: {
        int32_t land_y = pd_slope_y(PD_LAND_Z) - PD_EYE_OFF;
        int32_t hump;
        p = pd_progress(PD_T_HOP);
        hump  = (4 * PD_HOP_RISE * p * (256 - p)) / (256 * 256);
        cam_z = PD_TOP_Z + ((PD_LAND_Z - PD_TOP_Z) * p) / 256;
        cam_y = PIT_GALLERY_EYE_Y + ((land_y - PIT_GALLERY_EYE_Y) * p) / 256 - hump;
        if (pd_t >= PD_T_HOP) {
            cam_z = PD_LAND_Z;
            cam_y = land_y;
            sound_play(SFX_STEP2);            /* feet on the slope */
            pd_enter(PD_SLIDE);
        }
        break;
    }

    /* Travel = D*(p/2 + pÂ²/2): moving from the first frame, and faster every
       frame after it. See THE SPEEDS MATCH AT THE JOINS above. */
    case PD_SLIDE:
        p = pd_progress(PD_T_SLIDE);
        e = (128 * p + (128 * p * p) / 256) / 256;
        cam_z = PD_LAND_Z + ((PD_SLOPE_BOT_Z - PD_LAND_Z) * e) / 256;
        cam_y = pd_slope_y(cam_z) - PD_EYE_OFF;
        if (pd_t >= PD_T_SLIDE) {
            cam_z = PD_SLOPE_BOT_Z;
            cam_y = PIT_FLOOR_EYE_Y;
            sound_play(SFX_STEP1);            /* feet on the floor */
            /* Light the far end for THE AMBUSH -- unless it is already over,
               in which case there is nothing up there to show. */
            if (pit_enc != PIT_ENC_DONE) pit_view_scene = 1;
            pd_enter(PD_RUNOUT);
        }
        break;

    /* Ease-out in z, so it carries on at the slide's speed and settles. The dip
       is a 4p(1-p) hump added (down), and the pitch levels on the same ease. */
    case PD_RUNOUT: {
        int32_t inv, dip;
        p   = pd_progress(PD_T_RUNOUT);
        inv = 256 - p;
        e   = 256 - (inv * inv) / 256;
        dip = (4 * PD_LAND_DIP * p * (256 - p)) / (256 * 256);
        cam_z     = PD_SLOPE_BOT_Z + ((PD_END_Z - PD_SLOPE_BOT_Z) * e) / 256;
        cam_y     = PIT_FLOOR_EYE_Y + dip;
        cam_pitch = PD_PITCH - (PD_PITCH * e) / 256;
        if (pd_t >= PD_T_RUNOUT) {
            /* Exactly where pd_finish() will leave the camera, so HOLD is a
               still frame and the hand-back later is not a jump. */
            cam_x = pd_x; cam_y = PIT_FLOOR_EYE_Y; cam_z = PD_END_Z;
            cam_rot = 0; cam_pitch = 0;
            /* THE AMBUSH HAPPENS ONCE. With all four of its enemies already
               dead (see the_pit_after_world_enter) the bars are up and stay up,
               and the landing hands straight back. */
            if (pit_enc == PIT_ENC_DONE) pd_finish();
            else                         pd_enter(PD_HOLD);
        }
        break;
    }

    /* ---- THE AMBUSH (see the block above the state enum) ---------------- */
    case PD_HOLD:
        if (pd_t >= PD_T_HOLD) {
            bars_drop(pit_bars);
            pd_enter(PD_DROP);
        }
        break;

    /* >>> IT HAS A TIMEOUT, BECAUSE bars_update() IS AREA-GATED. <<< Its
       gate is current_area, which a debug jump or a title load does not set
       (main.c's frontend hook puts it back to the Delivery Area), and a gated-
       out drop never settles â€” the scene then holds the camera for good. That
       was found by force-booting this room. PD_T_DROP_MAX is four times the
       real drop, so it never cuts a working one short. */
    case PD_DROP:
        /* bars_update() itself runs in pit_encounter_tick(), every frame. */
        if (bars_settled(pit_bars) || pd_t >= PD_T_DROP_MAX) {
            lumberers_ambush(STATE_THE_PIT, PIT_AMBUSH_DROP);
            pit_enc = PIT_ENC_LUMBERERS;
            pit_enc_t = 0;
            pd_land_t = 0;
            pd_enter(PD_AMBUSH);
        }
        break;

    /* Wait for both bodies to reach the floor, then PD_T_AMBUSH more so the
       landing and the roar are seen before the player can turn away.
       pd_land_t follows pd_t for as long as anything is still in the air, so
       the hold is measured from the LAST landing. Capped at PD_T_LAND_MAX for
       PD_DROP's reason: the lumberers' update is area-gated too. */
    case PD_AMBUSH:
        if (lumberers_dropping(STATE_THE_PIT) > 0 && pd_t < PD_T_LAND_MAX) {
            pd_land_t = pd_t;
            break;
        }
        if (pd_t - pd_land_t >= PD_T_AMBUSH) pd_finish();
        break;

    default:
        break;
    }
}

int the_pit_descent_active(void) {
    return pd_state != PD_IDLE;
}

/* ---- THE WAVES, AFTER THE SCENE ---------------------------------------------
   The descent's ambush is the first of three beats, and the other two run in
   free play, off this tick:

     LUMBERERS  the two that dropped in. When neither is left alive (and
                neither is still in the air), a PIT_ENC_BEAT pause â€” the death
                cry and the blood get their second â€” and then:
     CRAWLERS   two Crawlers appear at the TOP OF THE BANKS, west and east, and
                scuttle straight down them onto the pit floor, already roused:
                they scream at the bottom and rush (crawlers_ambush,
                CRW_ENTER). When neither is left alive, the same pause, then:
     OPENING    the bars winch back up to PIT_BARS_LIFT, over two plays of
                SFX_MCHNE back to back (bars_raise). Their base clears the
                player's head a little over half way, which is when the alcove
                can be walked into.
     DONE       nothing left to do.

   THE BANKS ARE SCRIPTED BECAUSE THE PROXY HAS NO BANKS. The visual mesh's
   east and west sides are one straight slope each â€” y 0 at x=-692 / 1292 up to
   y -1000 at x=-1505 / 2100, a constant 1.23 down per unit across on every
   segment â€” but the collision proxy stands a vertical wall at the pit's edge
   (16 west, 5 east) and another at the arm's inner edge (13 / 12), with no
   floor zone between. Nothing can WALK down them; the entry path is two
   straight legs that ignore collision (top -> foot of the bank -> spawn), and
   because the bank is straight, the first leg lies on its drawn surface.

   Each crawler starts 45 in from the bank's top edge (the arm wall line), at
   z=3000 â€” level with the Lumberers' marks and outside them, which is "east
   and west of where the lumberers spawn in". It ends on the floor 132 in from
   the pit wall, clear of the 90 CRW_BODY_RADIUS push, where world.c seeds it.

   This tick is also the ONE caller of bars_update(), for the drop in the scene
   and the rise out of it. It runs every frame the room is updated, locked or
   not: enemies keep running under the inventory menu, so the waves have to. */
#define PIT_ENC_BEAT            60    /* 1.0 s between a wave dying and the next */

static const CrawlerEntry pit_crawler_entry[2] = {
    /*  top x    top y  top z    foot x  foot y  foot z                     */
    { -1460,   -945,  3000,     -692,     0,   3000 },    /* west bank     */
    {  2055,   -944,  3000,     1292,     0,   3000 },    /* east bank     */
};

static void pit_encounter_tick(void) {
    bars_update();

    switch (pit_enc) {
    case PIT_ENC_LUMBERERS:
        if (lumberers_alive_in(STATE_THE_PIT) > 0 ||
            lumberers_dropping(STATE_THE_PIT) > 0) { pit_enc_t = 0; break; }
        if (++pit_enc_t < PIT_ENC_BEAT) break;
        crawlers_ambush(STATE_THE_PIT, pit_crawler_entry, 2);
        pit_enc = PIT_ENC_CRAWLERS;
        pit_enc_t = 0;
        break;

    case PIT_ENC_CRAWLERS:
        if (crawlers_alive_in(STATE_THE_PIT) > 0) { pit_enc_t = 0; break; }
        if (++pit_enc_t < PIT_ENC_BEAT) break;
        bars_raise(pit_bars, PIT_BARS_LIFT);
        pit_enc = PIT_ENC_OPENING;
        break;

    case PIT_ENC_OPENING:
        if (bars_raised(pit_bars)) pit_enc = PIT_ENC_DONE;
        break;

    default:
        break;
    }
}

int the_pit_descent_update(int lock) {
    int held, just;
    int32_t dx, dz, xz;

    pit_encounter_tick();

    if (pd_state != PD_IDLE) {
        pd_tick();
        return 1;
    }

    /* Edge state kept current even while locked, as the door's is. */
    held = circle_held();
    just = held && !descend_circle_prev;
    descend_circle_prev = held;
    if (lock || !just) return 0;

    if (cam_y >= PIT_STOREY_SPLIT_Y) return 0;     /* on the pit floor */
    dx = cam_x - PIT_DESCEND_X;
    dz = cam_z - PIT_DESCEND_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= PIT_TRIGGER_RADIUS) return 0;
    if (!interact_facing(PIT_DESCEND_X, PIT_DESCEND_Z)) return 0;

    pd_start();
    return 1;
}

void the_pit_spawn_south(void) {
    /* Arriving from the Tomb. Clear of the wall push radius on the +Z side â€” the
       walkable side of wall 19 â€” and facing +Z, the direction of travel through
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

    /* Park the descent, so no arrival can inherit a half-played one (a soft reset
       is allowed mid-scene). Its anchor goes with it: an anchor left up would
       keep every enemy hunting the bottom of the slope. */
    if (pd_state != PD_IDLE) camera_release_player();
    pd_state  = PD_IDLE;
    cam_pitch = 0;
    pit_view_scene = 0;

    /* THE BARS, raised over the north alcove's mouth, waiting for the descent
       to drop them â€” see THE AMBUSH above for the numbers. Cleared and re-placed
       on every entry, like the four arrays below: the room can only be entered
       at the top of the shaft, where the drop is always still to come. The
       floor reference is the pit floor's, y=0 less GROUND_FLOOR_Y. */
    pit_enc   = PIT_ENC_WAIT;
    pit_enc_t = 0;
    bars_clear();
    pit_bars = bars_place(STATE_THE_PIT, PIT_BARS_X, PIT_FLOOR_Y - GROUND_FLOOR_Y,
                          PIT_BARS_Z, 0, PIT_BARS_LIFT);

    the_pit_spawn_south();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every one of their collide routines â€”
       so an instance left over from another room would block the player invisibly
       anywhere its coordinates fall inside this room's bounds (x[-2104,2699]
       z[0,3900]).

       BOTH OF THE CHAPTER'S SCONCES ARE INSIDE THOSE BOUNDS, as they are inside
       the Room of Arms': the Catacombs Entry's pair is at (Â±595,200), and both
       (595,200) and (-595,200) land on this gallery's south ledge â€” in the 800 of
       it the player walks in on. Neither bites, because sconces_collide() gates on
       s->area != current_area and both instances are tagged
       STATE_CATACOMBS_ENTRY; the two arrays that are NOT gated, the save point and
       the dresser, have nothing in Chapter 3 that lands in here. So this is the
       cheap guarantee the Tomb's and the Room of Arms' four calls are, and not a
       fix â€” but it is a room with NO margin: this footprint is the largest in the
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

/* After world_enter(), which seeds this room's two Lumberers and two Crawlers
   on the first visit and restores them on every later one â€” ACTIVE either way.
   They are an ambush, so every one still alive is put away until its wave. Called
   from main.c's post-world_enter block; the_pit_init() is too early, because
   world_enter runs after it. */
void the_pit_after_world_enter(void) {
    lumberers_stow_area(STATE_THE_PIT);
    crawlers_stow_area(STATE_THE_PIT);
    /* Both counts include stowed bodies and exclude only the dead, so this is
       zero exactly when every enemy the ambush owns has been killed -- on this
       visit or on any earlier one, and across a save. See THE AMBUSH. */
    if (lumberers_alive_in(STATE_THE_PIT) == 0 &&
        crawlers_alive_in(STATE_THE_PIT) == 0)
        pit_enc = PIT_ENC_DONE;
}

void the_pit_spawn_north(void) {
    /* Arriving back from the North Chamber: in the north alcove on the pit
       floor, 220 off the door's wall on its -Z side, facing -Z -- down the pit
       toward the slope the player came down. The pit floor's eye, not the
       gallery's: the_pit_init()'s default spawn is the gallery one. */
    cam_x   = PIT_NORTH_X;
    cam_y   = PIT_FLOOR_EYE_Y;
    cam_vy  = 0;
    cam_z   = PIT_NORTH_Z - (PIT_WALL_RADIUS + 25);
    cam_rot = 2048;                    /* facing -Z, south down the pit */
    the_pit_arm();
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
           advance â€” so a rejected primitive never touches the SMD header, the
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
           player sees along the gallery AND down into the pit in one frame â€” so
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

    /* >>> LEVEL 8 REMOVES THE SIGNS, THE BARS AND THE AMBUSH. <<< The room
       holds one 34-primitive prop (the bars) and two Lumberers that are stowed
       until the descent's drop, so from the gallery the D reading at levels 1, 4
       and 8 splits its frame between the mesh and TWO floating strings (the
       door's and the descend prompt, both on the south ledge) â€” which is
       exactly the case STEP 3D of tools/DIAGNOSING_FRAME_RATE.txt was written
       about. Reception's frame turned out to be its SIGNAGE and not its mesh,
       because door_draw_string_3d has no facing test and queues every glyph in
       full with the player's back to it. With one sign in a 4800 room that is
       cheaper here than it was there, but it is still the second thing to measure
       and not the last. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        pit_sign_text(ctx, "Press " BTN_CIRCLE " to enter",
                      PIT_SOUTH_X, PIT_SOUTH_Z, PIT_SOUTH_TEXT_Y,
                      +11, 1);   /* south: XY plane, approached from +Z */
        pit_sign_text(ctx, "Press " BTN_CIRCLE " to enter",
                      PIT_NORTH_X, PIT_NORTH_Z, PIT_NORTH_TEXT_Y,
                      -11, 0);   /* north: XY plane, approached from -Z */
        /* The descend prompt: gallery only (see THE DESCENT), and gone while
           the scene runs, since the hop carries the eye straight through it. */
        if (!the_pit_descent_active() && cam_y < PIT_STOREY_SPLIT_Y)
            pit_sign_text(ctx, "Press " BTN_CIRCLE " to descend",
                          PIT_DESCEND_X, PIT_DESCEND_Z, PIT_SOUTH_TEXT_Y,
                          0, 0);  /* rim: XY plane, approached from -Z */
        /* BOTH CHAPTER 3 ENEMIES, and both are drawn in all six of its rooms
           whether or not world.c places one here. That is deliberate and it is
           what "either enemy may go in any Catacombs room" actually costs: the
           area tag makes an absent enemy free (the loop skips every instance whose
           area is not current_area), and a placement then starts drawing without
           this file having to be touched again.

           >>> AND IN THIS ROOM A PLACEMENT HAS TO PICK A STOREY, which is new.
           <<< world_seed_room() authors coordinates as literals and cannot read
           this room's floor zones, so an enemy meant for the gallery needs y
           authored against -1000 and one meant for the pit against 0. The
           ambush's two are pit-floor placements (anchor -149), and they are
           stowed until the player is down there with them. src/the_pit.h's
           layout list is the reference for which is which.

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
        /* The bars over the north alcove. Inside the entity gate, as the Room
           of Arms' crib is, and AFTER the two enemy calls for a stronger reason
           than the crib's: this prop's UVs run past 127 and only tile because
           the 128 window wraps them, and the enemies' sprite code is what puts
           that window back after drawing unmasked (src/bars.h). */
        bars_draw(ctx);
    }
}
