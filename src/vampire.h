#ifndef VAMPIRE_H
#define VAMPIRE_H

#include <stdint.h>
#include "render.h"

#define VAMPIRE_SPEED    4
#define VAMPIRE_HALF_W   100
#define VAMPIRE_HALF_H   200
#define VAMPIRE_Y        100
#define CATCH_DIST       200
#define DAMAGE_INTERVAL  2
#define VAMPIRE_MAX_HEALTH 5

extern int32_t vampire_x;
extern int32_t vampire_z;
extern int     vampire_health;
extern int32_t vampire_kb_vx;
extern int32_t vampire_kb_vz;

void update_vampire(void);
void draw_vampire(RenderContext *ctx);

#endif
