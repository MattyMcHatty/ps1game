#ifndef ARM_PUZZLE_H
#define ARM_PUZZLE_H

#include "render.h"

/* The Head's arm-switch puzzle. The EIGHT stone arms in the east relief
   (src/arm_switch.h) each hold one thing in an open hand.

   Walk up to an arm for a "Press O" sign over its hand, and Circle lifts the
   camera to a fixed shot above and west of THAT arm, looking down on it, with
   one box on the right — the Incinerator's conveyor board, one box, the same
   picker (src/incinerator_panel.c):

     - EMPTY: Circle opens the picker; choosing an item puts it in the hand.
     - FULL:  Circle takes it back.

   Cross leaves. >>> ONLY A BLOOD PEARL STAYS. <<< Anything else can be put in
   the box, but on the way out the log says "That doesn't make sense..." and
   the item is the player's again. It never actually left: a non-pearl in the
   box is DISPLAY ONLY (ap_box_item), so no exit path, room change or reset can
   lose it. A pearl really moves (player_blood_pearls - 1, arm bit set), and is
   saved where it is.

   An arm holding a pearl turns HAND DOWN about its shoulder (the red face of
   the export) once the board is closed, and the pearl rests in the hand as a
   fixed sprite — no bob, and it cannot be walked into; the only way to have it
   back is this board.

   >>> THE SOLUTION IS ARMS 2, 3 AND 6, NUMBERED 1..8 FROM NORTH TO SOUTH. <<<
   The switch array runs SOUTH to north (the_head.c's th_arm_switch_z, low Z
   first), so arm n is index 8 - n: the answer is indices 6, 5 and 2. There are
   exactly three pearls in the game (BLOOD_PEARLS_MAX), so "those three hold a
   pearl" is the whole test. Checked when the board closes; it sets
   FLAG_HEAD_ARMS_SOLVED at once, and the unlock sound and "You unlocked the
   door." land as the arm finishes its turn. From then on no arm offers its
   sign or its board — the pearls are spent — and the east door reads "Press O
   to enter".

   SAVED as SaveData.arm_pearls (bit i = switch i holds a pearl) beside the
   flag. */

void arm_puzzle_reset(void);          /* new game: every hand empty            */
int  arm_puzzle_pearls(void);         /* save: bitmask, bit i = switch i       */
void arm_puzzle_set_pearls(int bits); /* load: masked to the eight switches    */

void arm_puzzle_arm(void);            /* room entry: swallow a held Circle,
                                         drop any board, snap every arm to its
                                         rest pose. AFTER the switches are
                                         placed (the_head_init).               */
void arm_puzzle_update(void);         /* per frame: proximity trigger, board,
                                         the arms' turn, the delayed unlock    */
void arm_puzzle_draw(RenderContext *ctx);   /* the sign, or the board overlay */

int  arm_puzzle_active(void);         /* 1 while it owns the camera + input    */
int  arm_puzzle_targeted(void);       /* 1 while an arm is in reach and faced:
                                         the east door's veto, so one Circle
                                         never does two things                 */
int  arm_puzzle_solved(void);         /* FLAG_HEAD_ARMS_SOLVED                 */

#endif
