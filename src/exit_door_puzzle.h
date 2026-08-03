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
   Completion is saved as FLAG_EXIT_DOOR_UNLOCKED. */
void exit_door_puzzle_load_assets(void);  /* startup: the magenta stone icon */
void exit_door_puzzle_arm(void);          /* reset to free play (new game / room entry) */
int  exit_door_puzzle_active(void);       /* 1 while the board owns the camera */
void exit_door_puzzle_update(void);
void exit_door_puzzle_draw(RenderContext *ctx);   /* the board; call from the room's draw */
void exit_door_prompt_draw(RenderContext *ctx);   /* the world-space sign on the door */

/* TEMPORARY: with the door unlocked, its prompt reads "Press O to exit" and a
   press latches here. main.c consumes it and runs a single-door transition that
   drops the player back at the start of this same room, standing in for the
   room that will eventually sit behind the door. */
int  exit_door_exit_triggered(void);

#endif
