#ifndef HATCH_DOORS_H
#define HATCH_DOORS_H

#include <stdint.h>
#include "render.h"

/* THE HATCH DOORS: the two animated leaves that cover the pit in The Hatch's
   yard, and the game's FIRST animated PROP. The Rabisu's idle flap was the first
   baked-vertex animation of any kind (tools/ANIMATING_A_3D_MODEL.txt); this is
   the same machinery with the clip played ONCE, forwards, on a button press
   instead of looped forever.

   >>> IT IS NOT THE LID ON THE WELL. <<< The Hatch has two things called a
   hatch: the `hatch` leaf standing on the brick well in the NORTH CHAMBER, which
   is part of the room mesh and does not move, and THIS pair, over the 1200 x 600
   hole in the middle of the yard at x(3000,4200) z(-300,300). Only the pair is
   in this file. They share the room's `hatch` texture and nothing else.

   ---- THE TWO CLIPS ----------------------------------------------------------
   One .smd and one .pva per leaf, all four out of "blender models/Garden/Hatch
   Doors.blend", exported at scale 100 like every other model in the game:

     HATCHDL.SMD / HATCHDL.PVA   Hatch_Left,  32 verts, 30 polys, 10 frames
     HATCHDR.SMD / HATCHDR.PVA   Hatch_Right, 40 verts, 38 polys, 10 frames

   FRAME 1 IS SHUT, FRAMES 2-9 ARE THE SWING, FRAME 10 IS OPEN — a ONE-SHOT, so
   the loop-trim the exporter applies to a cycle was suppressed with
   --no-trim-loop and the playback below STOPS on the last frame rather than
   wrapping to the first. Both were verified against their .smd before wiring:
   matching vertex counts and a bind-pose delta of 0, which is what says the two
   files came off the same mesh (the runbook's mistake #5).

   >>> THE RIGHT LEAF IS STILL ~246 UNITS OFF THE GROUND ON FRAME 10. <<< The
   left one comes to rest flat (its last frame spans y[-47,15]); the right one is
   authored a little short of that and finishes leaning. That is the ART, not a
   playback bug — the clip is played to its end and held. Re-bake the .pva if it
   should settle.

   ---- WHERE THEY SIT, AND WHY THE NUMBER IS THE PIT'S CENTRE ------------------
   Both leaves are baked in a SHARED space whose origin is the middle of the
   hole: shut, the left spans x[-620,0] and the right x[0,620], and both span
   z[-310,310]. So ONE translation places the pair, and it is the pit's centre.

   The pit is x(3000,4200) z(-300,300), 1200 x 600. The leaves are 1240 x 620.
   Placed at x=3600 they cover it with a 20-unit lip on all four sides — they
   rest ON the lawn round the hole rather than dropping into it, which is what
   the extra 20 is for and why the pair is not scaled to the hole.

   THE PIVOT EDGES LAND ON THE HOLE'S EDGES, which is the whole reason the
   translation is not a free parameter. Each leaf swings about its OUTSIDE edge:
   the left about x=-620 (world 2980, the hole's west lip) and the right about
   x=+620 (world 4220, its east lip), and each falls outward onto the lawn. Move
   HATCH_DOORS_X and both hinges move off the hole with it.

   HATCH_DOORS_Y is -15 and not 0: the slab is 30 thick, centred on its own y=0,
   so translating it by half that stands it ON the y=0 lawn (world y[-30,0])
   instead of half-sunk in it. PS1 y is DOWN-positive, hence the minus.

   ---- SOLID, AND THE FOOTPRINT COMES OUT OF THE CLIP -------------------------
   hatch_doors_collide() pushes the player out of each leaf's CURRENT frame,
   scanned off the clip rather than hard-coded, so the leaves are solid all the
   way through the swing and not only at its two ends. Open, the left leaf lies
   across x[2360,2981] and the right across x[4214,4800], both z[-310,310] — the
   player is stopped by them instead of walking through. See the long note above
   hd_leaf_box in the .c for why the box is a fair description of a leaf at any
   angle, and why there is no vertical gate and no line-of-sight test.

   >>> IT ADDS TO THE PIT'S FENCE AND DOES NOT REPLACE IT. <<< Collision walls
   20..23 face outward on all four sides of the hole and keep the player off it
   whatever the doors are doing (see the_hatch.h). A shut leaf's box is that same
   footprint plus the 20-unit lip, so on the shut frame this is very nearly a
   no-op — which is the point: the doors being solid is not what keeps anyone out
   of the hole. >>> DO NOT MAKE THE PIT A FLOOR ZONE TO STAND ON THE SHUT DOORS.
   <<< The single FLOOR_FLAT zone that already covers the hole is what holds the
   player at lawn height while they lean over the lip, and there is nothing under
   it if the fence ever comes down.

   ---- MEMORY: LOADED PER ROOM, NOT AT STARTUP --------------------------------
   The four files are 12 KB once read_file has rounded them to sectors, and
   tools/heap_budget.py reports 262 KB free at rest against a measured boot cliff
   of 233-235 KB and a "treat under 256 KB as trouble" line. Spending 12 KB of
   that permanently on a prop in one room at the end of one branch is exactly the
   trade tools/ANIMATING_A_3D_MODEL.txt's budget section says not to make, so
   these load on ENTRY into The Hatch and are freed on the way into any other
   room — hatch_doors_load() from the_hatch_load_geometry(), and
   hatch_doors_unload() from the top of main.c's load_area_geometry(), which runs
   on every transition. Nothing here is resident at startup and nothing here is
   near the startup peak that the cliff is actually about. */

