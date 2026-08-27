#ifndef PARTICLES_H
#define PARTICLES_H

#include <stdint.h>
#include "render.h"

#define MAX_PARTICLES 32
#define MAX_FIRE       32   /* continuous flame pool (e.g. the lit stove) */
/* Continuous WATER pool. Sized for the Greenhouse's flood: SIX ceiling jets
   running at once, each emitting one droplet every SPRAY_EMIT_EVERY frames and
   each droplet living SPRAY_LIFE, so the steady state is
   6 * (SPRAY_LIFE / SPRAY_EMIT_EVERY) = 60 and the pool never has to drop one.
   Change any of the three and re-check that product — a full pool does not
   fail, it just thins the jets silently. */
#define MAX_SPRAY      60

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

/* ---- Continuous water spray (the Greenhouse's ceiling jets) ----------------
   A THIRD pool rather than a use of either of the two above, for the reason
   they are separate from each other: the burst pool is one-shot and is
   overwritten wholesale by the next smash, and the fire pool is a SINGLE
   emitter (draw_fire sorts the whole flame at one origin's depth, which is only
   right for one jet in one place). Water comes from six points at once, falls
   instead of rising, and has to sort per droplet against a room-sized space —
   so it gets its own pool with its own physics.

   spray_emit is called once per jet per frame; it self-limits, placing a
   droplet only every SPRAY_EMIT_EVERY frames and only into a dead slot. */
void spray_emit(int32_t x, int32_t y, int32_t z);
void update_spray(void);
void draw_spray(RenderContext *ctx);
void reset_spray(void);

#endif
