#ifndef BULLET_HIT_H
#define BULLET_HIT_H

#include <stdint.h>
#include "render.h"

/*
 * Bullet-hit sprites: a brief textured billboard (the "ghit" texture) spawned in
 * world space at the point a Grave-olver shot lands — on an enemy, a wall, or
 * whatever it strikes — to punch up the impact. Purely cosmetic; it fades on a
 * short life timer. The texture is resident in VRAM (loaded once at startup) so
 * it works in every level.
 */
#define MAX_BULLET_HITS 8

typedef struct {
    int32_t x, y, z;
    int     life;      /* frames left; 0 = free */
    int     max_life;
} BulletHit;

void bullet_hits_load_texture(void);   /* STARTUP only (LoadImage) */
void bullet_hit_spawn(int32_t x, int32_t y, int32_t z);
void bullet_hits_update(void);
void bullet_hits_draw(RenderContext *ctx);
void bullet_hits_reset(void);

#endif
