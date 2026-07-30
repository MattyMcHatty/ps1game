#ifndef STOVE_PUZZLE_H
#define STOVE_PUZZLE_H

#include "render.h"

/* The kitchen stove cooking puzzle. Walk up to the stove for a "Press O to
   interact" prompt (drawn by kitchen_dining.c), then Circle locks the camera to
   a fixed top-down shot over the hob and hands the player a small board:

     - two ingredient boxes on the right of the screen. Circle on a box opens a
       picker listing every item the player carries; choosing one places it in
       the box WITHOUT consuming it from the inventory.
     - a half-height COOK box below them. Cooking the wrong pair just logs
       "That doesn't make sense..."; cooking the Copper Pot with the Wax Cube
       lights the burner for three seconds and awards the Green Key Stone.

   Winning ejects the player back to where they stood and retires the puzzle for
   good (ownership of the stone IS the completion flag, so it survives saves). */

void stove_puzzle_arm(void);      /* kitchen entry: swallow a held Circle */
void stove_puzzle_update(void);   /* per-frame: proximity trigger, then board input */
void stove_puzzle_draw(RenderContext *ctx);   /* 2D board/picker overlay */

int  stove_puzzle_active(void);   /* 1 while the puzzle owns the camera + input */
int  stove_puzzle_solved(void);   /* 1 once the Green Key Stone has been cooked */

#endif
