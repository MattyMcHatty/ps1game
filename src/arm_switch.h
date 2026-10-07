#ifndef ARM_SWITCH_H
#define ARM_SWITCH_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — the row is tagged with its area */

/* -----------------------------------------------------------------------
 * The Arm Switches — a stone arm reaching out of The Head's east wall, set
 * over a Lamashtu's elbow in the relief. EIGHT of them, in one north-south
 * row along the bottom register of that wall. Chapter 3 furniture on the
 * Gula Tablet's terms: a deferred texture registration, a small SMD read once
 * in main() and held for the run, and a narrow arm_switch_upload_texture()
 * for the room that draws it.
 *
 * >>> THE EXPORT FIXES X AND Y; THE ROOM PICKS EACH Z. <<< Arm Switch.smx was
 * authored IN PLACE in The Head's Blender scene — x[2221,2430] y[-254,-112]
 * z[2572,2612], partly inside the east wall (x=2400) — so its vertices are
 * already The Head's world coordinates and no model matrix is needed (the
 * Gula Tablet's arrangement, src/gula_tablet.h). Every switch is that mesh
 * slid along Z ONLY: same X, same Y, so the eight stand in one dead-straight
 * row. Each Z is in the_head.c, where the elbows are derived from the
 * relief's UVs.
 *
 * NO COLLISION. The whole row stands east of The Head's collision wall at
 * x=2210, where the player cannot go.
 *
 * TEXTURE: "lamashtu arm", \TEXCTCMB\LMSHARM.TIM, 4bpp 128x128 (the 64x64
 * source stretched, since the UVs span the full tile) at x768 y0, the
 * lamashtu tablet's page — the one other Chapter 3 tenant, drawn only in the
 * Catacombs Entry and the Outside Catacombs, both of which put it back with
 * their own full uploaders on entry; every mansion/garden occupant is
 * re-uploaded by its own room — with a palette of its own at (720,480).
 * Voff 0, so the room's 128 texture window serves it.
 *
 * Area-tagged, so arm_switches_draw() can be called unconditionally and is a
 * no-op in every other room.
 * ----------------------------------------------------------------------- */

#define ARM_SWITCH_MAX 8

void arm_switch_load_assets(void);     /* startup: geometry + deferred registration */
void arm_switch_upload_texture(void);  /* room entry: pure LoadImage, no CD           */

void arm_switches_clear(void);

/* Stand a switch in `area` with its centre at world Z `z`. X and Y are the
   export's own, so every switch shares them. Up to ARM_SWITCH_MAX; all must
   be in the same area (a later call with another area re-tags the row). */
void arm_switch_add(GameState area, int32_t z);

int     arm_switch_count(void);     /* how many arm_switch_add() placed     */
int32_t arm_switch_z(int i);        /* switch i's centre Z, as placed       */

/* Switch i's pose this frame: `angle` (4096 = a full turn) about the shoulder,
   positive = HAND DOWN, and whether a Blood Pearl rests in its hand. The pivot
   is the export's one untextured face, measured at load (src/arm_switch.c).
   The pearl is a fixed sprite (item_pickup_draw_fixed), not a pickup: it
   cannot be walked into, does not bob, and is saved by whoever owns the pose —
   src/arm_puzzle.c. arm_switches_clear() zeroes every pose. */
void arm_switch_set_pose(int i, int32_t angle, int pearl);

void arm_switches_draw(RenderContext *ctx);

#endif
