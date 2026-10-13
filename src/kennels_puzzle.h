#ifndef KENNELS_PUZZLE_H
#define KENNELS_PUZZLE_H

#include <stdint.h>
#include "render.h"

/* ---- The Kennels' lever puzzle ----------------------------------------------
   FOUR CELLS at the back of the Kennels, each behind a Bars gate, and FOUR
   LEVERS on the east wall. Named as the brief names them:

     cells   A B C D   west to east: the north-west corner bay, the west
                       kennel, the east kennel, the north-east corner bay
     levers  1 2 3 4   north to south

   A cell opens when EXACTLY its levers are down — every other lever up:

     A  3        B  1 + 4        C  2 + 4        D  3 + 4

   So at most one cell is ever open, and putting every lever down opens none.
   The gate of the cell that matches winches up whole into the lintel over it
   (bars_raise, two machinery clips); change a lever and it drops again
   (bars_drop, the slam). Nothing is judged until every lever has finished
   moving, so the player watches the last one land before the stone moves.

   Free play, no camera: Circle swings a lever over half a second with the
   Attic Exit's clunk - UP is 45 degrees above level, DOWN 45 below.

   SAVED: the four levers, as one byte (SaveData.kn_levers). The gates are not
   - they are derived from the levers on every entry. This module owns the
   placements of both props, so the lever <-> cell wiring has one source of
   truth. */

/* Place the four levers and the four gates from the saved levers. Call from
   kennels_init(), and again after a Load Game has installed the byte. */
void kennels_puzzle_place(void);

/* Per frame: the Circle test, the throws, the gates. `lock` is main's usual
   suppression; the edge state is kept current under it. */
void kennels_puzzle_update(int lock);

/* The levers, the gates and the four "Press O" signs. Call from kennels_draw
   with the camera view loaded, AFTER the Chapter 3 enemies (the bars need the
   128 texture window those restore - src/bars.h). */
void kennels_puzzle_draw(RenderContext *ctx);

/* The save byte: bit i = lever i+1 is DOWN. */
int  kennels_puzzle_levers(void);
void kennels_puzzle_set_levers(int bits);
void kennels_puzzle_reset(void);   /* new game: every lever up */

#endif
