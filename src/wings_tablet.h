#ifndef WINGS_TABLET_H
#define WINGS_TABLET_H

#include <stdint.h>
#include "render.h"

/* -----------------------------------------------------------------------
 * THE WINGS TABLET - a stone slab standing on Wingscape's terrace, in the
 * alcove at the north rim: 1200 long (x), 100 thick (z) and 600 tall, its base
 * on the terrace at y=-500. Chapter 4's SECOND PUZZLE, built on Sharur's
 * Tablet's pattern (src/sharur_tablet.h): a sign, a fixed shot, and on the
 * right answer a shatter that leaves NINURTA'S WINGS where it stood.
 *
 * >>> ITS MESH IS ITS POSITION AND ITS COLLISION, AS THE GULA TABLET'S IS. <<<
 * "Wings Tablet.smx" was authored IN PLACE in Wingscape's Blender scene, so
 * its vertices are the room's world coordinates - x[-168,1032] y[-1100,-500]
 * z[2880,2980] as exported - and it is drawn with the view alone, no model
 * matrix. wings_tablet_load_assets() MEASURES the vertex min/max on all three
 * axes and that box is its whole solid volume, so a re-export that moves or
 * resizes the slab moves its collision, its sign and its shot with it. It is
 * axis-aligned (its long side runs along X), so the Gula Tablet's min/max box
 * is exact - Sharur's oriented box is not needed.
 *
 * THE FRONT is the z=min face (2880), looking south over the terrace; the
 * reader faces +Z, which is yaw 0.
 *
 * WHAT THE PLAYER DOES (the user's brief, Nov 2026):
 *   - In front of the face, a "Press O to interact" sign. Circle flies the
 *     camera to a face-on shot of the whole slab (Sharur's terms).
 *   - The QUESTION, 'HAS WING', fades in at the very centre of the screen;
 *     then the four ANSWERS - BIRD, DOG, CAT, SNAKE - fade in along the
 *     bottom, evenly spaced, in a random order RE-ROLLED ON EVERY ROOM ENTRY
 *     (wings_tablet_arm). All five are CUNEIFORM only - lines of "wings tablet
 *     question and answers.png", no English (the user's answer).
 *   - Then the reticule - the double blue outline every board uses
 *     (src/exit_door_puzzle.c, xd_cursor). Left/Right move it, Circle
 *     answers. Cross leaves at ANY time, the camera flight included.
 *   - BIRD: the camera comes back and the slab SHATTERS into grey cubes as
 *     Sharur's does (SFX_RUMBLE_4), leaving NINURTA'S WINGS.
 *   - ANYTHING ELSE: the log says "YOU ARE NOT WORTHY" and the puzzle closes
 *     at once. The player is NOT put back where they stood: they land on the
 *     terrace's cliff edge behind them (WT_KICK_Z, on the slab's centre line)
 *     facing the tablet, and the GUARD - a Golem - appears in front of the
 *     tablet (WT_GUARD_*), already awake and attacking (SFX_RUMBLE). Every
 *     wrong answer does this.
 *
 * >>> THE GUARD IS ONE SEEDED GOLEM, SUMMONED AGAIN AND AGAIN. <<< world.c
 * plants it ABSENT (src/golem.h, SUMMONED GOLEMS), so it costs one slot of
 * the whole-game budget and one save bit however many wrong answers there
 * are. It is PERMANENT like the west door's Golem (the user's answer): left
 * alive it is back, idle and full-healthed, on the next entry, and
 * FLAG_WINGS_GUARD_UP carries it across a load (player.h has why it is a flag
 * and not its dead bit).
 *
 * >>> WHILE THE GUARD STANDS, THE TABLET IS SILENT. <<< No sign and no
 * Circle until it is dead (the user's answer) - so only one is ever summoned
 * at a time, and it can never stand in the shot between the camera and the
 * slab.
 *
 * PERSISTENCE: the right answer is FLAG_WINGS_TABLET_BROKEN, the collection
 * FLAG_NINURTAS_WINGS_TAKEN - Sharur's pair. The wings are a run-time spawn
 * re-laid by wings_tablet_apply_flags() on entry.
 *
 * TEXTURES:
 *   "wing tablet"  \TEXUNDW\WINGTBLT.TIM, 4bpp 128x128 at x704 y0, CLUT
 *       (976,502) - SHARUR'S TABLET's page AND palette, time-shared.
 *   "wings tablet question and answers"  \TEXUNDW\WINGTBQA.TIM, 4bpp 128x128
 *       with STP (it fades in ADDITIVELY) at x768 y0, CLUT (992,502) - SHARUR'S
 *       CRACKED TABLET's page and palette, time-shared the same way.
 *   Both uploaded on every entry by wingscape_upload_textures(). Voff 0, so
 *   the room's 128 texture window serves the slab and the overlay alike.
 *
 * Gated to STATE_WINGSCAPE inside, so every entry point may be called
 * unconditionally.
 * ----------------------------------------------------------------------- */

/* THE GUARD'S SPOT: on the slab's centre line (x 432), 400 in front of its
   face (z 2880) on the terrace (y -500). The alcove's walls are 1000+ to
   either side; the Golem's plug (425) holds a player off the slab's middle
   but leaves both ends of its face in reach - which is moot, as the tablet is
   silent while it stands. Authored rather than measured because world.c
   seeds it before any mesh is read; re-derive if the slab moves. */
#define WT_GUARD_X         432
#define WT_GUARD_Z        2480
#define WT_GUARD_FLOOR_Y  (-500)

void wings_tablet_load_assets(void);    /* startup: geometry, measure, registrations */
void wings_tablet_upload_texture(void); /* room entry: pure LoadImage, both TIMs     */

/* Room entry (wingscape_init): no shot, no shards, a new answer order, and a
   Circle held through the door swallowed. */
void wings_tablet_arm(void);

/* AFTER world_enter, savegame_apply_pending and the debug grants (main.c's
   re-derive block): lay Ninurta's Wings out again if the slab is broken and
   they were never collected, and bring the guard back if FLAG_WINGS_GUARD_UP
   says it is standing. Writes to item_pickups and golems, which world_enter
   has only just restored. */
void wings_tablet_apply_flags(void);

/* Per frame, from Wingscape's block AND from the puzzle branch at the top of
   update_current_area: the guard's flag, the Circle trigger (when idle and
   not `lock`), the shot and the board, and the shards' fall. */
void wings_tablet_update(int lock);

int  wings_tablet_examining(void);   /* 1 while the puzzle owns camera + input */

/* Player push-out against the measured box. Pass WT_COLLIDE_RADIUS, NOT the
   75 most props get: the face is 1200 wide and 600 tall, so its corners stand
   up to 600 off the eye, and tools/ADDING_A_PROP.txt's rule (standoff >= S/4)
   asks for 150; the wall standoff, as Sharur's Tablet takes for a slab of the
   same size, keeps the whole face inside the GTE's screen guard. Called
   BEFORE the walls (collision.c): the back stands 120 off the alcove's rim,
   so its push reaches past the rock, and the rock must have the last word.
   Nothing once the slab is broken. */
#define WT_COLLIDE_RADIUS 195
void wings_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* World: the slab, its shards and the sign. View-space, so call from the
   room's draw. */
void wings_tablet_draw(RenderContext *ctx);
/* Screen: the question, the answers and the reticule over the shot. Drawn
   whatever the debug experiment - it is the puzzle, not an entity. */
void wings_tablet_draw_overlay(RenderContext *ctx);

#endif
