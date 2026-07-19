#ifndef DELIVERY_AREA_H
#define DELIVERY_AREA_H

#include "render.h"

void delivery_area_init(void);
void delivery_area_draw(RenderContext *ctx);
void delivery_restore_textures(void);   /* re-upload slots the conservatory streams over */

#endif
