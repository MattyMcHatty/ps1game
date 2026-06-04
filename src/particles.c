#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "particles.h"

Particle particles[MAX_PARTICLES];
int      particle_count = 0;

static uint32_t rng_state = 12345;

static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int32_t rng_range(int32_t range) {
    return (int32_t)(rng_next() % (uint32_t)(range * 2 + 1)) - range;
}

void spawn_burst(int32_t x, int32_t y, int32_t z,
                 uint8_t r, uint8_t g, uint8_t b) {
    int i;
    for (i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        p->x        = x;
        p->y        = y;
        p->z        = z;
        p->vx       = rng_range(40);
        p->vy       = -rng_range(30) - 10;
        p->vz       = rng_range(40);
        p->life     = 45 + (int32_t)(rng_next() % 30);
        p->max_life = p->life;
        p->sw       = 4 + (uint8_t)(rng_next() % 8);
        p->sh       = p->sw;
        p->r0       = r;
        p->g0       = g;
        p->b0       = b;
    }
    particle_count = MAX_PARTICLES;
}

void spawn_blood_burst(int32_t x, int32_t y, int32_t z) {
    spawn_burst(x, y, z, 220, 55, 0);
}

void spawn_wood_burst(int32_t x, int32_t y, int32_t z) {
    int i;
    for (i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        p->x        = x;
        p->y        = y;
        p->z        = z;
        p->vx       = rng_range(40);
        p->vy       = -(int32_t)(rng_next() % 18) - 8;   /* 8–26 up, ~60–170 world units */
        p->vz       = rng_range(40);
        p->life     = 35 + (int32_t)(rng_next() % 30);
        p->max_life = p->life;
        p->sw       = 16 + (uint8_t)(rng_next() % 14);   /* 16–30 px wide plank */
        p->sh       =  4 + (uint8_t)(rng_next() %  5);   /* 4–9 px tall */
        p->r0       = 130;
        p->g0       = 85;
        p->b0       = 25;
    }
    particle_count = MAX_PARTICLES;
}

void update_particles(void) {
    int i;
    for (i = 0; i < particle_count; i++) {
        Particle *p = &particles[i];
        if (p->life <= 0) continue;
        p->x  += p->vx;
        p->y  += p->vy;
        p->z  += p->vz;
        p->vy += 3;
        p->vx  = (p->vx * 28) >> 5;
        p->vz  = (p->vz * 28) >> 5;
        p->life--;
    }
}

void draw_particles(RenderContext *ctx) {
    int i;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < particle_count; i++) {
        Particle *p = &particles[i];
        if (p->life <= 0) continue;

        SVECTOR sv;
        sv.vx  = (int16_t)p->x;
        sv.vy  = (int16_t)p->y;
        sv.vz  = (int16_t)p->z;
        sv.pad = 0;

        DVECTOR screen;
        int32_t otz;

        gte_ldv0(&sv);
        gte_rtps();
        gte_stsxy(&screen);
        gte_avsz3();
        gte_stotz(&otz);

        if (otz <= 0 || otz >= OT_LENGTH) continue;
        if (ctx->next_packet + sizeof(TILE) > buf_end) continue;

        int32_t t = (p->life << 8) / p->max_life;
        uint8_t r = (uint8_t)(((int32_t)p->r0 * t) >> 8);
        uint8_t g = (uint8_t)(((int32_t)p->g0 * t) >> 8);
        uint8_t b = (uint8_t)(((int32_t)p->b0 * t) >> 8);
        if (r < 8 && g < 8 && b < 8) r = 8;

        TILE *tile = (TILE *)ctx->next_packet;
        setTile(tile);
        setRGB0(tile, r, g, b);
        setXY0(tile, screen.vx - p->sw / 2, screen.vy - p->sh / 2);
        setWH(tile, p->sw, p->sh);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], tile);
        ctx->next_packet += sizeof(TILE);
    }
}

void reset_particles(void) {
    particle_count = 0;
}
