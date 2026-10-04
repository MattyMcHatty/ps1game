#ifndef CRUCIFAXE_H
#define CRUCIFAXE_H

#include "render.h"

#define SWING_DURATION  7
#define SWING_RETURN    14
#define SWING_TOTAL     21
/* Rest frames after the swing+return before the next press is taken. Equal to
   SWING_TOTAL, so press-to-press is twice the animation. A separate count, not
   a longer swing_timer: the hit window and rabisu.c's parry windows read
   swing_timer and must not move. */
#define SWING_COOLDOWN  SWING_TOTAL
#define SWING_RANGE     350
#define KNOCKBACK_SPEED 80

extern int swing_timer;

void crucifaxe_init(void);
void update_crucifaxe(void);
void draw_crucifaxe(RenderContext *ctx);

#endif
