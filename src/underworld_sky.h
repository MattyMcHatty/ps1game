#ifndef UNDERWORLD_SKY_H
#define UNDERWORLD_SKY_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   THE UNDERWORLD SKYBOX - drawn in every Chapter 4 room but the Descent
   =========================================================================
   One model, "Underworld Skybox.smx": an open ring of sixteen inward faces,
   radius 20000 and 12000 tall, centred on its origin, all textured with the
   Descent's hell bg. No top and no bottom: above and below it the room's
   clear colour shows. Each room draws it CENTRED ON ITS OWN GEOMETRY by
   passing that centre to underworld_sky_draw(), so the room sits in the
   middle of it.

   NOT FOGGED, NOT CULLED, ALWAYS DRAWN, AND BEHIND EVERYTHING. Every face is
   queued at the very back of the ordering table, so nothing the room draws can
   be hidden by it, and its colour is the texture's own whatever the distance.
   It has no collision; the player never comes near it.

   CUT UP AT DRAW TIME, NOT IN THE MODEL. With the camera inside the ring, the
   faces beside and behind it have corners BEHIND the camera, which the GTE
   cannot project, so a face drawn as one quad flickers out whenever any of
   its corners does. src/descent_to_hell.c found this with its own box. So
   each face is cut into a UW_SKY_SUB x UW_SKY_SUB grid when it is drawn, and
   only the cells whose four corners all project are queued. At the sky's
   distance a cell that fails is off screen by construction. Re-exporting the
   model needs nothing here.

   TEXTURE: hell bg (4bpp x640 y0), the Descent's, through its narrow
   uploader. Voff 0 on a page start, so it draws under the room's own 128
   texture window. TEXBANK_UNDERWORLD.

   SIZE CEILING: each corner is moved to the room's centre and projected as a
   16-bit world position, so radius + the room's centre (and the camera's
   distance from it) must stay under 32767. At radius 20000 there is ~8000 to
   spare; a ring much past ~28000 wraps round and tears.

   LIFETIME: the model is ~1.2 KB read on entry to a sky room and handed
   back on the way into any other (main.c's load_area_geometry). */

void underworld_sky_load_assets(void);       /* startup: the texture's slot    */
void underworld_sky_upload_textures(void);   /* room entry: pure LoadImage     */
void underworld_sky_load(void);              /* room entry: the model (CD)     */
void underworld_sky_unload(void);            /* every transition               */

/* Queue the sky, centred on world (cx, cy, cz). Call from the room's draw
   AFTER the view matrix is loaded into the GTE; it leaves the view loaded. */
void underworld_sky_draw(RenderContext *ctx, int32_t cx, int32_t cy, int32_t cz);

/* The OT slot the sky uses. A room's own mesh must clamp its far end to the
   slot in front of it, or a far poly could share the sky's slot and be drawn
   under it. */
#define UW_SKY_OT  (OT_LENGTH - 2)

#endif
