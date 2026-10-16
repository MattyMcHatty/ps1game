#ifndef WEAPON_H
#define WEAPON_H

#include <psxgte.h>
#include <smd/smd.h>
#include "render.h"

/*
 * Weapon layer: owns the equipped-weapon dispatch shared by every playable area.
 * Each concrete weapon (crucifaxe, grave-olver) provides its own init/draw (and,
 * later, its own update/fire); this module loads them all, switches the equipped
 * one on Triangle (tap = next owned, hold = the weapon wheel), and routes
 * update/draw to whichever is current (see player.h current_weapon /
 * player_weapons).
 */
void weapons_init(void);            /* load every weapon's model (startup) */
void weapons_update(void);          /* Triangle switch/wheel + the equipped weapon's update */
void weapons_draw(RenderContext *ctx);

/* Weapon-switch animation, shared by each weapon's draw: the outgoing weapon
   slides off the bottom, the incoming one rises up. Returns the view-space Y
   offset (0 = in place, positive = lowered) to add to the model's hold pose. */
int  weapon_switch_offset(void);
int  weapon_switching(void);        /* 1 while a switch animation is in progress */

/* ---- The selection wheel ----------------------------------------------------
   Four boxes in a cross around the screen centre, one per d-pad direction. Two
   wheels use it: the Grave-olver's AMMO wheel (hold R2, graveolver.c) and the
   WEAPON wheel (hold Triangle, weapon.c). Each owns what sits in its boxes and
   when it is open; this layer owns only where the boxes are and how they look,
   so the two can never drift apart. A press shorter than WHEEL_HOLD_FRAMES is
   a tap and keeps that button's old job (top-up / cycle to the next weapon). */
#define WHEEL_HOLD_FRAMES 15    /* held this long opens a wheel (0.25 s)        */
#define WHEEL_ICON        24    /* icon size — the inventory menu's ICON_SIZE    */

/* Wheel OT layers: inside the UI block (0..15), behind the HUD (0..6) and the
   gun's own flash/reticule, so a wheel never covers the log or the health. */
#define OT_WHEEL_ICON    10
#define OT_WHEEL_COUNT    8     /* shadow lands one step behind, at 9           */

enum { WHEEL_UP, WHEEL_LEFT, WHEEL_RIGHT, WHEEL_DOWN, WHEEL_SLOTS };

/* The box a d-pad press (this frame's NEW presses) points at, or -1. */
int  wheel_dpad_dir(int dpad_just);
/* Draw box `slot` (WHEEL_*), wearing the inventory cursor if `picked`, and
   return in *ix/*iy where its WHEEL_ICON-sized icon goes. Draw the icon into
   OT_WHEEL_ICON, then call wheel_draw_done once after the last box. */
void wheel_draw_box(RenderContext *ctx, int slot, int picked, int *ix, int *iy);
void wheel_draw_done(RenderContext *ctx);

/* 1 while the weapon wheel is up: it owns the d-pad (camera.c roots the player)
   and the crosshair is hidden, as for graveolver_wheel_open. */
int  weapon_wheel_open(void);

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

/* The same test with a BOX of half-extents rx x ry pixels instead of a square
   `radius` — the circle above is this with rx == ry (the test was always a box;
   "circle" is the name it grew up with). The Scattershot's wide, shallow field
   is the reason it exists. */
int  weapon_aim_in_box(int32_t ex, int32_t cyc, int32_t ez,
                       int32_t hw, int32_t hh,
                       int32_t fx, int32_t fz,
                       int32_t rx, int32_t ry, int32_t range, int32_t *out_depth);

/* 1 if the line from the camera STRAIGHT TO a world point is clear of walls and
   solid props. weapon_aim_clear below asks about the crosshair line instead,
   which is right when the field is a few pixels across and wrong when it is a
   third of the screen: a target 90 px off the crosshair would be judged by a
   wall it is nowhere near. A weapon that hits everything in a wide field asks
   this, once per target, at the target. */
int  weapon_aim_clear_to(int32_t px, int32_t py, int32_t pz);

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
