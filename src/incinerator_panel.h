#ifndef INCINERATOR_PANEL_H
#define INCINERATOR_PANEL_H

#include "render.h"

/* The Incinerator's conveyor board. Walk up to the tray at the WEST end of the
   machine for a "Press O to interact" prompt (drawn by incinerator_room.c),
   then Circle locks the camera to a fixed shot up and to the side of the
   conveyor, looking down at it, and hands the player one box:

     - the box is EMPTY: Circle opens a picker listing every item the player
       carries, and choosing one moves it out of the inventory and into the
       machine. Ammo moves at most INC_AMMO_MAX rounds (src/incinerator.h).
     - the box is FULL: Circle takes the item back.

   Cross backs out of the picker, then out of the board.

   >>> IT IS THE STOVE PUZZLE'S BOARD, ONE BOX INSTEAD OF THREE, AND WITH ONE
   REAL DIFFERENCE. <<< The stove COPIES an item into its box and the player
   keeps it either way; this machine MOVES it, because the whole point is that
   the incinerator can then destroy it. So every exit path out of this board
   leaves the item where the player put it — there is no "nothing was spent"
   case to fall back on, and the item lives in the hopper (and in the savegame)
   rather than in a board variable that a room change would drop.

   THE BUTTON IS NOT HERE. Pressing it is an ordinary walk-up interaction on the
   machine's north face and belongs to the room, beside the door prompt; this
   module owns only the shot over the conveyor. */

void incinerator_panel_arm(void);     /* room entry: swallow a held Circle   */
void incinerator_panel_update(void);  /* per frame: proximity trigger, input */
void incinerator_panel_draw(RenderContext *ctx);   /* 2D board/picker overlay */

int  incinerator_panel_active(void);  /* 1 while it owns the camera + input  */

#endif
