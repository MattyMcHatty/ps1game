#ifndef KITCHEN_DINING_H
#define KITCHEN_DINING_H

#include "render.h"

void kitchen_load_assets(void);     /* load textures + geometry once at startup */
void kitchen_dining_init(void);
void kitchen_dining_draw(RenderContext *ctx);
void kitchen_door_arm(void);        /* seed Circle edge state on entering the kitchen */
int  kitchen_door_triggered(void);  /* 1 when Circle pressed within range of the door */

#endif
