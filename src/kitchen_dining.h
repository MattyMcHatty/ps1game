#ifndef KITCHEN_DINING_H
#define KITCHEN_DINING_H

#include "render.h"

void kitchen_load_textures(void);   /* call once at startup, not mid-game */
void kitchen_dining_init(void);
void kitchen_dining_draw(RenderContext *ctx);

#endif
