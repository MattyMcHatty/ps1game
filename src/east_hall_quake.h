#ifndef EAST_HALL_QUAKE_H
#define EAST_HALL_QUAKE_H

/* -----------------------------------------------------------------------
 * THE EAST HALL'S COLLAPSE
 *
 * The beat that closes the wrecked Library behind the player. It is to the East
 * Hall what hadad_library.c is to the Library Destroyed: the ENCOUNTER, not the
 * room. src/quake.c is the shake; this file is the moment it happens and what it
 * costs.
 *
 * ======================================================================
 * THE BEAT
 * ======================================================================
 *   1. FLAG_HADAD_TWO is set — the four key stones are out of the Attic Exit's
 *      door and the Library has become the Library Destroyed. The player has got
 *      away from Hadad through the wrecked Library's west double door and is
 *      arriving in the East Hall.
 *
 *   2. THE HOUSE SHAKES. The same two-second hold and three-second shake, with
 *      the same six overlapping rumbles and the same log line, that the Attic
 *      Exit ran when the stones came out. No input for the five seconds. The
 *      camera does not move to a new shot — it is jittered about the arrival
 *      spot just inside the east door, so the room the player has just walked
 *      into is what they watch move.
 *
 *   3. Control comes back and the door behind them is BURIED. It keeps its
 *      "Press O to enter" sign — the player is meant to try it — but Circle now
 *      only posts "There is rubble behind the door!" and there is no transition.
 *      The way back to that half of the house is the East Stairwell.
 *
 * ONCE, AND FOR GOOD. Step 2 is armed on the FIRST arrival from the Library
 * Destroyed only; the flag it sets (FLAG_EAST_HALL_RUBBLE, player.h) is both the
 * "already happened" record and the thing the door reads, and it is saved. A
 * later loop back round through the East Stairwell into the wrecked Library and
 * out of its west door again is silent, and the door stays dead.
 *
 * >>> THE FLAG IS SET AT THE ARM, NOT AT THE END. <<< Five seconds is long
 * enough to be interrupted — a reset, a debug jump, a death — and the door has
 * to be buried in every one of those futures. Nothing in the beat can fail
 * halfway and leave the world half-changed.
 *
 * The main.c contract is the one quake.h states: a branch that routes the frame
 * to east_hall_quake_update() and skips update_camera/apply_collision/
 * apply_height, plus the HUD and menu suppression that goes with it.
 * ----------------------------------------------------------------------- */

/* New game, or any room entry that is not the one this beat wants: park it and
   silence the rumble voices. */
void east_hall_quake_reset(void);

/* Called from main.c's East Hall entry, having just established that the player
   came from STATE_LIBRARY_DESTROYED. Starts the beat if FLAG_HADAD_TWO is set
   and FLAG_EAST_HALL_RUBBLE is not, and sets the latter; otherwise a no-op.
   Runs AFTER the spawn helper, because the shake is about the arrival spot. */
void east_hall_quake_arm_on_entry(void);

/* 1 while the shake owns the camera and all input. */
int  east_hall_quake_active(void);

void east_hall_quake_update(void);

#endif
