#ifndef WINGS_TABLET_H
#define WINGS_TABLET_H

#include <stdint.h>
#include "render.h"

/* -----------------------------------------------------------------------
 * THE WINGS TABLET - a stone slab standing on Wingscape's terrace, in the
 * alcove at the north rim: 1200 long (x), 100 thick (z) and 600 tall, its base
 * on the terrace at y=-500. Static furniture: no sign, no interaction.
 *
 * >>> ITS MESH IS ITS POSITION AND ITS COLLISION, AS THE GULA TABLET'S IS. <<<
 * "Wings Tablet.smx" was authored IN PLACE in Wingscape's Blender scene, so
 * its vertices are the room's world coordinates - x[-168,1032] y[-1100,-500]
 * z[2880,2980] as exported - and it is drawn with the view alone, no model
 * matrix. wings_tablet_load_assets() MEASURES the vertex min/max on all three
 * axes and that box is its whole solid volume, so a re-export that moves or
 * resizes the slab moves its collision with it. It is axis-aligned (its long
 * side runs along X), so the Gula Tablet's min/max box is exact - Sharur's
 * oriented box is not needed.
 *
 * TEXTURE: "wing tablet", \TEXUNDW\WINGTBLT.TIM, 4bpp 128x128 at x704 y0 with
 * CLUT (976,502) - SHARUR'S TABLET's page AND palette, time-shared: Wingscape
 * never draws Sharur's Tablet and Sharur's Trial never draws this one, and
 * each room uploads its own tablet on entry (tools/vram_map.py
 * KNOWN_STREAM_PAIRS). Voff 0, so the room's 128 texture window serves it.
 *
 * Gated to STATE_WINGSCAPE inside, so every entry point may be called
 * unconditionally.
 * ----------------------------------------------------------------------- */

void wings_tablet_load_assets(void);    /* startup: geometry, measure, registration */
void wings_tablet_upload_texture(void); /* room entry: pure LoadImage, no CD         */

/* Player push-out against the measured box. Pass WT_COLLIDE_RADIUS, NOT the
   75 most props get: the face is 1200 wide and 600 tall, so its corners stand
   up to 600 off the eye, and tools/ADDING_A_PROP.txt's rule (standoff >= S/4)
   asks for 150; the wall standoff, as Sharur's Tablet takes for a slab of the
   same size, keeps the whole face inside the GTE's screen guard. Called
   BEFORE the walls (collision.c): the back stands 120 off the alcove's rim,
   so its push reaches past the rock, and the rock must have the last word. */
#define WT_COLLIDE_RADIUS 195
void wings_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* World: the slab. View-space, so call from the room's draw. */
void wings_tablet_draw(RenderContext *ctx);

#endif
