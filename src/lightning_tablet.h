#ifndef LIGHTNING_TABLET_H
#define LIGHTNING_TABLET_H

#include <stdint.h>
#include "render.h"

/* -----------------------------------------------------------------------
 * THE LIGHTNING TABLET - a stone slab standing against Lightnings Edge's
 * north wall, 1200 long, 700 tall and 100 thick, x[-600,600] y[-700,0]
 * z[2450,2550]. Chapter 4's third tablet puzzle and the room's only prop.
 *
 * >>> ITS MESH IS ITS POSITION AND ITS COLLISION, AS THE WINGS TABLET'S IS. <<<
 * "Lightning Tablet.smx" was exported IN PLACE, so its vertices are world
 * coordinates and it is drawn with the view alone. load_geometry() measures the
 * axis-aligned box off them, and the squares' grid off the box. It is read
 * on every entry into the ROOM ARENA's tail (room_arena_load_extra), not held
 * on the heap: only this room ever draws it.
 *
 * THE FACE (z=2450, facing -Z; the reader looks +Z, yaw 0): a frieze of clouds along the top (y -700..-600),
 * Ninurta on the right (x 200..600) holding a lightning bolt, and a 4 x 3
 * grid of 200 x 200 SQUARES (x -600..200, y -600..0) carrying yellow and blue
 * lightning. Each square is a fan of four triangles round its centre on the
 * front (one quad on the back), so a square TURNS by moving its corner UVs
 * round a quarter - the Anzu Tablet's quarter-turn table, in the mesh.
 *
 * WHAT THE PLAYER DOES:
 *   - Every room entry deals each square a random quarter turn, never the
 *     correct one (and never upside-down for the one square that would
 *     still look joined that way - see FLIP below).
 *   - In front of the face, a "Press O to interact" sign. Circle flies the
 *     camera to a face-on shot. A cursor picks any of the twelve squares
 *     (d-pad), SQUARE turns the one under it 90 degrees clockwise (the Anzu
 *     Tablet's button), Cross
 *     leaves at any time. The turns stay as left for the rest of the visit.
 *   - Only the EIGHT squares with YELLOW lightning on them have to be
 *     upright: the run from the cloud to the bolt in Ninurta's hand. The four
 *     blue-only squares never matter.
 *   - SOLVED: a short scene in the same shot (a cutscene: no HUD, no log
 *     box, no Cross). The yellow lights up as a fuse from the cloud to the
 *     bolt - the Seal's lines (src/seal.c): an additive halo and core over
 *     the carving and a spark riding the front - then Ninurta glows, then
 *     the slab SHATTERS as the Wings Tablet's does and leaves the LIGHTNING
 *     BOLT where it stood. Control returns.
 *
 * FLIP: the one ambiguity in the art, measured off the PNG. Row 1 col 1's
 * yellow enters at its top edge and leaves at its bottom edge at two points
 * a half turn swaps exactly, so upside down it LOOKS connected. It is
 * accepted at 0 or 180 (snapped upright as the scene starts), and the deal
 * gives it only 90 or 270, so it never starts looking right.
 *
 * Permanent: FLAG_LIGHTNING_TABLET_BROKEN (set as it shatters) and
 * FLAG_LIGHTNING_BOLT_TAKEN (src/player.h).
 *
 * TEXTURES, the face split at x=200 by the export, both 8bpp 128x128:
 *   "lightning tablet puzzle"  (\TEXUNDW\LTNGPZL.TIM)  x704 y0, CLUT (272,484)
 *       the grid, the cloud frieze over it, the ends, the back behind them
 *   "lightning tablet ninurta" (\TEXUNDW\LTNGNNRT.TIM) x768 y0, CLUT (272,486)
 *       Ninurta's panel, its frieze, the back behind it
 * Two of the tablets' pages (Sharur's/the Wings Tablet's, and the cracked
 * tablet's/the Wings cuneiform's/Destiny's Door's), whose own rooms re-upload
 * their art on entry; Lightnings Edge draws none of it. Each face's texture
 * is read off the palette smxlink baked into its prim.
 *
 * Gated to STATE_LIGHTNINGS_EDGE inside, so every entry point may be called
 * unconditionally.
 * ----------------------------------------------------------------------- */

void lightning_tablet_load_assets(void);   /* startup: the texture's registration */
/* ROOM ENTRY (lightnings_edge_load_geometry, after its room_data_bind): the
   mesh into the room arena's tail - room-scoped, no heap - rebased, measured
   into its box and squares. The next room's load overwrites it. */
void lightning_tablet_load_geometry(void);
void lightning_tablet_upload_texture(void);/* room entry: pure LoadImage           */

/* Room entry (lightnings_edge_init): no shot, no shards, a fresh deal of the
   twelve squares, and a Circle held through the door swallowed. */
void lightning_tablet_arm(void);

/* AFTER world_enter, savegame_apply_pending and the debug grants (main.c's
   re-derive block): lay the Lightning Bolt out again if the slab is broken
   and the bolt was never collected. */
void lightning_tablet_apply_flags(void);

/* Per frame, from Lightnings Edge's block AND from the examine branch at the
   top of update_current_area: the Circle trigger (when idle and not `lock`),
   the shot, the board, the scene, and the shards' fall. */
void lightning_tablet_update(int lock);

int  lightning_tablet_examining(void);   /* 1 while the shot owns camera + input */

/* Player push-out against the measured box: the Wings Tablet's slab, so its
   standoff - a 1200 x 700 face held 75 off the eye throws its corners past
   the GTE's screen guard (tools/ADDING_A_PROP.txt, "THE STANDOFF"). Called
   BEFORE the walls (collision.c): its back stands 49 off the north wall. */
#define LT_COLLIDE_RADIUS 195
void lightning_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* World: the slab, its lit lines, its shards and the sign. Reloads the plain
   view itself. */
void lightning_tablet_draw(RenderContext *ctx);
/* Screen: the cursor and the controls over the shot. Drawn whatever the debug
   experiment: it is the puzzle. */
void lightning_tablet_draw_overlay(RenderContext *ctx);

#endif
