#ifndef DELIVERY_INTRO_H
#define DELIVERY_INTRO_H

#include "render.h"

/* ============================================================================
   THE ARRIVAL — the Delivery Area's opening sequence
   ============================================================================
   Runs on ONE path only: the frame src/intro.c's opening sequence hands over to
   the Delivery Area on a NEW GAME. A Load Game, a door transition and a debug
   level-select jump all reach the same room without ever arming this.

   The beat sheet, as briefed:

     fade in     the screen comes up out of the intro's black on a camera
                 standing on the grass OUTSIDE the yard, on the far side of the
                 rusty fence from the usual starting spot, facing it
     walk        the camera moves forward to the fence, with footsteps
     jump        it crouches, leaps the fence in one arc, and comes down
     land        the hurt sound, a knee-bend, and control is handed over on the
                 exact spot reset_game() would have started the player

   IT IS A CAMERA MOVE, NOT AN ANIMATION. The camera IS the player in this
   engine (camera.h), so "vaulting the fence" is cam_x walking west and cam_y
   describing an arc — and the hand-over is simply camera_release_player() with
   the camera already standing where the game starts. That is the same trick
   the Rabisu encounter's fight spot uses; see tools/ADDING_A_BOSS_ENCOUNTER.txt
   STEP 3, which is the reference for everything in this file.               */

/* Arm the sequence and place the camera at its start. Called from main.c on the
   STATE_INTRO -> STATE_DELIVERY_AREA hand-over, AFTER reset_game(), which would
   otherwise stamp the normal spawn back over the camera. */
void delivery_intro_start(void);

/* Advance one frame. Drives cam_* directly; the caller must NOT run
   update_camera / apply_height / apply_collision while this is active, or
   gravity drags the jump back onto the floor. */
void delivery_intro_update(void);

/* Screen-space overlay: the fade up from the intro's black. Drawn LAST, from
   the end of delivery_area_draw. */
void delivery_intro_draw(RenderContext *ctx);

/* 1 while the sequence owns the camera. main.c reads this as a cutscene flag:
   no Start menu, no weapon overlay, no HUD, and no free-roam update. */
int  delivery_intro_active(void);

/* Forget a half-played sequence. In reset_game(), beside the other cutscene
   resets — a New Game started out of a paused arrival must not carry the
   camera script into the fresh one. */
void delivery_intro_reset(void);

#endif
