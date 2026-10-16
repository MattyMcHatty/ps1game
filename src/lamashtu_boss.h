#ifndef LAMASHTU_BOSS_H
#define LAMASHTU_BOSS_H

#include "render.h"

/* =========================================================================
   THE LAMASHTU ENCOUNTER — the director. The body is src/lamashtu.h.
   =========================================================================
   tools/ADDING_A_BOSS_ENCOUNTER.txt's split: everything here is one-off script
   bolted to the Lamashtu Arena - the camera, the two lines and their voice,
   the music, the six phases and their bars, the seal on the door, the death.

   THE OPENING (a cutscene: no control, no HUD)
     The camera glides from wherever the player arrived (the west door) forward
     and up to hover just in front of her, looking down at her face. Then the
     two lines, the Rabisu's and Asag's cadence exactly - SFX_DMNSPEAK keyed as
     each appears, 6 s a line - with the bosses' music (track 6) starting on the
     first. Then back to where the player stood, and control returns.

   THE FIGHT (free play), SEVEN PHASES ON A LOOP
     Phase 1's bars come up out of the floor the moment the encounter arms, so
     they are there as the camera moves. In a phase she stands at its spot,
     crying, pouring Creeps and throwing her shockwave at anyone close (all of
     that is the body's, src/lamashtu.c). A hit on the spot on her back:
       - she screams and shrieks once and FADES OUT over the shriek, unhittable;
       - at the same instant this phase's bars go down and the next phase's
         come up (a gate in BOTH phases stays where it is - it does not dip);
       - faded out, she is placed at the next phase's spot and facing and FADES
         IN over LBE_T_FADE_IN; fully in, the cry starts again and the phase
         runs. Phase 7 is followed by phase 1. (Phase 7 was added after the
         first six: without it, phase 6 could strand a player behind phase 1's
         gates.)

   THE DEATH (a cutscene), on the hit that takes her last point
     The bars go down, the Creeps vanish, the music stops, and the camera turns
     from where the player stood to her FRONT, at the opening's angle. She
     freezes, then shakes and pours light under the explosion, and burns away -
     the Rabisu's sequence and its timing. The camera comes back to the player
     and control returns. FLAG_LAMASHTU_DEAD is set on the killing blow.

   THE VOID (free play, but ROOTED), straight after the death
     NO MUSIC: the chapter's track does not come back. The player can turn and
     attack but not walk (player_rooted, src/camera.h). The arena fades to
     black over LBE_T_VOID_FADE (6 s) - lamashtu_boss_room_light() is the scale
     the room's draw loop multiplies by - and holds black for LBE_T_VOID_BLACK.
     Then lamashtu_boss_leaving() goes to 1 and main.c carries the player into
     THE SEAL (src/seal.h) without a door and without the red LOADING screen;
     the rest of the two seconds of black is the Seal's own opening.

   THE DOOR IS SEALED from the encounter arming to the hand-off into the Seal.
   The arena has no save point, so dying is the only other way out, and that
   loads a save from before the arena.
   ========================================================================= */

void lamashtu_boss_reset(void);      /* new game / title: forget everything     */
void lamashtu_boss_enter(void);      /* the arena's init: park; arm on update   */
void lamashtu_boss_update(void);

/* 1 while the camera is the director's: the opening and the death. */
int  lamashtu_boss_cutscene(void);
/* 1 from the opening to the hand-off into the Seal: the west door is dead. */
int  lamashtu_boss_seals_door(void);

/* THE VOID, after the death (see above). */
int  lamashtu_boss_roots_player(void);   /* 1: turn and attack, no walking     */
int32_t lamashtu_boss_room_light(void);  /* the arena's brightness, 256..0     */
int  lamashtu_boss_leaving(void);        /* 1 once black: go to the Seal now   */

void lamashtu_boss_draw(RenderContext *ctx);          /* the death's lights  */
void lamashtu_boss_draw_overlay(RenderContext *ctx);  /* the two lines       */

#endif /* LAMASHTU_BOSS_H */
