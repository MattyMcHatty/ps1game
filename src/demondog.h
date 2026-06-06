#ifndef DEMONDOG_H
#define DEMONDOG_H

#include <stdint.h>
#include "render.h"

#define MAX_DEMON_DOGS        8
#define DDOG_MAX_HEALTH       3
#define DDOG_SPEED           10
#define DDOG_WAKE_RADIUS      2400
#define DDOG_CATCH_DIST       180
#define DDOG_DAMAGE_AMOUNT    10
#define DDOG_DAMAGE_COOLDOWN  60
#define DDOG_HALF_W           60
#define DDOG_HALF_H           50
#define DDOG_Y_OFFSET        100
#define DDOG_KNOCKBACK        40
#define DDOG_BAR_TIMER_MAX    120

#define DDOG_SHADOW_W          70
#define DDOG_SHADOW_D          30
#define DDOG_SHADOW_R           0
#define DDOG_SHADOW_G           0
#define DDOG_SHADOW_B           0

typedef enum {
    DDOG_DORMANT,
    DDOG_ALERT,
    DDOG_DEAD,
} DDogState;

typedef struct {
    int32_t   x, y, z;
    int32_t   vy;
    int32_t   kb_vx, kb_vz;
    int       health;
    int       damage_timer;
    int       hit_timer;
    DDogState state;
    int32_t   active;
    int       on_upper_floor;
    int       on_ramp;
    int       anim_tick;
} DemonDog;

extern DemonDog demon_dogs[MAX_DEMON_DOGS];
extern int      demon_dog_count;

void demon_dogs_init(void);
void demon_dogs_reset(void);
void update_demon_dogs(void);
void draw_demon_dogs(RenderContext *ctx);

#endif
