#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

typedef enum {
    SFX_SWING   = 0,
    SFX_HURT    = 1,
    SFX_PICKUP  = 2,
    SFX_SMASH   = 3,
    SFX_DOGBARK = 4,
    SFX_DOGHURT = 5,
    SFX_DOGDIE  = 6,
    SFX_UNLOCK  = 7,
    SFX_DOOR    = 8,   /* double-door open/close, used by the level transition */
    SFX_COUNT   = 9,
} SfxID;

void sound_init(void);
void sound_play(SfxID id);

#endif
