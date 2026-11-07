#ifndef LAVA_H
#define LAVA_H

#include "render.h"
#include "title.h"   /* GameState */

/* =========================================================================
   CHAPTER 4'S BURNING LAVA
   =========================================================================
   Wherever the lava texture is on an Underworld floor, standing in it costs
   LAVA_HP_PER_SECOND, and for as long as the player is in it the screen
   pulses orange - one pulse per point lost, so the flash IS the damage tick.

   WHERE THE LAVA IS comes from src/lava_floors.h, a per-room bitmask GENERATED
   from the room's visual mesh by tools/gen_lava_floors.py. Nothing here names
   a room: lava_update() finds the mask by area, so a new Underworld room
   burns the moment it has a line in that script's ROOMS and the generator has
   been run. A room with no line has no burning lava. See tools/ADDING_A_ROOM.txt,
   "UNDERWORLD (CHAPTER 4) ROOMS".

   THE TEST is the player's centre (cam_x, cam_z) on a lava cell AND their feet
   at the lava's plane - so a bridge or a slab drawn over the lava never burns,
   and nor does anything the generator has already carved out (paths, the
   Seal, ruins).

   THE RATE is exact at 60 frames: one point on the frame the player steps in,
   then one every LAVA_TICK_FRAMES while they stay. Stepping out drops the
   count, so hopping in and out costs a point a hop rather than dodging the
   tick. Gated by DBG_INFINITE_LIFE like every other hurt (player_hurt); the
   flash is not, so the lava still reads as lava with the cheat on.

   SFX_HURT plays once on the way in, the grinder's rule (src/grinder_puzzle.c):
   a hurt sample on every tick would be a buzz, not a warning. */

#define LAVA_HP_PER_SECOND  5
#define LAVA_TICK_FRAMES   (60 / LAVA_HP_PER_SECOND)   /* 12 */

/* Clear the burn. reset_game's; lava_update recomputes it every frame anyway. */
void lava_reset(void);

/* Once a frame from update_current_area, for EVERY area - a room with no mask
   costs one table walk. Applies the damage and raises game_over. */
void lava_update(GameState area);

/* 1 while the player is standing in burning lava (this frame). */
int lava_burning(void);

/* The orange pulse: an additive full-screen TILE in the poison wash's layer
   (src/player.c), over the scene and under the HUD. Nothing when not burning. */
void lava_draw_overlay(RenderContext *ctx);

#endif
