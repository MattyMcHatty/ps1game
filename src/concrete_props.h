#ifndef CONCRETE_PROPS_H
#define CONCRETE_PROPS_H

#include <stdint.h>
#include "render.h"

/* Simple static concrete props (block + chair) sharing one texture (cncrte).
   Modelled on the dining table / piano props: indestructible, solid to the
   player and the gun's line of sight. Currently placed in the conservatory;
   the collide/point_solid tests gate themselves to that area (the collision
   routine is shared with reception/piano room). */

#define CONCRETE_BLOCK  0
#define CONCRETE_CHAIR  1

void concrete_props_load_assets(void);     /* startup: geometry + texture register */
void concrete_props_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void concrete_props_place(void);           /* conservatory_init: position the props */
void concrete_props_draw(RenderContext *ctx);
void concrete_props_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
int  concrete_props_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack);

#endif
