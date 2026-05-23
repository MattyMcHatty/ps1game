#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include "render.h"

#define MAX_HEALTH 100

extern int32_t player_health;
extern int     game_over;
extern int     flash_timer;
extern int     damage_timer;

void draw_hud(RenderContext *ctx);

#endif
