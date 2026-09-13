#ifndef ASAG_BOSS_H
#define ASAG_BOSS_H

#include "render.h"

/* =========================================================================
   ASAG'S DIRECTOR — THE OPENING SCENE, THE HANDOVER, AND THE DEATH.
   =========================================================================
   >>> THE PLACEHOLDER IS GONE. <<< For three passes this header said half the
   file was a demo meant to be thrown away, and ABE_FIGHT cycled the four attack
   clips end to end so the animation could be looked at. The fight exists now
   (src/asag_fight.c) and this file is a whole encounter: an opening, a handover,
   free play it does not interfere with, and a death sequence.

   THE THREE FILES, because the split is the thing to understand first:
       src/asag.c        the mesh, the clips, the position track, the collider
                         and the geometry accessors. Decides nothing.
       src/asag_fight.c  the combat AI: 20 HP, the exposure windows, the two
                         boils, the attack loop and every attack's damage.
       src/asag_boss.c   THIS. The camera, the subtitles, the music cues and
                         the two scripted bookends. Owns the camera and nothing
                         else. It starts the track when Asag speaks and STOPS IT
                         ON THE KILLING BLOW — see begin_death() for why that
                         moment rather than the end of the fade.

   The traffic between this file and the fight is three calls: asag_fight_begin()
   on the last frame of the handover, asag_fight_dying() watched during
   ABE_FIGHT, and asag_fight_stop() + asag_fight_set_dead() inside the death.
   It reaches past none of them.

   WHAT THE OPENING IS. The camera arrives in the AIR above the landing ALREADY
   AIMED AT ASAG, falls to it, and bounces twice with the hurt sound on the
   landing — Asag SITTING two seconds into his faint throughout, held there. The
   faint is then released, and as it resolves and carries him back to Home the
   camera drifts UP, TO THE RIGHT and IN, ending looking down at his face. He
   drops into a looping idle; his two boils light up over two seconds and he
   LEANS HALF OUT OF THE WALL as they do; the demon speech and the boss music
   start together and two lines of subtitle play over them; and then the camera
   returns to the landing and levels off while he withdraws, and the player has
   it back standing exactly where the shaft dropped them. All of that is
   src/asag_boss.c's phases ABE_DROP through ABE_HANDOVER, and the fight was
   built without touching one line of it.

   (TWO BEATS HAVE GONE SINCE THE FIRST VERSION, both described in the .c: the
   drop used to look straight DOWN at the mud, which cost a 1.2 s pan-up beat to
   get off again and put the boss on screen four seconds after the cut; and
   ABE_BOILS used to be a static pause.)

   >>> EVERY CAMERA POSITION IN IT IS AN OFFSET FROM THE LANDING, CAPTURED ON
   THE ARM. <<< The scene ends on the same numbers it started from, which is how
   "control returns at the original starting position" is guaranteed rather than
   asserted — and it means the whole scene follows AA_SHAFT_X/Z and AA_EYE_Y if
   the landing ever moves. The .c has the shot list and the arithmetic behind
   each offset, including why the vantage had to be moved IN toward him as well
   as up: the arena's roofline is at y=-1000, so height alone cannot buy more
   than about six degrees of downward look, and the rest comes off the range.

   >>> AND THE SCENE NOW LEAVES THE BODY SOMEWHERE, WHICH IT DID NOT BEFORE.
   <<< The half-lean parks Asag 484 units out of the wall, and a clip's position
   track is ABSOLUTE (src/asag.h) — so an attack begun from there would SNAP him
   Home on its first frame. ABE_HANDOVER ramps him back for exactly that reason.
   Anything else added to this scene that moves him owes the same debt.

   WHAT THE DEATH IS: the camera is taken back where the player is STANDING (not
   where they landed — they killed him from wherever they killed him from), it
   drifts to the vantage the opening already solved, he plays an idle while a
   position ramp carries him Home, he freezes, he vibrates, he burns away, and
   the camera comes back. That is the Rabisu's RBE_D_* sequence with the three
   differences the timing block in the .c sets out, all of which come from Asag
   having no position and no facing of his own.

   >>> WHAT IS STILL MISSING IS THE WAY OUT OF THE ROOM, AND IT IS NOW THE ONLY
   THING. <<< The arena has no exit at all — src/asag_arena.h says so in as many
   words — and it mattered less while the fight could not be won. It is the
   whole of what is left:
     tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 9   the seal. It needs a DOOR first.
                                                The predicate belongs in
                                                asag_arena_exit_sealed(), so the
                                                trigger and the floating prompt
                                                cannot disagree; this file would
                                                supply `state != ABE_IDLE &&
                                                state != ABE_DONE` and call the
                                                re-arm on the frame it lifts.
     src/rabisu_boss.c                          the reference encounter, and the
                                                one this file is shaped after

   THE INTERFACE THE BODY OFFERS is the whole of src/asag.h. This module uses
   asag_play(), asag_play_at(), asag_clip_done(), asag_clip_loaded(),
   asag_set_visible(), asag_set_frozen(), asag_face_point() and — added for the
   death — asag_set_shake(), asag_set_fade() and asag_ramp_home(). Plus
   asag_arena_set_boil_glow(). It reaches past none of them into the body's
   internals, and the fight keeps the same shape.

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

/* One game frame of the SCRIPT: the opening, then nothing at all while the
   fight runs, then the death. It does not drive the fight — src/asag_fight.c
   has its own update and main.c calls it separately.

   Call from BOTH of main.c's branches for this room — the cutscene early-return
   at the top of update_current_area() and the free-play branch — and in both
   call it BEFORE asag_update(). That order is worth exactly one frame:
   asag_update() raises the "holding the last frame" flag and this reads it, so
   in this order a clip's final pose has been drawn once before the switch.
   Swapped, every clip in a chained sequence ends a beat early. Safe with no
   model loaded. */
void asag_boss_update(void);

/* 1 while the script owns the camera — the opening AND the death sequence.
   FALSE during the fight, which is free play, and FALSE once the death is over,
   which is the rest of the session. Both exclusions matter: leaving either
   inside would suppress the player's camera, menu and HUD while they still had
   control.

   main.c needs this in three places, all of them listed in
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 9: the early-return branch at the top
   of update_current_area(), the `cutscene` flag that suppresses the Start menu
   and the weapon overlay, and the HUD exception — nothing can touch the player
   during the opening, so a health bar over it is a frame of UI insisting on a
   fight that is not on yet. */
int  asag_boss_cutscene(void);

/* The death's lights: additive world-space glows hung ALONG the body while it
   comes apart, ramping on over the burn and riding the body's own fade out so
   they never outlive the thing they are pouring out of. Call from
   asag_arena_draw() with the PLAIN view matrix loaded, after the fight's own
   draws and BEFORE the subtitles. A no-op outside the two burning phases. */
void asag_boss_draw(RenderContext *ctx);

/* The subtitles, in screen space. Call LAST from asag_arena_draw(), after the
   world: it sorts into the menu-reserved OT range so the lines sit on top of
   everything. A no-op outside the two speaking phases. */
void asag_boss_draw_overlay(RenderContext *ctx);

#endif