/* The pit's centre, and the pair's shared origin. See above before moving it. */
#define HATCH_DOORS_X   3600
#define HATCH_DOORS_Y    (-15)   /* half the slab: stands it ON the y=0 lawn */
#define HATCH_DOORS_Z       0

/* GAME FRAMES PER ANIMATION FRAME. The game runs at 60, so 6 plays the clip at
   10 fps and the whole swing takes 9 x 6 = 54 frames, a bit under a second.
   The clip was AUTHORED at 24, so this is not period-correct playback the way
   the Rabisu's idle is (60/3 = 20 against an authored 24) — it is deliberately
   slower than authored, because ten frames of a heavy pair of doors reads as
   weight at 10 fps and as a flicker at 20. Snapping between baked frames, no
   interpolation. */
#define HATCH_DOORS_ANIM_TICKS  6

void hatch_doors_load(void);     /* ROOM ENTRY: read both .smd and both .pva  */
void hatch_doors_unload(void);   /* leaving: free all four. Safe if not loaded */

/* Room entry, after the flag is restored: poses the pair shut or open to match
   FLAG_HATCH_DOORS_OPEN and arms the Circle edge state. */
void hatch_doors_init(void);

/* Per frame. Fires the swing on a fresh Circle press in reach of the west lip
   while they are still shut, and advances a swing already running. Returns 1 on
   the frame the press is taken, so the caller can play off it.

   `lock` is main.c's menu lock, and it is passed IN rather than checked at the
   call site (the way grinder_puzzle_update takes it) because the two halves of
   this function want opposite answers: a swing already running keeps ticking
   under an open menu, as every other animation in a locked area does, while the
   PRESS that starts one is a player interaction and is refused. Calling this
   only when unlocked would freeze a hatch mid-travel for as long as the player
   left the inventory up. */
int  hatch_doors_update(int lock);

void hatch_doors_draw(RenderContext *ctx);   /* the pair, at their frame  */
void hatch_doors_text(RenderContext *ctx);   /* the floating prompt       */

/* Push the player out of whichever leaf they are standing in, at this frame's
   pose. Area-gated to The Hatch, so the shared reception collision routine calls
   it unconditionally like every other prop family in there. */
void hatch_doors_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

int  hatch_doors_open(void);      /* 1 once the swing has finished          */
int  hatch_doors_swinging(void);  /* 1 while it is running                  */

#endif
