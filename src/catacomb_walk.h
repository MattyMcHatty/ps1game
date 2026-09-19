#ifndef CATACOMB_WALK_H
#define CATACOMB_WALK_H

#include "render.h"

/* =========================================================================
   THE WALK INTO THE CATACOMBS — the transition between Chapters 2 and 3
   =========================================================================
   Every other level change in this game is a DOOR: a leaf swings at the camera
   and the screen goes black behind it (src/door_anim.h). Two are not — the
   stair climb (src/stair_anim.h) and the fall down The Hatch's shaft
   (DOOR_PANEL_FALL) — and this is the third, for the reason those two exist:
   what happens here is not a door opening.

   The doors are ALREADY OPEN. They were slid apart by the scene that ends
   Asag's fight (src/catacomb_open.h) and they have been standing open ever
   since. So there is nothing to swing: the player simply walks between them,
   and the point of the shot is that the two slabs pass either side of the
   camera and there is nothing behind them but the walk.

   THE SHOT:
     0.00 s  the two leaves, lit and still, seen from CW_Z_FAR back — far
             enough that both are comfortably in frame with the gap between
             them open down the middle. Fading up from black.
     0.70 s  fully up, and held for a beat.
     1.10 s  THE WALK. CW_STEP_COUNT paces forward over CW_STEP_FRAMES each,
             each one eased so it lands rather than glides, with SFX_STEP1 and
             SFX_STEP2 alternating on the footfalls — the player's own
             footsteps, which are SND_RESIDENT and so are still loaded whatever
             bank the room being left was on.
     5.90 s  the sixth pace lands, the leaves are long past the edges of the
             screen, and the fade to black starts.
     6.80 s  black, and the cut to STATE_LOADING.

   ---- WHY IT DRAWS ITSELF RATHER THAN BORROWING door_anim ------------------
   door_anim's panels are FLAT: a quad on the screen that swings by moving its
   corners. That is right for a door, because a door you are about to walk
   through fills the frame. These two are objects in a space the camera moves
   through, several hundred units apart, and the whole read depends on them
   separating as the camera closes on them — which is perspective, not a swing.
   So this file projects by hand, exactly as DOOR_PANEL_FALL does and with the
   same focal length:

       screen = CENTRE + world_offset * CW_FOCAL / z

   CW_FOCAL is 256 to match gte_SetGeomScreen(256), which is the projection the
   rest of the game runs at, so the leaves are the size here that they are in
   the room.

   ---- THE GEOMETRY IS THE ROOM'S OWN -----------------------------------------
   The leaves are src/catacomb_doors.h's pair in their FULLY OPEN pose, which is
   the pose the player is looking at when they press Circle. Shut they span
   x[-500,0] and x[0,500]; open they have each slid CD_SLIDE_FULL outward, so
   they stand at x[-1000,-500] and x[500,1000] with the 900-wide mouth between
   them. Both run y[-910,0] — ground to lintel. Those numbers are lifted
   straight from that header and must move with it.

   ---- THE TEXTURE IS ALREADY IN VRAM, AND NOTHING HAD TO BE LOADED ---------
   `lamashtu tablet` at x768 y0, put there by outside_catacombs_upload_textures()
   on the way into the room this transition leaves. A transition draws BEFORE
   STATE_LOADING, so the outgoing room's art is still up and the incoming
   room's has not been stamped yet — the same window DOOR_PANEL_GREENHOUSE
   relies on, and the opposite of DOOR_PANEL_FALL's problem (its mud belongs to
   the room being entered, which is why that one alone reads off the CD at start
   time). The tpage/clut are compile-time constants from src/tim_slots.h.

   >>> AND IT IS DRAWN BEFORE THE CHAPTER PURGE, WHICH IS NOT LUCK. <<<
   area_bank_sync() frees the Outside Catacombs' copy of this very texture
   (src/area_bank.h). It runs inside STATE_LOADING, i.e. after the last frame this
   file draws. If this transition is ever moved to run after a purge, it will
   have to take the Catacombs' own copy of the tablet instead — the entry room
   registers one (src/catacombs_entry.c), so it is there, but it is not loaded
   until the purge has happened.

   Usage, the same shape as stair_anim:
     catacomb_walk_start()   - begin; silences the monsters being left behind
     then game_state = STATE_CATACOMB_WALK and, each frame in that state:
       catacomb_walk_update(); catacomb_walk_draw(ctx);
       if (catacomb_walk_finished()) game_state = STATE_LOADING;
   ========================================================================= */

void catacomb_walk_start(void);

/* 1 from the frame catacomb_walk_start() runs until the transition ends — the
   same answer, for the same callers, as door_anim_active().

   >>> IT EXISTS BECAUSE THE TRIGGER FRAME IS NOT COVERED BY ANYTHING ELSE. <<<
   The door-trigger branches are not the last thing in update_current_area, so
   the SHARED ENTITY TAIL below them still runs once on the frame the
   transition starts — after world_silence_monsters() has already cut every
   sound. update_rafflesias() and update_hadads() would then see their latches
   clear and key their loops straight back on, for the whole of the seven
   seconds this transition runs and into the room beyond it, where the sample
   under them no longer exists. Both carry a door_anim_active() guard written
   for exactly that bug; this is what lets them ask the same question about this
   transition. See the long note at the top of update_hadads().

   (stair_anim has the same hole and no accessor. It has never been hit because
   nothing that loops a sound is placed in the two rooms it runs between — but
   that is a fact about where enemies happen to be placed, not a property of the
   transition, and the day a Living Statue goes in the conservatory it wants the
   same treatment.) */
int  catacomb_walk_active(void);
void catacomb_walk_update(void);
int  catacomb_walk_finished(void);   /* 1 once it has fully faded out */
void catacomb_walk_draw(RenderContext *ctx);

#endif
