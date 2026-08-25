#ifndef KEYSTONE_PLINTHS_H
#define KEYSTONE_PLINTHS_H

#include <stdint.h>
#include "render.h"

/* ---- The Keystone Maze's plinth puzzle --------------------------------------
   FOUR plinths stand at the ends of four dead-end hedge alcoves, each capped
   with a sloped plinth_diamond face that a coloured light glows out of, and a
   FIFTH — the keystone itself — stands in the middle of the central court,
   faced in plinth_diamond on all five sides and dark.

   Placing the matching key stone in an alcove plinth puts that plinth's light
   out and lights the corresponding FACE of the keystone in the same colour.
   With all four faces burning the top face lights white and a Hatch Key
   appears on the keystone.

     PLINTH   centre XZ   colour    stone            keystone face
     -------  ----------  --------  ---------------  --------------
     NW       (   0,5400) green     Green Key Stone  north  (z=2500)
     W        (   0,3600) magenta   Magenta   "      west   (x=2300)
     NE       (5400,5400) yellow    Yellow    "      east   (x=2500)
     SE       (4800,1200) blue      Blue      "      south  (z=2300)

   >>> "WEST" IS THE ALCOVE AT z=3600, NOT A CORNER OF THE COURT. <<< The four
   200x200 blocks standing at the court's own corners (x/z 1500-1700 and
   3100-3300) are plain `plinth` — no diamond cap, no light, nothing to
   interact with. The four this module owns are the ones keystone_maze.h calls
   SET DRESSING: each is buried in a hedge block with only its front face and
   its cap showing, at the end of a corridor that stops dead in front of it.

   WHICH WAY EACH IS FACED comes off THE SLOPE OF ITS CAP, not off the collision
   walls: the cap's low edge (y=-124) is always the one over the corridor. NW is
   read from the east (x=99) and W, NE and SE are all read from the south
   (z=3500, z=5300 and z=1100). The north-east one is the reason the rule is
   stated that way — it is cornered by hedge and carries a wall on BOTH sides, so
   the wall list alone would let you face it either way. That direction fixes the
   sign's plane and mirror and the side the camera swings out to.

   ---- Shape of the interaction -----------------------------------------------
   Structured like the kitchen's stove board (stove_puzzle.c) with the middle
   step taken out: a proximity prompt in free play, then Circle glides the
   camera up and out to one side looking down on the cap, and the item picker
   opens STRAIGHT AWAY — there is only one thing to fill, so there is no board
   to choose it on. The right stone is CONSUMED and hands play straight back;
   a wrong one is refused with a line and leaves the picker up.

   The light cross-fade then runs in FREE PLAY (the plinth fading down, then the
   keystone's face coming up), exactly as the Attic Exit's levers do: the player
   is walking around while it happens. Only the final payoff takes the camera,
   and that is what keystone_plinths_active() reports along with the picker.

   State is five bits of game_flags — see FLAG_KEYSTONE_NW in player.h. */

/* Install the puzzle from the CURRENT game_flags: lights, faces and the
   keystone's top. Call from keystone_maze_init AFTER the collision/floor setup.
   Safe to repeat. */
void keystone_plinths_place(void);

/* Everything DERIVED from a saved flag, re-run once the real flags are in — the
   same contract attic_exit_apply_flags has, since the area init above ran while
   game_flags still held the pre-load values.

   >>> IT MUST RUN AFTER world_enter(), NOT INSIDE THE AREA INIT. <<< On top of
   re-running the install it also CATCHES UP THE REWARD: four placements with
   FLAG_KEYSTONE_REWARD still clear means the player quit or died between the
   fourth stone and the payoff that spawns the key, so this hands it over
   on the spot. Spawning is a write to item_pickups, and world_enter overwrites
   that array wholesale with the saved room state — do it in the area init and
   the box is dropped on the floor of a room that is about to be restored over
   the top of it. */
void keystone_plinths_apply_flags(void);

/* 1 while the picker or the payoff owns the camera — free movement, weapons,
   the Start menu and the room's own gate trigger must all be suppressed. */
int  keystone_plinths_active(void);

/* Per-frame: proximity/Circle handling, the picker, the cross-fades and the
   payoff. Call once a frame from the Keystone Maze's area update. */
void keystone_plinths_update(void);

/* The lights, the prompts AND the 2D picker overlay, in that order. Call from
   keystone_maze_draw with the camera view matrix loaded (after the room mesh) —
   the 2D half sorts into the menu-reserved OT range, so sharing one call with
   the 3D half costs nothing. */
void keystone_plinths_draw(RenderContext *ctx);

#endif
