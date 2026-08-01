#ifndef ANZU_PUZZLE_H
#define ANZU_PUZZLE_H

#include <stdint.h>
#include "render.h"

/* ---- The Anzu Tablet ------------------------------------------------------
   The piano room's second puzzle, in the recessed frame set into the back
   (west) wall. Six carved tiles start scattered in boxes down either side of
   the screen, each one turned a random quarter-turn; the player moves a
   reticule between the boxes and the frame's six central panels, picks a tile
   up, turns it with Square and drops it into a panel. Nothing happens until all
   six sit in the frame in the order 1,2,3 / 4,5,6 and all six are upright —
   which reassembles the relief the Tablets prop is carved with.

   Leaving early throws every tile back into the boxes and re-randomises them.
   Solving it is permanent (FLAG_ANZU_SOLVED): the tiles stay in the frame, the
   Tablets prop vanishes, the Yellow Key Stone appears by the piano and the
   room's music changes for good.

   Structured like trick_drawers.c — a proximity prompt and a Circle trigger
   when idle, a fixed camera and a reticule when active. */

void anzu_puzzle_place(void);   /* piano_room_init: reset/restore + arm Circle */
void anzu_puzzle_update(void);  /* prompt/trigger when idle; input when active */
void anzu_puzzle_draw(RenderContext *ctx);  /* frame tiles (3D) + board (2D) */
int  anzu_puzzle_active(void);  /* 1 while the puzzle owns the camera + input */

#endif
