#ifndef RABISU_BOSS_H
#define RABISU_BOSS_H

#include "render.h"

/* -----------------------------------------------------------------------
 * The Rabisu ENCOUNTER — the Garden Courtyard's boss fight, end to end.
 *
 * src/rabisu.c owns the BODY: the model, the animation, the damage, and the
 * combat AI. This file owns everything ABOUT the fight that is a one-off
 * script: the camera, the sixteen lights in the lawn, the subtitles, the
 * music cue, the sealed door, and the twelve seconds of death.
 *
 * Read rabisu.h's "director API" section alongside this — the two files talk
 * through those six functions and nothing else, which is what keeps a second
 * Rabisu droppable into another room with no cutscene attached.
 *
 * THE SHAPE OF IT
 *   The player walks in through the east cage door. The camera is taken away
 *   from them and cranes out over the middle of the garden. Two seconds of
 *   nothing. Then sixteen polys of lawn start to glow, two at a time over
 *   three seconds, pulsing white to orange to red. When the last pair lights,
 *   the boss comes up through the grass over five seconds. The lights die back
 *   over two. THEN the music starts, and two lines of scripture go by at six
 *   seconds each. The camera drops to the south wall, and the player has
 *   control back — standing where the camera left them, looking north at it.
 *
 *   The door does not work again until the fight is over (see
 *   rabisu_boss_seals_door). There is no save point in this room, so the
 *   encounter is genuinely one sitting.
 *
 *   On the last point of damage the camera goes back to the crane shot, the
 *   boss goes back to the middle, and the music stops. It freezes for two
 *   seconds, then shakes itself apart for six while light pours out of its
 *   crown, its wing tips and its chest, and then it burns away. The camera
 *   returns to where the player was standing and hands control back.
 *
 * WHY THE CAMERA MOVES ARE GLIDES AND NOT CUTS
 *   The lightswitch puzzle hard-cuts to its payoff shot and the piano puzzle
 *   glides; both are in the codebase to copy. This uses the piano's glide
 *   throughout because the brief asks for it in so many words ("move smoothly
 *   from the player's starting position"), and because a cut would lose the
 *   one thing the opening move is for — showing the player that the middle of
 *   the garden is where they should be looking.
 * ----------------------------------------------------------------------- */

/* Called from garden_courtyard_init() on every entry. Parks the director; the
   reveal starts by itself on the first update that finds a living boss, which
   sidesteps the ordering problem that the boss is not PLACED until world_enter
   runs, after the room init. */
void rabisu_boss_enter(void);

/* New game / restart: forget everything. */
void rabisu_boss_reset(void);

void rabisu_boss_update(void);
/* World-space: the lawn lights and the death lights. Call from the room's draw
   with the plain camera view matrix loaded in the GTE. */
void rabisu_boss_draw(RenderContext *ctx);
/* Screen-space: the two subtitle lines. Call after the world draw. */
void rabisu_boss_draw_overlay(RenderContext *ctx);

/* 1 while the director owns the camera and the player has no control. main.c
   uses this to skip update_camera, the room's collision and the door, exactly
   as it does for the stove and lightswitch payoffs. */
int rabisu_boss_cutscene(void);

/* 1 while the east cage door must not respond. Covers the whole encounter,
   reveal and death sequence included — not just the fight — so the player
   cannot walk out through the middle of the opening shot. */
int rabisu_boss_seals_door(void);

#endif
