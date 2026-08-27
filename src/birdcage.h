#ifndef BIRDCAGE_H
#define BIRDCAGE_H

#include <stdint.h>
#include "render.h"

/* ---- Maze One's Bird Cage --------------------------------------------------
   The wire box hanging over the south-east pocket of the maze, with a Hatch Key
   locked inside it. The key is a real ItemPickup (world.c seeds it) placed
   inside the cage with a one-unit collect radius, so it draws and bobs where the
   player can see it and can never be walked into — see MO_CAGE_KEY_* in world.c.

   Under the cage floats an examine prompt, and this module owns it: the sign
   always reads "Press O to examine", and the Circle press posts one of three log
   lines depending how far the puzzle has got. The two later states are not
   reachable yet — nothing sets FLAG_BIRDCAGE_OPEN or FLAG_BIRDCAGE_WASHED — and
   birdcage_open()/birdcage_wash() are the entry points whatever ends up opening
   the cage and running the drain will call. */

/* ---- Where the cage is ------------------------------------------------------
   Read straight out of "assets/garden/Maze One.packed.smx": of the nineteen
   polys carrying the `chain` material, ten box a volume at

       x[3675, 3785]   z[713, 847]   y[-643, -477]

   (plus a small tapered collar above it, where the long strand that makes up
   the other nine hooks on). So the cage is 110 x 134 across and 166 tall,
   hanging with its FLOOR at y = -477. These are PUBLIC because world.c places
   the key inside the cage and must use the same numbers; re-export the room and
   re-derive them rather than nudging either copy.

   >>> THE CAGE IS OVER THE PLAYER'S HEAD AND THAT IS THE PUZZLE. <<< This room's
   floor is y=0 and the standing eye is MO_EYE_Y (-189), so the underside is 288
   above eye level — high enough to read as out of arm's reach from anywhere in
   the pocket, which is what makes "I can't reach it" honest rather than an
   arbitrary refusal. Move the cage in Blender and this note has to be
   re-checked, not just the constants. */
#define BIRDCAGE_X          3730    /* (3675 + 3785) / 2 */
#define BIRDCAGE_Z           780    /* ( 713 +  847) / 2 */
#define BIRDCAGE_FLOOR_Y   (-477)   /* the cage's underside; -Y is up          */
#define BIRDCAGE_MID_Y     (-560)   /* halfway up the inside: where the key is */

typedef enum {
    BIRDCAGE_LOCKED = 0,   /* the key is in the cage, out of reach   */
    BIRDCAGE_DROPPED,      /* the cage is open, the key is in the drain */
    BIRDCAGE_WASHED        /* the drain has run and the key is gone  */
} BirdcageState;

BirdcageState birdcage_state(void);

/* Room entry: seeds the Circle edge state so a press carried in through the
   gate transition does not fire the prompt on the first frame. Called from
   maze_one_init() after the spawn, exactly as the gates are armed. */
void birdcage_init(void);

/* One frame of the prompt's interaction. Returns 1 if it CONSUMED this frame's
   Circle tap, which is the veto main.c hands to the gate triggers so a single
   press can never both examine the cage and walk the player through a gate.
   (No gate is anywhere near the cage today; the veto is the room's contract,
   not a fix for a live overlap.) */
int  birdcage_update(void);

/* The floating prompt. Called from maze_one_draw with the view matrix loaded. */
void birdcage_text(RenderContext *ctx);

/* The two state advances, for whatever ends up driving them. Each is
   idempotent, and _wash implies _open so the state can never skip. */
void birdcage_open(void);
void birdcage_wash(void);

#endif
