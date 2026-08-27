#ifndef GREENHOUSE_PUZZLE_H
#define GREENHOUSE_PUZZLE_H

#include <stdint.h>
#include "render.h"

/* ---- The Greenhouse's pipe-button puzzle ------------------------------------
   Ten buttons set into the nave's two long walls — five at x=-3100 (the west
   wall, read from +X) and five at x=100 (the east wall, read from -X) — each
   one a single quad in "Greenhouse.smx" textured with pipe_button_off. Standing
   in front of one and tapping Circle toggles it; the mesh poly redraws with
   pipe_button_on. Turn on EXACTLY buttons 3, 4, 5 and 6 and the vine curtain
   that fills the gap into the west annexe winds up into the ceiling, opening the
   room the Helluminator is sitting in.

   NUMBERING IS NORTH TO SOUTH, WEST WALL FIRST: 1..5 run down x=-3100 from
   z=813 to z=-2370, then 6..10 run down x=100 from z=813 to z=-2370. +Z is
   north. That is the order the solution is stated in and the order BUTTON[]
   below is written in; nothing derives it from the geometry, so do not sort the
   table.

   IT IS THE ATTIC EXIT'S LIGHTSWITCH PUZZLE IN SHAPE (src/lightswitch_puzzle.c)
   — a set of free-play toggles checked against a fixed mask, re-armed on every
   entry while unsolved and installed solved afterwards — with three differences
   worth stating:

     THE PAYOFF CUT LOOKS BACK AT THE DOORWAY, not at the board. The lightswitch
     cuts down its room's spine at the gate it opened; this one cuts to the
     aisle halfway up the nave and looks back and DOWN at the annexe doorway in
     the west wall, because that is the thing that changed and the buttons that
     changed it are strung out along both walls with no single shot that holds
     them. See the GHB_CAM_* block in the .c for the spot, and for how the yaw
     and the pitch were solved onto the curtain rather than written down.
     greenhouse_puzzle_active() is lightswitch_puzzle_active(): main.c reads it
     to stop the rest of the room running underneath the shot.

     THE STATE IS A MASK, NOT AN ARRAY. Ten buttons is ten bits, and the solve
     test is one comparison against GH_SOLUTION_MASK — which is what makes "any
     extra button on, or any of the four off, is unsolved" fall out for free
     rather than needing a loop.

     THE MESH DRAWS THE BUTTONS, NOT THIS MODULE. There is no prop and no model:
     greenhouse.c's mesh loop asks greenhouse_button_prim_lit() for each poly
     that carries the button texture and swaps the CLUT and a u bias. See the
     GH_TEX_BUTTON_* block in src/greenhouse.c for why that costs nothing. */

#define GH_BUTTON_COUNT 10

/* Room entry: arm the Circle edge state and install the lit set if the puzzle is
   already solved. Call from greenhouse_init(). */
void greenhouse_puzzle_init(void);

/* Per-frame. Returns 1 on the frame a Circle tap was CONSUMED by a button, so
   the caller can stop the same tap also opening the east door — buttons 6 and 7
   sit within the door's own 500-unit trigger radius. Call from the Greenhouse's
   area update, BEFORE greenhouse_door_triggered() (which must still be called
   every frame either way, or its edge state goes stale). */
int  greenhouse_puzzle_update(void);

/* 1 while the vine curtain is winding up. Nothing is camera-locked; this exists
   so the door and the weapons stay inert for the two seconds it takes. */
int  greenhouse_puzzle_active(void);

/* The floating Circle prompts. Call from greenhouse_draw with the camera view
   matrix loaded, inside the room's 128 texture window. */
void greenhouse_puzzle_draw(RenderContext *ctx);

/* 1 if mesh primitive `prim` is a button the player has switched ON. Called by
   greenhouse.c's draw loop for every primitive carrying the button texture. */
int  greenhouse_button_prim_lit(int prim);

#endif
