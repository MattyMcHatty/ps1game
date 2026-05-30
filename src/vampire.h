#ifndef VAMPIRE_H
#define VAMPIRE_H

#include <stdint.h>
#include "render.h"

#define VAMPIRE_SPEED    4
#define VAMPIRE_HALF_W   50
#define VAMPIRE_HALF_H   100
#define VAMPIRE_Y        50
#define CATCH_DIST       200
#define DAMAGE_INTERVAL  2
#define VAMPIRE_MAX_HEALTH   5
#define VAMPIRE_BAR_TIMER_MAX 120

extern int32_t vampire_x;
extern int32_t vampire_y;
extern int32_t vampire_vy;
extern int32_t vampire_z;
extern int     vampire_health;
extern int32_t vampire_kb_vx;
extern int32_t vampire_kb_vz;
extern int     vampire_hit_timer;

void update_vampire(void);
void draw_vampire(RenderContext *ctx);

#endif
