#ifndef ADAPA_DEATH_H
#define ADAPA_DEATH_H

#include "render.h"

/* -----------------------------------------------------------------------
 * ADAPA'S DEATH — THE DIRECTOR
 *
 * src/adapa.c is the BODY: the float, the burn, the fades, the arming. This
 * file is the one-off script that plays when his last point of health goes
 * (tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 1). It is hadad_grinder.c's death
 * with the grinders taken out, and the spirit is that file's verbatim — "his
 * soul will appear and shoot up into the sky, exactly as when Hadad dies".
 *
 * ======================================================================
 * THE BEAT SHEET
 * ======================================================================
 *   0. The killing burn. adapa.c sets ADP_DYING and FLAG_ADAPA_DEAD on that
 *      frame and plays SFX_ADAPA. This file sees ADP_DYING on its next update
 *      and takes the camera, anchoring the player where they stand.
 *   1. TURN, 0.75 s. The camera stays where the player is and turns — yaw and
 *      pitch — onto his body. From here it is FIXED on him.
 *   2. FADE, ADP_FADE_FRAMES (4.18 s, the clip's length). He fades out where
 *      he is, additively, over the sound that started on the blow.
 *   3. ORB, 1 s. The green spirit fades up out of where his body was and the
 *      camera holds on it.
 *   4. RISE, 3 s. SFX_WOOSH, and the spirit climbs at a constant acceleration
 *      until it is gone, the camera's pitch solved each frame to follow it.
 *   5. BACK, 1 s. Home to the yaw the player had, pitch 0, control returned.
 *      "Adapa's spirit was set free." goes up in the log.
 *
 * THE CAMERA DOES NOT MOVE, ONLY TURNS. The brief's "fixed looking at him" is
 * read as the player's own eyes being held on him — which also means the scene
 * needs no shot that has to be checked against either room's walls, and every
 * frame of it is from somewhere the player was really standing.
 *
 * Nothing pitches through a yaw-only view: both his rooms already build their
 * view with camera_build_view() (the trap in ADDING_A_BOSS_ENCOUNTER.txt
 * mistake 1 cannot bite).
 * ----------------------------------------------------------------------- */

void adapa_death_enter(void);   /* room entry: park                          */
void adapa_death_reset(void);   /* new game / load: park and silence          */

/* Every frame in his two rooms, normal branch AND cutscene branch. Arms
   lazily off adapa_dying(). */
void adapa_death_update(void);

/* The spirit's glow. World space: call from the room's draw with the view
   matrix up, after draw_adapa. Also draws the particles a cutscene's
   suppression of draw_player_systems would otherwise lose. */
void adapa_death_draw(RenderContext *ctx);

/* 1 while the camera and all input belong to the death. main.c's cutscene
   branch, its menu lock and its HUD suppression read this. */
int  adapa_death_cutscene(void);

#endif
