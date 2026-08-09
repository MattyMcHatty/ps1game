#ifndef TRIAL_END_H
#define TRIAL_END_H

#include "render.h"

/* ============================================================================
   THE TRIAL-END SCREEN — where this build of the game stops
   ============================================================================
   Armed on ONE frame only: the one where the Rabisu encounter's death sequence
   hands the camera back and retires (rabisu_boss.c, RBE_D_CAM_BACK -> RBE_DONE).
   The encounter's own bail-out path — the boss vanishing out from under the
   script on a debug jump or a load — reaches RBE_DONE too and must NOT arm this,
   which is why rabisu_boss.c calls it at that one transition rather than this
   file watching for RBE_DONE.

   The beat sheet, as briefed:

     fade      4 s from the garden to purple, with the Anzu track coming in
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

/* Arm it. Called from rabisu_boss.c the frame the death sequence retires. */
void trial_end_start(void);

/* 1 while this screen owns the game: no menu, no HUD, no world update. */
int  trial_end_active(void);

/* 1 while the room behind it should still be drawn — the 4-second fade only.
   Past that the screen is flat purple and drawing the garden would be four
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
