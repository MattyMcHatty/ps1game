#ifndef ASAG_BOSS_H
#define ASAG_BOSS_H

#include "render.h"

/* =========================================================================
   ASAG'S DIRECTOR — THE OPENING SCENE IS REAL; THE FIGHT IS NOT.
   =========================================================================
   This file used to say it was a placeholder meant to be thrown away. Half of
   it still is, and the halves are worth stating separately because they want
   different things from the next person:

   WHAT IS FINISHED is the encounter's OPENING, exactly as briefed. The camera
   arrives in the AIR above the landing looking straight down, falls to it,
   bounces twice with the hurt sound on the landing, and pans up onto Asag —
   who has been SITTING two seconds into his faint throughout, held there. The
   faint is then released, and as it resolves and carries him back to Home the
   camera drifts UP AND TO THE RIGHT, ending looking slightly down at his face.
   He drops into a looping idle; his two boils light up over two seconds and are
   left pulsing; the demon speech and the boss music start together and two
   lines of subtitle play over them; and then the camera returns to the landing,
   levels off, and the player has it back standing exactly where the shaft
   dropped them. All of that is src/asag_boss.c's phases ABE_DROP through
   ABE_HANDOVER and should survive the work below untouched.

   >>> EVERY CAMERA POSITION IN IT IS AN OFFSET FROM THE LANDING, CAPTURED ON
   THE ARM. <<< The scene ends on the same numbers it started from, which is how
   "control returns at the original starting position" is guaranteed rather than
   asserted — and it means the whole scene follows AA_SHAFT_X/Z and AA_EYE_Y if
   the landing ever moves. The .c has the shot list and the arithmetic behind
   each offset, including why the vantage's downward look is only four degrees
   (the arena's roofline is at y=-1000 and forbids more).

   WHAT IS STILL A PLACEHOLDER is everything after the handover. ABE_FIGHT runs
   the old demo — the four attack clips and the idle, end to end on a loop —
   which decides nothing and can hurt nobody. There is no health, no attack, no
   damage, no death sequence and no seal.

   WHAT THE REST NEEDS, in the order the runbook puts it:
     tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 6   the movement and the attacks
                                      STEP 11   `dying` vs `dead`, two flags
                                      STEP 9    the seal — AND IT NEEDS A DOOR
                                                FIRST. The arena has no exit at
                                                all (src/asag_arena.h says so in
                                                as many words), so the seal
                                                predicate has nothing to gate
                                                and this header does not declare
                                                one. It belongs in
                                                asag_arena_exit_sealed() when it
                                                comes, so the trigger and the
                                                floating prompt cannot disagree.
     src/rabisu_boss.c                          the reference encounter, and the
                                                one this file is shaped after

   THE INTERFACE THE BODY OFFERS is the whole of src/asag.h. This module uses
   asag_play(), asag_play_at(), asag_stop(), asag_clip_done(),
   asag_clip_loaded(), asag_set_visible(), asag_set_frozen() and
   asag_face_point() — the last three added for this scene — plus
   asag_arena_set_boil_glow(). It reaches past none of them into the body's
   internals, which is the shape the fight should keep.

   NOTE asag_face_point() AND NOT asag_body_centre(). Asag is 1669 units long
   and lies along the view axis, so his centre is his flank and sits some 800
   units further off than his head; a shot of his face has to aim at his face.
   asag_body_centre() is still there for a wide shot that wants the whole
   silhouette, and asag_collide()'s box is a third thing again.
   ========================================================================= */

/* Park the director and clear everything the scene touched. Called from
   asag_arena_init(), i.e. on EVERY arrival — the drop, a debug level-select
   jump, a load — so all three get the same scene from the top. It does NOT arm
   it: the first update does, because the model and the world's placement both
   land after the room's init. See the .c. */
void asag_boss_reset(void);

/* One game frame. Runs the scene, or the placeholder fight once it is over.
   Call from BOTH of main.c's branches for this room — the cutscene early-return
   at the top of update_current_area() and the free-play branch — and in both
   call it BEFORE asag_update(). That order is worth exactly one frame:
   asag_update() raises the "holding the last frame" flag and this reads it, so
   in this order a clip's final pose has been drawn once before the switch.
   Swapped, every clip in a chained sequence ends a beat early. Safe with no
   model loaded. */
void asag_boss_update(void);

/* 1 while the script owns the camera — i.e. from the arrival until control is
   handed back. FALSE during the fight, which is free play.

   main.c needs this in three places, all of them listed in
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 9: the early-return branch at the top
   of update_current_area(), the `cutscene` flag that suppresses the Start menu
   and the weapon overlay, and the HUD exception — nothing can touch the player
   during the opening, so a health bar over it is a frame of UI insisting on a
   fight that is not on yet. */
int  asag_boss_cutscene(void);

/* The subtitles, in screen space. Call LAST from asag_arena_draw(), after the
   world: it sorts into the menu-reserved OT range so the lines sit on top of
   everything. A no-op outside the two speaking phases. */
void asag_boss_draw_overlay(RenderContext *ctx);

#endif
