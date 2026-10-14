#ifndef CRUCIFAXE_H
#define CRUCIFAXE_H

#include "render.h"

#define SWING_DURATION  7
/* The return IS the cooldown: the axe is raised slowly over what used to be a
   14-frame return plus a 21-frame rest, so the next press is taken the frame it
   is upright again. Press-to-press is unchanged at 41 frames. The hit window
   and rabisu.c's parry windows read only the first frames of swing_timer, so
   lengthening the tail moves neither. */
#define SWING_RETURN    34
#define SWING_TOTAL     (SWING_DURATION + SWING_RETURN)
#define SWING_RANGE     350
#define KNOCKBACK_SPEED 80

extern int swing_timer;

void crucifaxe_init(void);
void update_crucifaxe(void);
void draw_crucifaxe(RenderContext *ctx);

#endif
