#ifndef PIANO_PUZZLE_H
#define PIANO_PUZZLE_H

#include "render.h"

/* The piano room's missing-key puzzle. Walk up to the piano for the "Press O to
   examine" sign (drawn by piano_props.c), then Circle locks the camera to a
   fixed shot angled down onto the keyboard and hands the player a small board,
   built to the same pattern as the kitchen stove's:

     - ONE item box on the right of the screen. Circle on it opens a picker
       listing every item the player carries; choosing one places it in the box.
     - a half-height PLACE box below it. Placing anything but the Piano Key just
       logs "That makes no sense..."; placing the Piano Key consumes it, swaps
       the piano's keyboard texture for the repaired one, then turns and pulls
       the camera back onto the bookcase and sinks it through the floor before
       ejecting the player back to where they stood.

   Completion is FLAG_PIANO_SOLVED (see player.h): unlike the stove, this puzzle
   awards no item to hang the flag on — it takes one away. */

void piano_puzzle_arm(void);      /* piano-room entry: swallow a held Circle */
void piano_puzzle_update(void);   /* per-frame: proximity trigger, then board input */
void piano_puzzle_draw(RenderContext *ctx);   /* 2D board/picker overlay */

int  piano_puzzle_active(void);   /* 1 while the puzzle owns the camera + input */
int  piano_puzzle_solved(void);   /* 1 once the Piano Key has been fitted */

#endif
