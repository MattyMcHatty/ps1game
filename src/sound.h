#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

typedef enum {
    SFX_SWING   = 0,
    SFX_HURT    = 1,
    SFX_PICKUP  = 2,
    SFX_SMASH   = 3,
    SFX_DOGBARK = 4,
    SFX_AXEHIT  = 5,   /* crucifaxe connects with an enemy (non-fatal hit) */
    SFX_DOGDIE  = 6,
    SFX_UNLOCK  = 7,
    SFX_DOOR     = 8,   /* double-door open/close, used by the level transition */
    SFX_ZOMBIE   = 9,   /* zombie groan, looped while a zombie is alert */
    SFX_ZOMBIEDIE = 10, /* zombie death */
    SFX_DIE       = 11, /* player death */
    SFX_GR_SHOT   = 12, /* grave-olver gunshot */
    SFX_GR_RELOAD = 13, /* grave-olver reload */
    SFX_COUNT    = 14,
} SfxID;

void sound_init(void);
void sound_play(SfxID id);
void sound_stop(SfxID id);

#endif
