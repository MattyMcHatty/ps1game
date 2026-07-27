#ifndef STAIR_ANIM_H
#define STAIR_ANIM_H

#include "render.h"

/* Stair-climb transition between the conservatory and the 2F hall, replacing the
   door open/close animation for those two stair triggers. Shows the upstairs
   texture on a screen quad and lurches the "camera" up (or down) + forward three
   times, ~1s per step, with alternating footstep sounds, then fades to black and
   hands off to STATE_LOADING. The upstairs texture is resident in VRAM in both
   the conservatory and the hall, so it is available throughout the transition. */
#define STAIR_UP    0   /* conservatory -> 2F hall (climb up)   */
#define STAIR_DOWN  1   /* 2F hall -> conservatory (climb down) */

void stair_anim_load_assets(void);   /* startup: capture the upstairs tpage/clut */
void stair_anim_start(int direction);
void stair_anim_update(void);
int  stair_anim_finished(void);
void stair_anim_draw(RenderContext *ctx);

#endif
