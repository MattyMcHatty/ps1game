#ifndef VALVE_PUZZLE_H
#define VALVE_PUZZLE_H

#include "render.h"
#include "title.h"   /* GameState */

/* THE VALVE PUZZLE — one puzzle, three rooms, one handle.
   =========================================================================
   The Valve Handle comes off the Greenhouse's standing pipe (that half is
   src/greenhouse_flood.c and is not this module's business). THREE more pipes
   in the garden carry the same pipe texture and the same 50-unit standpipe
   silhouette — Maze One's, Maze Two's and the Chain Room's — and the handle
   fits all three, IN ANY ORDER. This module is the interaction, the board, the
   shot and the three payoffs; src/valve_handle.c is the wheel itself.

   WHAT THE PLAYER DOES. A floating "Press O to interact" sign hangs over each
   pipe. Circle cuts to a fixed shot looking down at it and puts up a board of
   two boxes: one ITEM box that opens the same picker the stove uses, and a
   half-height USE box under it. USE with anything but the Valve Handle logs
   "That doesn't make sense..." and nothing else happens. USE with the handle
   hides the board and plays the FIT: the wheel appears out in front of the
   pipe, slides in, pauses, and takes three sharp clockwise 30-degree turns
   (src/valve_handle.c owns the animation; VALVE_FIT_FRAMES is its length). The
   camera does not move for any of it.

   WHAT EACH PIPE THEN DOES.

     MAZE ONE    opens the drain that crosses the maze's paths at z[764,831] —
                 the one that runs under the bird cage. SFX_WATER starts looping
                 and goes on looping in Maze One, Fountain Square and the Rear
                 Gate for the rest of the game. Log: "The drain below is flowing
                 with water".

     MAZE TWO    unlocks BOTH of the Chain Room's gates, from all four sides.
                 Log: "You hear something unlock".

     CHAIN ROOM  winds in the chain over Maze One's bird cage. The Hatch Key
                 floating inside the cage goes, and the cage advances a state.
                 Log: "The chain has been wound in".

   >>> THE CAGE REACHES ITS LAST STATE BY EITHER ORDER, AND THAT IS DELIBERATE.
   <<< birdcage.h has three states: LOCKED (key in the cage), DROPPED (cage
   open, key in the drain) and WASHED (the drain has run and the key is gone).
   The Chain Room's pipe is what OPENS the cage and Maze One's is what RUNS the
   drain, so whichever is turned second is the one that reaches WASHED:

     Maze One then Chain Room   the drain is already running, so winding the
                                chain drops the key straight into moving water:
                                LOCKED -> WASHED in one step.
     Chain Room then Maze One   the key lies in a dry drain (DROPPED), and
                                turning Maze One's valve washes it away.

   Without the second route the puzzle would be order-dependent and DEAD-ENDABLE
   — a player who wound the chain first would sit at DROPPED forever and the
   Rear Gate's key would never appear.

   >>> AND THAT LAST STATE IS WHAT PUTS A HATCH KEY IN THE REAR GATE. <<< It
   appears on the path just north of the two grinders, on the line the drain
   runs down (x=230), which is where water leaving Maze One's drain would put
   it. See valve_puzzle_apply_flags().

   THE HANDLE IS CONSUMED once all three pipes are spent — "I no longer need
   this Valve Handle", posted as a second log line after the third pipe's own.
   Each pipe goes inert the moment it is turned, so no pipe can be worked twice
   and the handle can never be spent early.

   ---- What rides in the save ------------------------------------------------
   Three GameFlags, one per pipe (FLAG_VALVE_* in src/player.h), plus
   FLAG_DRAIN_KEY_PLACED for the Rear Gate's key. Nothing else: the board, the
   camera and a half-played fit are not world changes, which is the same call
   greenhouse_flood.c and greenhouse_puzzle.c make about their own scenes. The
   wheel's own present/absent bits ride in WorldDelta.valve_present as they
   always did.

   ---- Where this hooks into main.c ------------------------------------------
     valve_puzzle_arm()          from each of the three rooms' _init(), beside
                                 the gate arms, so a Circle carried in through a
                                 gate cannot fire the prompt on arrival.
     valve_puzzle_update()       from the three rooms' area blocks. It returns 1
                                 when it CONSUMED this frame's Circle tap, which
                                 is the veto the gate triggers take — the same
                                 shape birdcage_update() has in Maze One.
     valve_puzzle_active()       must gate the room's gates and go in main.c's
                                 `puzzle` list, so the board owns the screen.
     valve_puzzle_draw()         the 2D board, from the room's own _draw().
     valve_puzzle_text()         the 3D floating sign, likewise, inside the
                                 room's 128 texture window with the other signs.
     valve_puzzle_apply_flags()  from main.c's re-derive block AFTER
                                 savegame_apply_pending() — it writes to
                                 item_pickups and reads flags the load has only
                                 just installed, exactly as
                                 keystone_plinths_apply_flags() does. It also
                                 starts and stops the water loop, so it must run
                                 on EVERY transition and not only the garden's.
     valve_puzzle_reset()        from reset_game(). */

/* ---- The Rear Gate's washed-out Hatch Key ----------------------------------
   PUBLIC because it is placed from TWO PLACES and both must agree: the live
   path in valve_puzzle_apply_flags(), which spawns it the first time the room is
   entered after the drain has run, and world_seed_room(STATE_REAR_GATE), which
   re-places it on a save rebuild. Two copies of three numbers is exactly the
   thing that goes out of step, so there is one copy and both read it.

   The spot: x on the drain channel's centre line (its run through this room is
   x[204,257]), z just NORTH of the two grinders at z=-85, y the -50 every
   floor-level pickup in a y=0 room passes. See the block in
   valve_puzzle_apply_flags() for the derivation. */
#define VP_DRAIN_KEY_X    230
#define VP_DRAIN_KEY_Y   (-50)
#define VP_DRAIN_KEY_Z    300

/* 1 while the puzzle owns the camera and input: the intro glide, the board, the
   picker, the fit animation and the payoff hold. */
int  valve_puzzle_active(void);

/* Room entry, for the three pipe rooms: seed the Circle edge state. */
void valve_puzzle_arm(void);

/* One frame. Returns 1 if it consumed this frame's Circle tap OR if it owns the
   screen — in both cases the room's gates must not act on the press. */
int  valve_puzzle_update(void);

void valve_puzzle_draw(RenderContext *ctx);   /* 2D board / picker overlay */
void valve_puzzle_text(RenderContext *ctx);   /* the 3D "Press O to interact" */

/* Post-load / post-transition reconcile. Handles BOTH of the puzzle's
   cross-room effects, because both are read off flags that a save restores
   after the room has already been built:
     - the water loop, started or stopped for the area being entered;
     - Maze One's caged Hatch Key, removed once the cage has been opened;
     - the Rear Gate's Hatch Key, placed the first time that room is entered
       after the drain has washed the cage key away. */
void valve_puzzle_apply_flags(GameState area);

void valve_puzzle_reset(void);

/* 1 once Maze Two's pipe has been turned. The Chain Room's two gates read this
   from all four sides — chain_room.c, maze_two.c and keystone_maze.c — and show
   "Locked by some mechanism" until it is true. */
int  valve_puzzle_gates_unlocked(void);

#endif
