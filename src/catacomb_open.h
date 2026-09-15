#ifndef CATACOMB_OPEN_H
#define CATACOMB_OPEN_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   THE CATACOMB DOORS OPEN — the middle beat of Asag's ending.
   =========================================================================
   Asag's death sequence used to end by handing the camera back to a player
   standing in a finished room with no way out of it, which src/asag_boss.h
   called the one thing still missing. The way out is this: the fade ends, the
   red LOADING screen goes up, and the player is put down in the OUTSIDE
   CATACOMBS in front of the facade they have walked past all game.

   THE SHOT, and it is one shot — the camera does not move once:

     0.0 s   a fixed vantage south of the catacomb mouth, up at the height of
             the doors' own tops and looking down onto them. Held, doors shut.
     2.0 s   THE SLIDE. The two leaves come apart, the left due west and the
             right due east, CD_SLIDE_FULL each over CO_T_SLIDE — eight seconds
             for five hundred units, which is sixty-odd units a second and is
             the "very slowly" the brief asks for. SFX_MCHNE_GH, the
             house's machinery grind, runs under all eight of them: the
             clip is 1.8 s and this file retriggers it five times on
             CO_T_GRIND, then keys the voice off on the frame the leaves
             land, so the noise starts and stops with the stone and with
             nothing else.
    10.0 s   THE GLOW. The fifteen black backing polys behind the doorway
             (src/outside_catacombs.h) light up — not flat white across the
             panel but a RIM, brightest against the four edges of the opening
             and falling away to black in the middle, so it reads as something
             shining out around the doorway rather than as a lamp hung in it.
             The shape is the room's, in draw_outside_catacombs_smd(); this
             module supplies only how far up the ramp is.
    12.0 s   held on the lit doorway, and then the cut.

   ...and then the red LOADING screen again, and THE HATCH, where
   src/hatch_arrival.h drops the player off the well and gives them back their
   legs. This module does not know about either transition: it reports
   catacomb_open_finished() for one frame and main.c takes them, the same
   division hatch_puzzle_drop_done() has with the drop into the arena.

   >>> IT OWNS THE FLAG, AND THAT IS NOT AN ACCIDENT OF WHERE IT WAS EASY TO
   PUT. <<< FLAG_ASAG_DEAD is set on the last frame of the hold, because
   catacomb_doors_init() POSES THE LEAVES FROM IT on room entry and this
   scene's own arrival goes through that init. See the flag's note in
   src/player.h, which is the long version.

   ---- WHAT THE ROOM LOOKS LIKE AFTERWARDS -----------------------------------
   Everything the flag turns on is somebody else's:
     the leaves stay apart              src/catacomb_doors.c, from the flag
     the backing polys stay lit         catacomb_open_glow() below, from the flag
     the doorway offers Circle          src/outside_catacombs.c, from the flag
   Which means a player who comes back to this room later — by the south gate,
   by a title-screen load, by a debug jump — finds exactly what the scene left,
   with none of it needing the scene to have been the thing that ran.
   ========================================================================= */

/* ROOM ENTRY from the arena, and from nowhere else: main.c arms this in the
   STATE_LOADING branch when the room being entered is the Outside Catacombs and
   the room being left is Asag's arena. Anchors the player where the room's own
   spawn put them and takes the camera. */
void catacomb_open_start(void);

/* Park it. From outside_catacombs_init(), i.e. on EVERY arrival, so an ordinary
   walk in through the south gate cannot inherit a half-played scene from a
   previous visit. Called BEFORE catacomb_open_start() on the one path that
   arms it. */
void catacomb_open_reset(void);

/* 1 while the scene owns the camera. main.c needs it in the cutscene
   early-return at the top of update_current_area(), in the `cutscene` list that
   suppresses the Start menu and the weapon overlay, and in the HUD exception —
   the same three places src/asag_boss.h asks for. */
int  catacomb_open_active(void);

/* One game frame of the scene. Safe when it is not running. */
void catacomb_open_update(void);

/* 1 on the SINGLE frame the hold ends. main.c turns that into the transition
   into The Hatch; see the note above. */
int  catacomb_open_finished(void);

/* How lit the fifteen black backing polys are, 0..256, read per frame by
   src/outside_catacombs.c's mesh draw.

   >>> IT ANSWERS FOR THE WHOLE SESSION AND NOT JUST FOR THE SCENE. <<< While
   the scene is running it is the ramp; every other time it is FLAG_ASAG_DEAD,
   which is what keeps the doorway lit on a visit that did not play the scene.
   One accessor rather than a ramp here and a flag test in the room, so the two
   cannot disagree about a frame. */
int32_t catacomb_open_glow(void);

#endif
