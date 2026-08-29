#ifndef HATCH_PUZZLE_H
#define HATCH_PUZZLE_H

#include "render.h"

/* THE HATCH PUZZLE — two keyholes, two Hatch Keys, and the drop.
   =========================================================================
   The pair of leaves over the pit in The Hatch's yard (src/hatch_doors.c) is
   locked by TWO keyholes. The two Hatch Keys the garden hands out are what open
   them, one keyhole each, and BOTH KEYS BREAK IN THE LOCK — each is consumed on
   use and neither can be got back. This module is the sign, the shot, the board,
   the two turns and the descent that follows; the leaves themselves, their clip
   and their collision box stay in src/hatch_doors.c.

   >>> IT IS NOT THE LID ON THE WELL. <<< The Hatch has two things called a
   hatch, and the one in the north chamber is part of the room mesh and does not
   move. See the top of hatch_doors.h.

   ---- WHAT THE PLAYER DOES ---------------------------------------------------
   A floating "Press O to interact" sign hangs over the pit's south lip. Circle
   pans the camera up and round to a fixed shot standing south-EAST of the hole
   and looking down across it at a three-quarter angle, and puts
   up THE VALVE PUZZLE'S BOARD, unchanged in shape because it is the same
   question: one ITEM box that opens the stove's picker, and a half-height USE
   box under it. The log states what is in front of the player as the board
   arrives, and it counts the keyholes that are left:

     neither turned    "A hatch. There are 2 keyholes"
     one turned        "A hatch. There are 2 keyholes. One is unlocked."

   USE with anything but a Hatch Key logs "That doesn't make sense..." and the
   board stays up. USE with a Hatch Key sounds SFX_UNLOCK and logs

     first     "You unlocked one side. The key broke in the keyhole!"
     second    "You unlocked the other side. This key broke too!"

   and in BOTH cases spends the key. After the FIRST the board comes straight
   back, so a player carrying both keys can work the second keyhole without
   leaving the shot. After the SECOND the board goes, the LOCK IS ALLOWED TO
   FINISH TURNING — the shot is held for the length of the unlock clip before
   anything moves — and then hatch_doors_begin_open() throws the leaves, the
   swing plays out under the same fixed shot, and the camera hands back.

   >>> THE PUZZLE STAYS OPEN AFTER ONE KEY, AND THAT IS WHY THE FLAG EXISTS. <<<
   A player can turn one keyhole, back out with Cross, wander off and come back
   with the second key — possibly a whole play session later. FLAG_HATCH_LOCK_ONE
   is what remembers it (see the long note in src/player.h on why there is no
   second bit for "both": FLAG_HATCH_DOORS_OPEN is that bit).

   ---- AND THEN THE DROP ------------------------------------------------------
   Once the leaves are open the same sign reads "Press O to drop" and the same
   Circle runs a CUTSCENE instead of a board. It is a SCRIPTED HEAD over a body
   that walks two seconds and then stands still — the beats are a table in
   hatch_puzzle.c (HP_SCRIPT), read top to bottom:

     2.0s  forward, closing on the lip      2.0s  down into the hole again
     3.0s  panning down into the hole       1.5s  slowly back to forward
     0.5s  SHARPLY back to forward          2.0s  held, and then the jump

   Near the bottom of the 1200-unit shaft the scene ends and
   hatch_puzzle_drop_done() comes true for one frame.

   >>> WHAT IS AT THE BOTTOM IS NOT BUILT YET. <<< The drop is meant to land in a
   boss encounter. Until that room exists main.c takes drop_done as an ordinary
   gate transition back into THE HATCH ITSELF, which re-enters the room at its
   west gate — the doors stay open (the flag is set), so the player can walk back
   and drop again. Replacing the placeholder is one line in main.c's
   STATE_THE_HATCH block: point pending_area at the new room.

   ---- What rides in the save ------------------------------------------------
   TWO GameFlags, both already accounted for: FLAG_HATCH_LOCK_ONE and
   FLAG_HATCH_DOORS_OPEN. Nothing else — the board, the camera and a half-played
   swing are not world changes, which is the call valve_puzzle.c and
   greenhouse_flood.c make about their own scenes. The keys themselves ride in
   player_hatch_keys, which the save blob already carries.

   ---- Where this hooks into main.c ------------------------------------------
     hatch_puzzle_arm()        from the_hatch_init(), beside hatch_doors_init(),
                               so a Circle carried in through the gate cannot
                               fire the prompt on the arrival frame.
     hatch_puzzle_update()     from main.c's STATE_THE_HATCH block. Returns 1
                               when it consumed this frame's Circle tap, which is
                               the veto the gate trigger takes.
     hatch_puzzle_active()     must gate the room's gate and go in main.c's
                               `puzzle` list, so the board owns the screen.
     hatch_puzzle_drop_done()  polled in the same block: 1 for ONE frame at the
                               bottom of the shaft.
     hatch_puzzle_draw()       the 2D board, from the_hatch_draw().
     hatch_puzzle_text()       the 3D floating sign, likewise.
     hatch_puzzle_reset()      from reset_game(). */

/* 1 while the puzzle owns the camera and input: the intro pan, the board, the
   picker, the door swing and the whole of the descent. */
int  hatch_puzzle_active(void);

/* Room entry: seed the Circle edge state and drop any half-played scene. */
void hatch_puzzle_arm(void);

/* One frame. Returns 1 if it consumed this frame's Circle tap OR if it owns the
   screen — in both cases the room's gate must not act on the press. */
int  hatch_puzzle_update(int lock);

/* 1 on the single frame the descent reaches the bottom of the shaft. main.c
   turns that into the transition; see the placeholder note above. */
int  hatch_puzzle_drop_done(void);

void hatch_puzzle_draw(RenderContext *ctx);   /* 2D board / picker overlay    */
void hatch_puzzle_text(RenderContext *ctx);   /* the 3D floating Circle sign  */

void hatch_puzzle_reset(void);

#endif
