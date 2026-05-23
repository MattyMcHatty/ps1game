#ifndef PARTICLES_H
#define PARTICLES_H

#include <stdint.h>
#include "render.h"

#define MAX_PARTICLES 32

typedef struct {
    int32_t x, y, z;
    int32_t vx, vy, vz;
    int32_t life;
    int32_t max_life;
    uint8_t size;
} Particle;

extern Particle particles[MAX_PARTICLES];
extern int      particle_count;

void spawn_blood_burst(int32_t x, int32_t y, int32_t z);
void update_particles(void);
void draw_particles(RenderContext *ctx);
void reset_particles(void);

#endif
