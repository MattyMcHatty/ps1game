#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include "render.h"

#define MAX_HEALTH 100

extern int32_t player_health;
extern int     game_over;
extern int     flash_timer;
extern int     damage_timer;
extern int     player_keys;    /* bitmask — bit N set means KeyType N is held */

#define PICKUP_MSG_DURATION 120  /* frames (~2 seconds at 60fps) */
#define PICKUP_MSG_COUNT    3

typedef struct {
    char msg[64];
    int  timer;
} PickupEntry;

extern PickupEntry pickup_log[PICKUP_MSG_COUNT];

void show_pickup_msg(const char *item_name);
void draw_hud(RenderContext *ctx);

#endif
