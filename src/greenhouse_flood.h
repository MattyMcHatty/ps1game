#ifndef GREENHOUSE_FLOOD_H
#define GREENHOUSE_FLOOD_H

#include <stdint.h>
#include "render.h"

/* ---- Taking the Valve Handle, and what comes down when it comes off --------
   The Greenhouse's second event, and the one that turns the room from a quiet
   glasshouse into a fight. It is TWO things bolted together and the join is
   worth stating up front, because neither half is much on its own:

     THE TAKE      A floating "Press O to remove" hangs over the wheel on the
                   standing pipe in the south bay (the mount src/valve_handle.c
                   places, and the only one in the game today). Tapping Circle
                   in range clears that mount's `present` bit — the wheel is
                   gone from the world — and sets ITEM_VALVE_HANDLE in
                   player_items. There is no PickupKind and no sprite: the
                   player unbolts the 3D prop itself, which is why this lives
                   here rather than in src/item_pickup.c.

     THE FLOOD     Unbolting the wheel opens the roof. The camera cuts to a
                   fixed shot over the nave, six ceiling jets run for three
                   seconds, and then — on ONE frame — six Rafflesias, four
                   Mushroom Heads and five vine curtains arrive together. Then
                   control goes back.

   >>> IT IS THE PIPE-BUTTON PUZZLE'S SHAPE, ONE ROOM OVER. <<< Read
   src/greenhouse_puzzle.c first: the free-play prompt, the Circle edge state
   that starts "held" so a press carried through the door transition is
   swallowed, the "did I consume this tap" return value that keeps one press
   from also opening a door, the save/restore of the camera around a hard cut,
   and greenhouse_flood_active() as main.c's gate for stopping the rest of the
   room. All of that is that file's, unchanged.

   THREE THINGS ARE NEW, and each is a note in the .c:

     THE CAMERA LOOKS AT THE WHOLE ROOM, not at one changed thing. The button
     puzzle's payoff cuts to a doorway because a doorway is what changed; here
     the change is everywhere, so the shot is a corner vantage that holds the
     entire nave — see the GHF_CAM_* block.

     THE POPULATIONS ARRIVE BY THREE DIFFERENT ROUTES, because the three
     enemies persist three different ways. The flowers are placed dormant at
     startup and simply switched on (rafflesia.h); the curtains are placed
     inactive at startup and dropped out of the roof (vines.h); the mushrooms
     are SEEDED BY world_seed_room, so this file adds them live AND world.c
     re-adds them on a save rebuild. Getting that split wrong is how a room
     comes back from a load half-populated.

     NOTHING ABOUT THE SCENE IS SAVED — only that it HAPPENED
     (FLAG_GREENHOUSE_FLOOD). A session that ends inside the three seconds
     comes back with the flag set and the animation never run, so
     greenhouse_flood_init installs the finished state outright: the flag is
     the single source of truth for all three populations and for the wheel.
     That is the same catch-up greenhouse_puzzle_init does for its curtain, and
     it exists for the same reason. */

/* Room entry: seed the Circle edge state, and install the flooded room whole if
   FLAG_GREENHOUSE_FLOOD is already set. Call from greenhouse_init(). */
void greenhouse_flood_init(void);

/* Put the flowers and the curtains where the flood leaves them, with no travel.
   >>> world.c's world_seed_room() MUST CALL THIS, and it is not optional. <<<
   A title-screen load runs greenhouse_init (hence greenhouse_flood_init) and
   only THEN savegame_apply_pending -> world_load_delta, which begins by calling
   rafflesias_reset() and vines_reset() — undoing anything room entry had just
   installed. world_seed_room is the one hook that runs on the far side of those
   resets, so it is where a loaded save gets its overgrowth back. Idempotent, and
   safe for a room whose geometry is not resident: it touches no mesh.

   The mushrooms are NOT in here — world_seed_room places them itself, because
   they are the population that has to be rebuilt rather than merely switched on.
   See the note on the three routes above. */
void greenhouse_flood_seed(void);

/* New game: take the Valve Handle back out of the inventory. Every module that
   GRANTS a player_items bit clears its own on a reset — see item_pickups_reset
   for the ones the pickup module owns — and this is the only grantor of
   ITEM_VALVE_HANDLE. Without it the item survives into a fresh playthrough with
   the wheel back on its pipe, ready to be taken twice. Call from reset_game(),
   beside valve_handles_reset(). */
void greenhouse_flood_reset(void);

/* Per-frame. Returns 1 on the frame a Circle tap was CONSUMED by the valve, so
   the caller can stop the same tap doing anything else. Call from the
   Greenhouse's area update. */
int  greenhouse_flood_update(void);

/* 1 while the cut owns the camera. main.c reads it to stop the rest of the room
   running underneath the shot, and to suppress the Start menu and the weapon
   overlays — exactly as greenhouse_puzzle_active() is read. */
int  greenhouse_flood_active(void);

/* The floating Circle prompt over the wheel. Call from greenhouse_draw with the
   camera view matrix loaded, inside the room's 128 texture window. */
void greenhouse_flood_draw(RenderContext *ctx);

#endif
