#ifndef GRINDER_PUZZLE_H
#define GRINDER_PUZZLE_H

#include <stdint.h>
#include "render.h"

/* The Rear Gate's corridor gate: two grinders set into the hedge, and a lever
   in the west hedge that drives them together to block the path and apart again
   to clear it.
 *
 * It is a TOGGLE, not a solve. There is no state to get wrong and no flag to
 * save — throwing the lever once closes the corridor, throwing it again opens
 * it, and leaving the room and coming back re-runs grinder_puzzle_place() and
 * puts everything back at its start. Nothing else in the room depends on it, so
 * that reset costs nothing today; a player who is meant to find the corridor
 * still shut on their return will need a GameFlag adding here.
 *
 * THE PAIRING LIVES IN THIS MODULE, not in rear_gate.c. The two grinders' start
 * and end positions, the lever's place on the wall and the travel time are one
 * fact in three parts — the grinders must meet exactly where the corridor's
 * centre is, and the travel must be exactly as long as the sound that covers
 * it — so grinder_puzzle_place() owns all of it, the way lightswitch_place()
 * owns the Attic Exit's lever/light pairing. */

/* Room entry: place the two grinders and the lever, and arm the puzzle open.
   Calls grinders_clear() and levers_clear() first, so it is the whole of the
   Rear Gate's prop placement. */
void grinder_puzzle_place(void);

/* Per-frame. `lock` is main.c's menu-open flag: it suppresses the PRESS only.
   Grinders already travelling keep travelling under an open menu, because
   stopping a moving wall dead because someone opened the map would read as a
   bug and the room ticks on around it anyway. */
void grinder_puzzle_update(int lock);

/* The floating "Press O to activate" sign in front of the lever. The lever and
   the grinders themselves are drawn by levers_draw()/grinders_draw(). */
void grinder_puzzle_draw(RenderContext *ctx);

/* 1 while the grinders are anywhere but fully apart — for anything that wants to
   know the corridor is obstructed without reading the props' coordinates. */
int  grinder_puzzle_blocking(void);

#endif
