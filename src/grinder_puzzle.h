#ifndef GRINDER_PUZZLE_H
#define GRINDER_PUZZLE_H

#include <stdint.h>
#include "render.h"

/* The Rear Gate's corridor gate: two grinders set into the hedge, and a lever
   in the west hedge that drives them together to block the path and apart again
   to clear it.
 *
 * It is a TOGGLE, not a solve, FOR AS LONG AS IT WORKS AT ALL. Throwing the
 * lever once closes the corridor, throwing it again opens it, and leaving the
 * room and coming back re-runs grinder_puzzle_place() and puts everything back
 * at its start.
 *
 * >>> EXCEPT UNDER FLAG_HADAD_THREE, WHERE IT IS ONE THROW AND NO MORE. <<<
 * That encounter is a single timed decision — the plates either have Hadad on
 * the frame the switch goes over or they do not (THE THREE ANSWERS TO THE LEVER
 * in the .c) — so the throw itself sets FLAG_GRINDER_BROKEN and the toggle above
 * never gets its second half. Without that the player could shut the corridor,
 * watch him walk up to it, re-open, and keep trying the timing until it landed.
 *
 * >>> AND ONCE FLAG_GRINDER_BROKEN IS SET, THAT IS THE END OF IT. <<< From then on
 * grinder_puzzle_place() seats the pair SHUT rather than open, the lever answers
 * a press with "The mechanism is broken" and nothing re-opens it — the corridor
 * is the only way from the lawn to the ramp, so the house is sealed off from the
 * garden for the rest of the game. Every ending of Hadad's third encounter sets
 * it, the throw above first among them; the flag's own block in src/player.h has
 * the list.
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

/* THE INVISIBLE BACKSTOP ACROSS THE MACHINE'S SOUTH MOUTH, up for exactly as
   long as the pair are travelling. Call it from the room's collision pass, right
   after grinders_collide.

   >>> IT IS HERE TO STOP THE PLAYER OUTRUNNING THEIR OWN LEVER. <<< The lever is
   1485 up the corridor from the grinders (see GP_LEVER_Z in the .c) and the
   travel is over five seconds, which is time enough to throw it and then sprint
   down and through the closing gap. Doing that left the player SOUTH of a
   corridor that then shut behind them, with no lever on that side — a
   fine-looking move that quietly took the room away.

   So while the machinery runs, the way through is closed at the machine's south
   face and the player is held inside its throat. They can still walk in, and
   that is the point: what is left is turn back or be crushed, which is the
   choice the corridor gate is meant to offer.

   ONE-SIDED, LIKE EVERY OTHER WALL IN THE GAME. It pushes only what comes at it
   from the north; anything already south of the plane is left alone, so a player
   down by the ramp when the lever is thrown is never dragged back into the
   plates. `py` is the player's standing Y, gated as the grinders' own boxes are
   so the plane cannot reach up the ramp. */
void grinder_puzzle_collide(int32_t *px, int32_t py, int32_t *pz);

/* 1 while the grinders are anywhere but fully apart — for anything that wants to
   know the corridor is obstructed without reading the props' coordinates. */
int  grinder_puzzle_blocking(void);

/* ---- What the Hadad Death Scene reads (src/hadad_grinder.c) ---------------
   The scene is played entirely between these two plates, so it needs to know
   where they are and when they arrive. Both are DERIVED from the live prop
   positions rather than re-stated over there: the travel, the plate offset and
   the timing are this module's facts (see THE PAIRING LIVES IN THIS MODULE
   above) and a second copy of them in the director is a second copy to keep in
   step. */

/* WHERE THE PLATES MEET. The corridor's centre line, and the Z both grinder
   bodies stand on — the point the scene frames and the point the body being
   crushed is slid onto. Stated here rather than in the director because it is
   this module's placement; grinder_puzzle.c static-asserts the Z against its own
   GP_Z so the two cannot drift. */
#define GRINDER_PUZZLE_MEET_X      0
#define GRINDER_PUZZLE_MEET_Z   (-85)

/* Half the gap still open between the two plates, in world units: 220 with the
   pair fully apart, 0 with them touching at the corridor's centre line. This is
   what the body being crushed between them is scaled to. */
int32_t grinder_puzzle_plate_gap(void);

/* Frames of travel still to run in the direction the pair are currently
   committed to. GP_TRAVEL_FRAMES on the frame the lever is thrown, 0 once they
   have arrived. The scene schedules its roar and its kill off this. */
int  grinder_puzzle_travel_left(void);

/* Re-seed the lever's own Circle edge state, the way rear_gate_gate_arm() does
   for the gate. Call it when a cutscene that has been holding the pad for
   several seconds gives control back: this module's edge detector is not polled
   while the scene runs (grinder_puzzle_update takes lock == 1 throughout), so
   without this the player's first press afterwards is swallowed. */
void grinder_puzzle_arm(void);

#endif
