#ifndef CHAINLINK_DOOR_H
#define CHAINLINK_DOOR_H

#include <stdint.h>
#include "render.h"
#include "title.h"

/* Chainlink Door: a placeable solid prop, modelled on the concrete block/chair
   props. One SMD ("Chainlink Door.smx"), one texture (chnlnk), a world AABB for
   the player push-out and for hitscan.

   The model is 600 wide x 31 deep x 507 tall, with its origin centred in X and
   on its FRONT face in Z (local x[-300,300], z[0,31], y[-507,0] with y=0 the
   base). That is not the concrete props' symmetric convention, so the footprint
   is carried as explicit local offsets rather than half-extents — place it by
   the same coordinates the gap it fills has in the room mesh.

   It is area-tagged, so chainlink_doors_collide/point_solid can be called
   unconditionally from the shared collision routines. */
void chainlink_doors_load_assets(void);   /* startup: geometry + texture header */
void chainlink_doors_clear(void);         /* drop every placed instance */

/* Place one. x/z are the model origin in world space (so x is the panel's
   centre, z its front face); y is the standing reference, -GROUND_FLOOR_Y for a
   room whose floor is at world y=0. rot_y is 0..4096 = a full turn. */
void chainlink_door_place(GameState area, int32_t x, int32_t y, int32_t z,
                          int32_t rot_y);

/* Player push-out and hitscan volume; both no-op outside the placing area. */
void chainlink_doors_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
int  chainlink_doors_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack);

/* Draw every instance in the current area. Restores the caller's view matrix. */
void chainlink_doors_draw(RenderContext *ctx);

#endif
