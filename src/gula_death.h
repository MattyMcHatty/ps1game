#ifndef GULA_DEATH_H
#define GULA_DEATH_H

#include "render.h"

/* -----------------------------------------------------------------------
 * GULA'S DEATH — THE DIRECTOR
 *
 * src/gula.c is the BODY: the fade-in, the howl, the waves, the damage. This
 * file is the one-off script that plays when her last point of health goes
 * (tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 1). It is src/adapa_death.c, beat for
 * beat - "a death animation the same as Hadad and Adapa", with the same sound
 * (her own cry on the blow, as Adapa's is his, then the spirit's woosh) and the
 * spirit shooting up into the air.
 *
 * ======================================================================
 * THE BEAT SHEET
 * ======================================================================
 *   0. The killing blow. gula.c sets GULA_DYING and FLAG_GULA_DEAD on that
 *      frame, stops the howl loop and plays the howl once as her death cry.
 *      This file sees GULA_DYING on its next update and takes the camera,
 *      anchoring the player where they stand.
 *   1. TURN, 0.75 s. The camera stays where the player is and turns - yaw and
 *      pitch - onto her body. From here it is FIXED on her.
 *   2. FADE, GULA_HOWL_FRAMES (3.38 s, the clip's length). She fades out where
 *      she stands, additively, over the howl that started on the blow.
 *   3. ORB, 1 s. The green spirit fades up out of where her body was.
 *   4. RISE, 3 s. SFX_WOOSH, and the spirit climbs at a constant acceleration
 *      until it is gone, the camera's pitch solved each frame to follow it.
 *   5. BACK, 1 s. Home to the yaw the player had, pitch 0, control returned,
 *      the Kennels' door unsealed and re-armed. "Gula's spirit was set free."
 *
 * The Kennels builds its view with camera_build_view(), so the pitch is honoured
 * (ADDING_A_BOSS_ENCOUNTER.txt mistake 1 cannot bite).
 * ----------------------------------------------------------------------- */

void gula_death_enter(void);   /* room entry: park                          */
void gula_death_reset(void);   /* new game / load: park and silence          */

/* Every frame in the Kennels, normal branch AND cutscene branch. Arms lazily
   off gula_dying(). */
void gula_death_update(void);

/* The spirit's glow, and the particles a cutscene's suppression of
   draw_player_systems would otherwise lose. World space: call from the room's
   draw with the view matrix up, after draw_gula. */
void gula_death_draw(RenderContext *ctx);

/* 1 while the camera and all input belong to the death. main.c's cutscene
   branch, its menu lock and its HUD suppression read this. */
int  gula_death_cutscene(void);

#endif
