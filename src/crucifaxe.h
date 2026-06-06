#ifndef CRUCIFAXE_H
#define CRUCIFAXE_H

#include "render.h"

#define SWING_DURATION  7
#define SWING_RETURN    14
#define SWING_TOTAL     21
#define SWING_RANGE     350
#define KNOCKBACK_SPEED 80

extern int swing_timer;

void crucifaxe_init(void);
void update_crucifaxe(void);
void draw_crucifaxe(RenderContext *ctx);

#endif
