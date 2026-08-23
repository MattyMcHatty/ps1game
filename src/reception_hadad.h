#ifndef RECEPTION_HADAD_H
#define RECEPTION_HADAD_H

/* -----------------------------------------------------------------------
 * THE RECEPTION ENCOUNTER — THE DIRECTOR
 *
 * src/hadad.c is the BODY: where the fourth Hadad appears, the four stages he
 * walks through two storeys of this room, and the fact that he is solid and
 * lethal while he does. This file is the ENCOUNTER: the house shaking as the
 * player walks in, and the place on the balcony that brings him through the
 * ceiling. The split is STEP 1 of tools/ADDING_A_BOSS_ENCOUNTER.txt and it is
 * not cosmetic — the walk would work unchanged in another room; neither of the
 * two things below would.
 *
 * It is to the Reception what east_hall_quake.c is to the East Hall and what
 * hadad_library.c is to the Library Destroyed, and it is deliberately built out
 * of both: the quake half is the East Hall's, function for function, and the
 * handover half is the Library's.
 *
 * They talk through the two functions in the marked block at the foot of
 * hadad.h (hadad_reception_instance / _begin) and nothing else.
 *
 * ======================================================================
 * THE BEAT SHEET
 * ======================================================================
 *   1. The player walks into the Reception with FLAG_HADAD_TWO set — the four
 *      key stones are back out of the Attic Exit's door and the house has
 *      closed around them. Four of this room's six doors are already rubble and
 *      the save point is already gone (reception.h); they arrive on the SECOND
 *      LEVEL, through the East Hall's double door, because that is the only way
 *      in that is left.
 *
 *   2. THE HOUSE SHAKES. The same two-second hold and three-second shake, the
 *      same six overlapping rumbles and the same log line the Attic Exit ran
 *      when the stones came out and the East Hall ran on the way out of the
 *      wrecked Library — src/quake.c, unchanged. No input for the five seconds,
 *      and the camera never leaves the arrival spot: what the player watches
 *      move is the room they have just walked into.
 *
 *   3. Control comes back. Free play, on a balcony, in silence.
 *
 *   4. THEY CROSS ONTO THE FLOOR IN FRONT OF THE STAIR HEAD. Hadad appears in
 *      the ceiling in front of the north-west door — the West Corridor door,
 *      the room's only way on — and drops onto the second level in front of it.
 *      From there hadad.c has him: he walks to the head of the stair and then
 *      down after the player, by one of two routes, to the same spot at its
 *      foot — and from there onto a CIRCUIT of the whole room that never ends,
 *      round to the Kitchen Dining door, up onto the second level in one leap,
 *      and back down the stair to start again. See THE RECEPTION ENCOUNTER in
 *      src/hadad.h.
 *
 *      >>> WHICH MEANS THERE IS NO STEP 5. <<< Nothing here ends the encounter.
 *      He is never rooted, the stalker track never stops, and the only way out
 *      of the room is the West Corridor door the player watched him drop in
 *      front of.
 *
 * ONCE, AND FOR GOOD. Both halves are gated on FLAG_RECEPTION_HADAD, and it is
 * set at the ARM — on the entry, before the shake — for the reason
 * FLAG_EAST_HALL_RUBBLE is: the beat is a minute long and a reset, a debug jump
 * or a death inside it must not leave a world where the encounter is still
 * pending. A later visit finds an empty, silent room.
 *
 * >>> WHICH MEANS THE TRIGGER MAY NOT BE MISSABLE. <<< A trigger the player can
 * walk around is a trigger that loses the whole encounter, because nothing will
 * ever arm it again. HRH_TRIG is therefore TWO tests and not one: the radius
 * round the stair head, which is what the beat is about and what will fire in
 * practice, and a BACKSTOP line further west that catches anybody who hugged
 * the north wall on their way to the West Corridor door. The second level's
 * north band is the only route between the two doors and it is one floor, so
 * between them they cannot be dodged.
 *
 * The main.c contract is the one quake.h states: a branch that routes the frame
 * to reception_hadad_update() and skips update_camera / apply_collision /
 * apply_height, plus the HUD and menu suppression that goes with it.
 * ----------------------------------------------------------------------- */

/* New game, or entry to a room this beat does not belong to: park it and
   silence the five rumble voices. */
void reception_hadad_reset(void);

/* Room entry. Parks the director; reception_init() calls it, and it does NOT
   decide anything — reception_init runs while game_flags may still hold the
   pre-load values, and before world_enter() has placed anybody. */
void reception_hadad_enter(void);

/* Called from main.c's post-entry re-derive block, AFTER world_enter and
   savegame_apply_pending, for the same reason reception_apply_flags() is: the
   two flags this reads are only correct by then. Starts the beat if the room is
   sealed and it has not run, and sets FLAG_RECEPTION_HADAD; otherwise a no-op.

   Deliberately NOT gated on which door the player came in by. The brief says
   they can only arrive from the East Hall once the room is sealed, and with
   four of the six doors buried that is true — but if a route were ever added,
   an encounter that silently declined to happen would be far worse than one
   that ran from the wrong doorway. */
void reception_hadad_arm_on_entry(void);

/* 1 while the shake owns the camera and all input. FALSE for the rest of the
   encounter, which is ordinary free play with something walking at you. */
int  reception_hadad_active(void);

/* One frame. Called from BOTH main.c branches — the camera-locked one above
   while reception_hadad_active(), and the room's ordinary one, which is where
   the trigger is actually watched. */
void reception_hadad_update(void);

#endif
