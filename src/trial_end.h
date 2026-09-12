#ifndef TRIAL_END_H
#define TRIAL_END_H

#include "render.h"

/* ============================================================================
   THE TRIAL-END SCREEN — where this build of the game stops
   ============================================================================
   Armed on ONE frame only: the one where The Hatch's descent reaches the bottom
   of the shaft (main.c's hatch branch, at hatch_puzzle_drop_done()). The drop
   used to hand off to ASAG'S ARENA; the arena, its boss, its meshes and its
   textures are ALL STILL HERE and still build. There is simply no longer a way
   in, and this screen is what the shaft lands on instead.

   >>> AND IT IS THIS SCREEN THAT IS BOXED OUT NOW, NOT THE FIGHT. <<< The Asag
   arena is back on the end of the drop, so NOTHING CALLS trial_end_start()
   ANY MORE. trial_end_active() is therefore false forever, main.c's branch that
   owns the frame for this screen is dead code, and every function below is
   unreachable. All of it is kept, and kept COMPILING, because the screen itself
   is fine and the build may want it again.

   TO BRING IT BACK: call trial_end_start() from somewhere. The natural place is
   the one it used to live in - src/main.c at hatch_puzzle_drop_done(), in place
   of the transition into STATE_ASAG_ARENA - and that single line is the whole
   of it. reset_game() already calls trial_end_reset(), the draw branch is still
   in main(), and the sign-off's CD track is still on the disc.

   The paragraph below is the original note from when the gating ran the other
   way. It is kept because it is still the map of where the two switches are.

   ---------------------------------------------------------------------------
   >>> THE ASAG FIGHT IS LOCKED OUT, NOT DELETED. <<< Two places gate it, and
   they are the two to undo if it ever comes back: the arming call in main.c's
   hatch branch, and the ASAG ARENA row that was taken out of title.c's
   level-select tables. Nothing else about the arena was touched.

   It was written for the trial build, where it was armed by the Rabisu's death
   instead (rabisu_boss.c, at RBE_D_CAM_BACK -> RBE_DONE). That is no longer the
   end of anything, so the death sequence just ends the encounter now.

   The beat sheet, as briefed:

     fade      4 s from the shaft to purple, with the Anzu track coming in
     text      the sign-off fades on a line at a time, the way the opening
               sequence's blocks do
     start     PRESS START TO RETURN — and Start does exactly what it does on
               the game-over screen: reset_game() and back to the title

   THE FADE IS TWO BLENDS, NOT ONE. There is no alpha on the PS1: a full-screen
   SUBTRACTIVE tile (ABR=2) is what takes a live 3D scene to true black — the
   delivery arrival's fade, in reverse — and a full-screen ADDITIVE tile (ABR=1)
   of the title's purple is what brings the colour up underneath it. Ramped
   together on one counter they read as a single 4-second dissolve to purple, and
   at the end of it the screen is EXACTLY the title's purple, so the swap to the
   flat clear colour on the following frame is invisible.

   THE TEXT IS THE OPENING SEQUENCE'S FONT (intro.h). Not the SDK's: FntSort
   draws at one fixed brightness and cannot be faded at all.                   */

/* Arm it. Called from main.c the frame the descent reaches the bottom. */
void trial_end_start(void);

/* 1 while this screen owns the game: no menu, no HUD, no world update. */
int  trial_end_active(void);

/* 1 while the room behind it should still be drawn — the 4-second fade only.
   Past that the screen is flat purple and drawing the room would be a few
   hundred wasted primitives under an opaque wash. */
int  trial_end_world_visible(void);

void trial_end_update(void);
void trial_end_draw(RenderContext *ctx);

/* 1 once, on the frame Start is pressed. main.c answers it the way it answers
   the game-over screen: reset_game() and STATE_TITLE. */
int  trial_end_finished(void);

/* Forget it. In reset_game(), beside the other cutscene resets. */
void trial_end_reset(void);

#endif
