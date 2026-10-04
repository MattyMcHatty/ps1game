#ifndef GULA_TABLET_H
#define GULA_TABLET_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — the instance is tagged with its area */

/* -----------------------------------------------------------------------
 * The Gula Tablet — a stone slab, 600 wide, 800 tall and 50 deep, standing
 * against the Nursery's west wall across the sealed west door. Chapter 3
 * furniture on the crib's terms: a deferred texture registration, a small SMD
 * read once in main() and held for the run, and a narrow
 * gula_tablet_upload_texture() for the room that draws it.
 *
 * >>> ITS MESH IS ITS COLLISION DATA AND ITS POSITION. <<< Gula Tablet.smx was
 * authored IN PLACE in the Nursery's Blender scene, so its vertices are the
 * Nursery's world coordinates — x[-1292,-1242] y[-800,0] z[-300,300] — and
 * that IS the placement: the brief was "the points that can be derived from
 * its export file". Unlike src/bars.c, which re-centres an in-place export so
 * the model can stand in other rooms, this one is drawn exactly where it was
 * modelled: no model matrix, the view alone (the Incinerator's arrangement,
 * src/incinerator.h). gula_tablet_load_assets() measures the vertex min/max on
 * all three axes and that box is the whole of its solid volume, so a
 * re-export that moves or resizes the slab moves its collision with it.
 *
 * Should it ever have to stand somewhere it was not modelled, the way is
 * bars_load_assets()'s re-centre plus an (x, z, rot_y) placement — not a
 * second export.
 *
 * TEXTURE: "gula tablet", \TEXCTCMB\GULATBLT.TIM, 4bpp 128x128 at x640 y0 —
 * the Room of Arms' page, the gaol door's terms — with a palette of its own at
 * (592,502). The Nursery's six floor tiles draw the same texture through
 * TIM_SLOT(GULATBLT), and the Nursery's uploader calls the one below. Voff 0,
 * so the room's 128 texture window serves it.
 *
 * Area-tagged, so gula_tablets_collide() and gula_tablets_draw() can be called
 * unconditionally and are a no-op in every other room.
 * ----------------------------------------------------------------------- */

void gula_tablet_load_assets(void);     /* startup: geometry + deferred registration */
void gula_tablet_upload_texture(void);  /* room entry: pure LoadImage, no CD           */

void gula_tablets_clear(void);

/* Stand the tablet in `area`, where its export puts it. One instance; a second
   call moves the tag. */
void gula_tablet_place(GameState area);

void gula_tablets_draw(RenderContext *ctx);
void gula_tablets_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

#endif
