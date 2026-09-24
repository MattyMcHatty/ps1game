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
/* A boulder smashing on the floor (Asag's slam). spawn_wood_burst's twin with
   stone colours and square chunks instead of planks; see the .c for why those
   are the only two things that differ. Like every burst here it OVERWRITES the
   whole pool, so two smashes on one frame show as one. */
void spawn_rock_burst(int32_t x, int32_t y, int32_t z);
/* ---- A DIRECTIONAL PUFF OF SOOT (the Incinerator venting) ------------------
   The other four bursts here are EXPLOSIONS: they throw in every direction off
   a point, because something broke there. This one is a VENT - it leaves a hole
   travelling one way - so it is the only one that takes a direction. (dx,dz) is
   that direction in plan, expected as one of -1/0/+1 on each axis, and the
   speed and spread are the .c's own.

   It is also the only one that does not fill the pool: a machine clearing its
   throat is a smaller event than a crate coming apart, so it writes SOOT_COUNT
   slots and sets particle_count to match. The slots past it stop being drawn,
   which is the same wholesale-overwrite contract the four above already have.

   BLACK-GREY, and it is read against the furnace it comes out of rather than
   against the room: draw_pool fades every particle to black over its life, and
   this one starts only a little above that, so what the player sees is soot
   crossing the glow and then gone. */
void spawn_soot_spurt(int32_t x, int32_t y, int32_t z, int32_t dx, int32_t dz);
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
