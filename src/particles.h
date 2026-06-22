#ifndef PARTICLES_H
#define PARTICLES_H

#include <stdint.h>
#include "render.h"

#define MAX_PARTICLES 32
#define MAX_FIRE       32   /* continuous flame pool (e.g. the lit stove) */

typedef struct {
    int32_t x, y, z;
    int32_t vx, vy, vz;
    int32_t life;
    int32_t max_life;
    uint8_t sw, sh;       /* screen-space width and height of the tile */
    uint8_t r0, g0, b0;  /* peak colour, fades with life (bursts only) */
    uint8_t kind;         /* flame cone layer: 0 = outer (blue), 1 = inner (white) */
} Particle;

extern Particle particles[MAX_PARTICLES];
extern int      particle_count;

void spawn_burst(int32_t x, int32_t y, int32_t z, uint8_t r, uint8_t g, uint8_t b);
void spawn_blood_burst(int32_t x, int32_t y, int32_t z);
void spawn_wood_burst(int32_t x, int32_t y, int32_t z);
void update_particles(void);
void draw_particles(RenderContext *ctx);
void reset_particles(void);

/* Continuous flame pool (the lit stove). Call fire_emit each frame while the
   flame is on; update_fire/draw_fire each frame to animate and render it. */
void fire_emit(int32_t x, int32_t y, int32_t z, int count);
void update_fire(void);
void draw_fire(RenderContext *ctx);
void reset_fire(void);

#endif
