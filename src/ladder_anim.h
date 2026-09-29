#ifndef LADDER_ANIM_H
#define LADDER_ANIM_H

#include "render.h"

/* =========================================================================
   THE LADDER CLIMB — the transition between the North Chamber and the Cleaver
   Corridor
   =========================================================================
   The stair climb's sibling (src/stair_anim.h), and it differs from it in one
   thing: the stair climb lurches UP AND FORWARD, which it shows by sliding its
   one image AND zooming it. A ladder goes straight up. So there is no zoom here
   at all — the player's face stays the same distance from the rungs for the
   whole shot — and the only motion is vertical.

   THE SHOT:
     0.00 s  the ladder, filling the middle of the screen, fading up from black.
     0.50 s  fully up, held for a beat: hands on the rungs.
     1.25 s  THE CLIMB. LA_STEP_COUNT pulls, each one eased out so it lurches
             and settles, the ladder sliding DOWN the screen (climbing up) or UP
             it (climbing down) by two rungs a pull. A footfall on each.
     4.25 s  the last pull lands and the fade to black starts.
     5.25 s  black, and the cut to STATE_LOADING.

   ---- MULTIPLE POLYS, ONE ON TOP OF ANOTHER ---------------------------------
   The ladder is a COLUMN of identical tiles stacked up the screen, each one a
   whole copy of LADDER.TIM (a 128 tile carries four rungs over brick). The
   column is always long enough to cover the screen and scrolls by whole-tile
   wrap, so however far the climb goes there is never an end to the ladder in
   shot. Each tile is Gouraud-shaded brightest at the screen's centre and
   falling off toward the top and bottom, so the shaft reads as lit by the
   player's own light rather than as a flat picture of a ladder.

   ---- THE TEXTURE IS ALREADY IN VRAM ------------------------------------------
   LADDER.TIM at x704 y256. BOTH rooms this runs between draw the ladder, and
   both put it up on entry (north_chamber_upload_textures() owns it;
   cleaver_corridor_upload_textures() calls north_chamber_upload_ladder()). A
   transition draws BEFORE STATE_LOADING, so whichever room is being LEFT has
   it up — the same window the stair climb and the catacomb walk rely on. The
   tpage/clut are compile-time constants from src/tim_slots.h.

   Usage, the same shape as stair_anim:
     ladder_anim_start(LADDER_UP or LADDER_DOWN)  - silences the monsters left
                                                    behind
     then game_state = STATE_LADDER_ANIM and, each frame in that state:
       ladder_anim_update(); ladder_anim_draw(ctx);
       if (ladder_anim_finished()) game_state = STATE_LOADING;
   ========================================================================= */
#define LADDER_UP    0   /* North Chamber -> Cleaver Corridor (climb up)   */
#define LADDER_DOWN  1   /* Cleaver Corridor -> North Chamber (climb down) */

void ladder_anim_start(int direction);

/* 1 from the frame ladder_anim_start() runs until the transition ends: the
   question door_anim_active() and catacomb_walk_active() answer, for the same
   callers (the looped-sound guards in update_hadads and update_rafflesias — see
   src/catacomb_walk.h for why the trigger frame needs one). */
int  ladder_anim_active(void);
void ladder_anim_update(void);
int  ladder_anim_finished(void);   /* 1 once it has fully faded out */
void ladder_anim_draw(RenderContext *ctx);

#endif
