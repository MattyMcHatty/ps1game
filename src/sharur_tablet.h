#ifndef SHARUR_TABLET_H
#define SHARUR_TABLET_H

#include <stdint.h>
#include "render.h"

/* -----------------------------------------------------------------------
 * SHARUR'S TABLET - a stone slab standing in Sharur's Trial, 1200 long, 100
 * thick and 600 tall, on the diagonal between (-2117,-822) and (-1235,-1709).
 * Chapter 4's first puzzle and the room's only prop.
 *
 * >>> ITS MESH IS ITS POSITION AND ITS COLLISION, AS THE GULA TABLET'S IS. <<<
 * "Sharur Tablet.smx" was authored IN PLACE in the room's Blender scene, so its
 * vertices are world coordinates and it is drawn with the view alone - no model
 * matrix (src/gula_tablet.h has the argument). sharur_tablet_load_assets()
 * MEASURES the slab from those vertices: its front normal, its long axis, its
 * centre and its three extents. A re-export that moves, resizes or turns it
 * moves the box, the sign, the shot and the carving under it with it.
 *
 * >>> THE BOX IS ORIENTED, NOT AXIS-ALIGNED. <<< The slab stands at ~42
 * degrees, so the Gula Tablet's min/max box would be 880 x 950 of solid air
 * around a 100-thick stone. Collision, reach and the sign all work in the
 * slab's own frame (along it, out of its face) and come back to world space.
 *
 * THE FRONT is the face with the inscription, the one facing -X/-Z, out over
 * the room's south-west path. It is found at load as the faces whose outward
 * normal is within 45 degrees of that direction; those are also the faces that
 * take the CRACKED art.
 *
 * WHAT THE PLAYER DOES:
 *   - EXAMINE. In front of the face, a "Press O to examine" sign. Circle flies
 *     the camera to a face-on shot of the whole slab and the inscription fades
 *     in under it a letter at a time:
 *         AND WITH ALL THE MIGHT OF THE GODS HE STRUCK
 *         ONE TIME ON THE WING, ONE TIME ON THE REAR,
 *         AND THREE BLOWS TO THE HEAD
 *     (the user's second line wrapped in two: at the 5x7 font's advance it is
 *     394 px, the screen is 320.) Nothing works but Cross, which puts the
 *     camera back and returns control. In main.c's CUTSCENE list, so no HUD
 *     and no log box over the shot.
 *   - STRIKE IT FIVE TIMES with a melee weapon - the Crucifaxe today
 *     (src/crucifaxe.c calls sharur_tablet_try_hit). Blows 1-3 play AXEHIT;
 *     the 4th plays RUMBLE and the front faces switch to the cracked art; the
 *     5th plays RUMBLE again (on SFX_RUMBLE_4's voice, so it does not cut the
 *     4th off) and the slab SHATTERS into a handful of large grey cubes, in
 *     the texture's own greys. Ninurta's Arm is left where it stood.
 *
 * >>> THE HIT COUNT IS PER VISIT. <<< The user's call: leaving the room
 * (or loading) puts the slab back to whole and uncracked. Only the shatter is
 * permanent - FLAG_SHARUR_TABLET_BROKEN - and the carving's collection is
 * FLAG_NINURTAS_ARM_TAKEN (src/player.h has why it is not the item bit).
 *
 * TEXTURES: "sharur tablet" (\TEXUNDW\SHARTBLT.TIM, 4bpp x704 y0, CLUT
 * 976,502) and "sharur tablet cracked" (\TEXUNDW\SHARTBCR.TIM, 4bpp x768 y0,
 * CLUT 992,502) - two page starts tools/VRAM_MAP_UNDERWORLD.txt calls FREE.
 * Both uploaded on every entry by sharurs_trial_upload_textures(), so the swap
 * on the fourth blow is a tpage change, not a mid-room LoadImage.
 *
 * Gated to STATE_SHARURS_TRIAL inside, so every entry point may be called
 * unconditionally.
 * ----------------------------------------------------------------------- */

void sharur_tablet_load_assets(void);     /* startup: geometry, measure, registrations */
void sharur_tablet_upload_textures(void); /* room entry: pure LoadImage of both TIMs   */

/* Room entry (sharurs_trial_init): no shot, no shards, the hit count back to
   zero, and a Circle held through the door swallowed. */
void sharur_tablet_arm(void);

/* AFTER world_enter, savegame_apply_pending and the debug grants (main.c's
   re-derive block): lay Ninurta's Arm out again if the slab is broken, the arm
   was never collected and no live one came back with the room. Writes to
   item_pickups, which world_enter has only just restored. */
void sharur_tablet_apply_flags(void);

/* Per frame, from Sharur's Trial's block AND from the examine branch at the
   top of update_current_area: the Circle trigger (when idle and not `lock`),
   the shot and its Cross, and the shards' fall. */
void sharur_tablet_update(int lock);

int  sharur_tablet_examining(void);   /* 1 while the shot owns camera + input */

/* The Crucifaxe's hook. 1 if this swing struck the slab (it then counts the
   blow); 0 if it is broken, out of reach, behind the swing or being read. */
int  sharur_tablet_try_hit(void);

/* Player push-out against the oriented box, the box being the slab's OWN
   dimensions measured off the mesh. Pass SH_COLLIDE_RADIUS, NOT the 75 most
   props get: a 600-tall, 100-thick slab held 75 off the eye threw the face's
   corners past the GTE's +/-1023 screen guard, those polys were dropped, and
   the player saw through the stone. 195 (the wall standoff) keeps a whole
   1200x600 face on screen. Called BEFORE the walls (collision.c): the slab's
   back is ~170 off the rock band behind it, and the rocks must have the last
   word. tools/ADDING_A_PROP.txt, "THE STANDOFF". */
#define SH_COLLIDE_RADIUS 195
void sharur_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* World: the slab, its shards and the sign. View-space, so call from the
   room's draw with the camera live. */
void sharur_tablet_draw(RenderContext *ctx);
/* Screen: the inscription under the shot. Drawn whatever the debug
   experiment, since it is the content of a cutscene, not an entity. */
void sharur_tablet_draw_overlay(RenderContext *ctx);

#endif
