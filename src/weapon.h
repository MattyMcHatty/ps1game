#ifndef WEAPON_H
#define WEAPON_H

#include <psxgte.h>
#include <smd/smd.h>
#include "render.h"

/*
 * Weapon layer: owns the equipped-weapon dispatch shared by every playable area.
 * Each concrete weapon (crucifaxe, grave-olver) provides its own init/draw (and,
 * later, its own update/fire); this module loads them all, cycles the equipped
 * one on L2, and routes update/draw to whichever is current (see player.h
 * current_weapon / player_weapons).
 */
void weapons_init(void);            /* load every weapon's model (startup) */
void weapons_update(void);          /* Triangle switch + the equipped weapon's update */
void weapons_draw(RenderContext *ctx);

/* Weapon-switch animation, shared by each weapon's draw: the outgoing weapon
   slides off the bottom, the incoming one rises up. Returns the view-space Y
   offset (0 = in place, positive = lowered) to add to the model's hold pose. */
int  weapon_switch_offset(void);
int  weapon_switching(void);        /* 1 while a switch animation is in progress */

/* Shared flat-shaded, view-space weapon-model renderer used by each weapon's
   draw. weapon_vs is the fully-built view-space transform for the model; the
   caller decides the hold pose / animation. `gain` scales the model's base
   colours (GTE fixed point, 4096 = 1.0x) before shading, so a model authored
   with dark base colours can be brightened without changing the shared shading.
   Restores the camera view matrix before returning. */
void weapon_render_model(RenderContext *ctx, SMD *smd, MATRIX *weapon_vs,
                         int32_t gain);

/* ---- Shared ranged aim ------------------------------------------------------
   Screen-space hit testing, common to every weapon that reaches across the room.
   It was the Grave-olver's alone (three statics in graveolver.c) until the
   Helluminator needed the same geometry with a wider circle, so it lives here
   rather than in either weapon.

   THE MODEL: picture a circle of `radius` PIXELS around the crosshair. A target
   counts when its on-screen silhouette — the billboard's whole RECTANGLE, width
   included — falls inside that circle, and the line to it is not blocked by
   something nearer. Depth and height never widen the circle; they only decide
   which candidate is nearer. Every enemy in the game is drawn as a camera-facing
   billboard, so its width projects exactly like its height and one divide covers
   each axis.

   `fx`/`fz` are isin(cam_rot)/icos(cam_rot), passed in because the caller
   already has them and they are constant across a whole sweep.

   WHAT THE TWO WEAPONS DO WITH IT DIFFERS, and that difference is theirs to
   keep: the gun spends one round on the NEAREST thing in the circle, the lantern
   burns EVERY thing in it. Only the test is shared. */
int  weapon_aim_in_circle(int32_t ex, int32_t cyc, int32_t ez,
                          int32_t hw, int32_t hh,
                          int32_t fx, int32_t fz,
                          int32_t radius, int32_t range, int32_t *out_depth);

/* 1 if the crosshair line is clear out to `depth` — no wall or solid prop nearer
   than the target under the crosshair, so a closer table stops the shot even
   when the enemy is still inside the circle. */
int  weapon_aim_clear(int32_t fx, int32_t fz, int32_t depth);

/* World point along the crosshair line at forward distance `depth`. Back-projects
   the crosshair's screen offset from centre into view X (perp) and view Y, then
   places it — which is what generalises the aim ray to an off-centre (aimed)
   crosshair. */
void weapon_aim_ray_point(int32_t fx, int32_t fz, int32_t depth,
                          int32_t *px, int32_t *py, int32_t *pz);

/* Projection distance for the two above: matches gte_SetGeomScreen, so a screen
   pixel here is the same size as a screen pixel in the renderer. */
#define WEAPON_PROJ_H  256

#endif
