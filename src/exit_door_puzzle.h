#ifndef EXIT_DOOR_PUZZLE_H
#define EXIT_DOOR_PUZZLE_H

#include "render.h"

/* The Attic Exit's exit-door puzzle: four sockets in a diamond on the locked
   north-wall door (x[-225,225], y[-467,0], z=1000). The bottom one is fixed —
   it already holds the magenta key stone and cannot be emptied or swapped. The
   other three take an item each from the player's inventory; the door opens for
   Yellow (left) + Blue (right) + Green (top) and for nothing else.

   Structured like the kitchen's stove puzzle: a proximity prompt in free play,
   then a fixed camera + 2D board that owns input until the player backs out.
   Completion is saved as FLAG_EXIT_DOOR_UNLOCKED.

   ---- AND THEN TAKING THEM BACK OUT ------------------------------------------
   Once FLAG_HADAD_ONE is set, the same door offers "Press O to remove the
   keystones" instead of the way out. That press:

     - awards ALL FOUR stones (the three that were spent plus the magenta one
       that was part of the lock — nothing else in the game gives that one);
     - plays SFX_PICKUP and sets FLAG_HADAD_TWO;
     - shuts the door for good. Both faces then read "Locked" in red and do
       nothing: this one, and the Garden Stairs' south door, which reads the
       same flag (garden_stairs.c). The xt_dr_lckd art goes back up, here and on
       every later room entry (attic_exit.c's door_tex_id).
     - runs the quake below.

   FLAG_EXIT_DOOR_UNLOCKED IS NOT CLEARED by any of that, deliberately: it is
   still the record that the puzzle was solved, and it is what stops the board
   re-opening on the now-empty sockets.

   The quake is a cutscene in the rabisu_boss sense — it owns the camera and all
   input — but a still one: two seconds of nothing, then three of the view
   shaking in place with six overlapping rumbles under it and a log line on the
   frame it starts moving, and then free play again. The camera never moves to a
   new shot, so there is no glide either way.

   >>> THE SHAKE ITSELF IS src/quake.c, NOT THIS FILE. <<< The East Hall plays
   the same one on the way out of the wrecked Library (east_hall_quake.h), so it
   is shared. What is a state of THIS module is the beat's PLACE in the game —
   XD_QUAKE — and it is one because that is what gets it the input lock and the
   HUD/menu suppression for free: main.c already routes the whole frame through
   exit_door_puzzle_update() and hides the overlays whenever
   exit_door_puzzle_active() is 1. */
void exit_door_puzzle_load_assets(void);  /* startup: the magenta stone icon */
void exit_door_puzzle_arm(void);          /* reset to free play (new game / room entry) */
/* 1 while the board — or the quake — owns the camera and input. main.c reads it
   both to route the frame here and to suppress the menu and HUD overlays. */
int  exit_door_puzzle_active(void);
void exit_door_puzzle_update(void);
void exit_door_puzzle_draw(RenderContext *ctx);   /* the board; call from the room's draw */
void exit_door_prompt_draw(RenderContext *ctx);   /* the world-space sign on the door */

/* TEMPORARY: with the door unlocked, its prompt reads "Press O to exit" and a
   press latches here. main.c consumes it and runs a single-door transition that
   drops the player back at the start of this same room, standing in for the
   room that will eventually sit behind the door. */
int  exit_door_exit_triggered(void);

#endif
