#ifndef DESTINY_TABLET_H
#define DESTINY_TABLET_H

#include <stdint.h>
#include "render.h"

/* -----------------------------------------------------------------------
 * THE DESTINY TABLET - a stone slab standing inside Destinys Cabin against
 * its north wall, 1200 long, 700 tall and 100 thick, x[2100,3300] y[-700,0]
 * z[4750,4850]. Chapter 4's fourth tablet puzzle: the Lightning Tablet's size
 * and module (src/lightning_tablet.c), with a WORD BOARD for a puzzle.
 *
 * >>> ITS MESH IS ITS POSITION AND ITS COLLISION, AS THE LIGHTNING TABLET'S
 * IS. <<< "Destiny Tablet.smx" was exported IN PLACE, so its vertices are
 * world coordinates and it is drawn with the view alone. load_geometry()
 * measures the axis-aligned box off them. It is read on every entry into the
 * ROOM ARENA's tail (room_arena_load_extra), not held on the heap: only this
 * room ever draws it.
 *
 * THE FACE (z=4750, facing -Z; the reader looks +Z, yaw 0): Anzu on the
 * left, four lines of cuneiform in the middle, Ninurta on the right. The back
 * carries the same art.
 *
 * WHAT THE PLAYER DOES:
 *   - In front of the face, a "Press O to interact" sign. Circle flies the
 *     camera to a face-on shot and lays the board over it, in screen space:
 *
 *         ADAM                          LIGHTNING      side words 0-3
 *         ...                                 ...
 *                     ______ IS                        the four lines; the
 *           MASTER OF ______                           blanks line up in one
 *                     ______ DEFIED & STOLE            column (the user's
 *         RETURNED BY ______                           layout)
 *         NINURTA                         NEPTUNE      side words 4-7
 *         ...                                 ...
 *
 *     The columns are split round the four lines because the lines and two
 *     full-height columns do not fit across 320 pixels side by side (the
 *     user's call, Nov 2026).
 *   - The d-pad walks a cursor over the sixteen side words. CIRCLE puts the
 *     word under it in the next blank, top first; a placed word greys out and
 *     the cursor skips it (each word once - the user's call). CROSS takes the
 *     last placed word back; CROSS with no word placed leaves the board.
 *   - Four words in and wrong: the log says "That doesn't make sense" and
 *     the board stays up.
 *   - ENLIL, FATE, ANZU, NINURTA: SOLVED. A short scene in the same shot (a
 *     cutscene: no HUD, no log box, no Cross): the side columns fade, the four
 *     words glow slowly in turn, the lines fade, and the slab SHATTERS as the
 *     Lightning Tablet's does, leaving the FAKE TABLET OF DESTINIES where it
 *     stood. Control returns.
 *
 * >>> THE PLAYER NEVER LEARNS THE TABLET IS FAKE. <<< Its log line, menu name
 * and description all say "Tablet of Destinies" (src/item_pickup.c,
 * src/menu.c). It is its own item, ITEM_FAKE_TABLET_OF_DESTINIES, with its
 * own art - not the Chamber's ITEM_TABLET_OF_DESTINIES.
 *
 * Permanent: FLAG_DESTINY_TABLET_BROKEN (set as it shatters) and
 * FLAG_FAKE_TABLET_TAKEN (src/player.h).
 *
 * TEXTURE: "destiny tablet" (\TEXUNDW\DSTTBLT.TIM), 8bpp 128x128 at x704 y0
 * (the tablets' page; Destinys Cabin draws none of its other tenants), CLUT
 * (272,488) of its own.
 *
 * Gated to STATE_DESTINYS_CABIN inside, so every entry point may be called
 * unconditionally.
 * ----------------------------------------------------------------------- */

void destiny_tablet_load_assets(void);    /* startup: the texture's registration */
/* ROOM ENTRY (destinys_cabin_load_geometry, after its room_data_bind): the
   mesh into the room arena's tail - room-scoped, no heap - measured into its
   box. The next room's load overwrites it. */
void destiny_tablet_load_geometry(void);
void destiny_tablet_upload_texture(void); /* room entry: inside STATE_LOADING */

/* Room entry (destinys_cabin_init): no shot, no shards, an empty board, and a
   Circle held through the door swallowed. */
void destiny_tablet_arm(void);

/* AFTER world_enter, savegame_apply_pending and the debug grants (main.c's
   re-derive block): lay the Fake Tablet of Destinies out again if the slab is
   broken and it was never collected. */
void destiny_tablet_apply_flags(void);

/* Per frame, from Destinys Cabin's block AND from the examine branch at the
   top of update_current_area: the Circle trigger (when idle and not `lock`),
   the shot, the board, the scene, and the shards' fall. */
void destiny_tablet_update(int lock);

int  destiny_tablet_examining(void);  /* 1 while the shot owns camera + input */
int  destiny_tablet_scene(void);      /* 1 during the solve's scene (a cutscene:
                                         main.c hides the log box for it, and
                                         keeps it up over the board) */

/* Player push-out against the measured box: the Lightning Tablet's slab, so
   its standoff - a 1200 x 700 face held 75 off the eye throws its corners
   past the GTE's screen guard (tools/ADDING_A_PROP.txt, "THE STANDOFF").
   Called BEFORE the walls (collision.c): its back stands 150 off the cabin's
   inner north wall. */
#define DT_COLLIDE_RADIUS 195
void destiny_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* World: the slab, its shards and the sign. Reloads the plain view itself. */
void destiny_tablet_draw(RenderContext *ctx);
/* Screen: the board over the shot. Drawn whatever the debug experiment: it is
   the puzzle. */
void destiny_tablet_draw_overlay(RenderContext *ctx);

#endif
