#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "particles.h"
#include "camera.h"

Particle particles[MAX_PARTICLES];
int      particle_count = 0;

/* Continuous fire pool — separate from the one-shot burst pool so the stove
   flame can run indefinitely without being overwritten by a crate/door smash,
   and so it can rise (no gravity) instead of falling like debris. */
static Particle fire[MAX_FIRE];

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

#define FIRE_OUTER 0   /* wide, taller, cool blue cone */
#define FIRE_INNER 1   /* narrow, short, hot white/cyan core */

/* Partition the pool so the short-lived inner core always has slots and the
   longer-lived outer cone can't crowd it out: [0,FIRE_INNER_MAX) = inner,
   [FIRE_INNER_MAX,MAX_FIRE) = outer. */
#define FIRE_INNER_MAX 12

/* Free-running clock for the flame's sine flicker (advanced in update_fire). */
static int32_t fire_time = 0;

/* Emitter origin, kept so draw_fire can sort the whole flame at one depth. */
static int32_t fire_ox, fire_oy, fire_oz;

/* Spawn one flame particle of the given cone layer into a free slot in that
   layer's range. */
static void fire_spawn(int32_t x, int32_t y, int32_t z, uint8_t kind) {
    int i, lo = (kind == FIRE_INNER) ? 0 : FIRE_INNER_MAX;
    int hi = (kind == FIRE_INNER) ? FIRE_INNER_MAX : MAX_FIRE;
    for (i = lo; i < hi; i++) {
        Particle *p = &fire[i];
        if (p->life > 0) continue;
        if (kind == FIRE_INNER) {
            /* Hot core: narrow, slow rise, short-lived, small. */
            p->x    = x + rng_range(3);
            p->z    = z + rng_range(3);
            p->vx   = rng_range(1);
            p->vz   = rng_range(1);
            p->vy   = -(3 + (int32_t)(rng_next() % 3));
            p->life = 4 + (int32_t)(rng_next() % 3);
            p->sw   = 2 + (uint8_t)(rng_next() % 3);
        } else {
            /* Cool envelope: wider, taller, longer-lived, larger. */
            p->x    = x + rng_range(6);
            p->z    = z + rng_range(6);
            p->vx   = rng_range(2);
            p->vz   = rng_range(2);
            p->vy   = -(5 + (int32_t)(rng_next() % 4));
            p->life = 6 + (int32_t)(rng_next() % 5);
            p->sw   = 3 + (uint8_t)(rng_next() % 4);
        }
        p->y        = y;
        p->sh       = p->sw;
        p->max_life = p->life;
        p->kind     = kind;
        return;   /* placed; done */
    }
}

/* Emit a frame of gas flame at (x,y,z): an outer blue cone plus a smaller hot
   inner core. `count` scales the outer emission; the core is ~half that. Fills
   only dead slots, so the pools self-limit their density. */
void fire_emit(int32_t x, int32_t y, int32_t z, int count) {
    int n;
    fire_ox = x; fire_oy = y; fire_oz = z;
    for (n = 0; n < count; n++)          fire_spawn(x, y, z, FIRE_OUTER);
    for (n = 0; n < (count + 1) / 2; n++) fire_spawn(x, y, z, FIRE_INNER);
}

void update_fire(void) {
    int i;
    fire_time++;
    for (i = 0; i < MAX_FIRE; i++) {
        Particle *p = &fire[i];
        if (p->life <= 0) continue;
        p->x  += p->vx;
        p->y  += p->vy;
        p->z  += p->vz;
        p->vx += rng_range(1);   /* tiny horizontal wander keeps it alive */
        p->vy += 1;              /* buoyancy decays so the flame slows as it rises */
        p->life--;
    }
}

/* Shared billboard renderer for both particle pools. */
static void draw_pool(RenderContext *ctx, Particle *pool, int count) {
    int i;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < count; i++) {
        Particle *p = &pool[i];
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

void draw_particles(RenderContext *ctx) {
    draw_pool(ctx, particles, particle_count);
}

/* Linear blend a->b by f (0..256). */
static uint8_t blend8(uint8_t a, uint8_t b, int32_t f) {
    return (uint8_t)((int32_t)a + (((int32_t)b - (int32_t)a) * f >> 8));
}

/* Gas flame. Coloured procedurally by cone layer + age (a blue base rising to
   a hot white/cyan core), with a sine-summed flicker, and drawn with additive
   semi-transparency so the cones glow and overlap like real gas jets.

   The whole flame is sorted at ONE depth — the emitter origin's projected OT
   index — so geometry nearer than the stove (walls, props) correctly occludes
   it, while additive blending (order-independent) means the cones don't need
   per-particle sorting among themselves. A DR_TPAGE added LAST to that band
   (processed first, LIFO) sets the additive blend rate (ABR=1) the setSemiTrans
   tiles then use. */
void draw_fire(RenderContext *ctx) {
    int i, drawn = 0;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    /* Project the emitter origin to one OT index for the whole flame. */
    int32_t base_otz;
    {
        SVECTOR ov;
        ov.vx = (int16_t)fire_ox; ov.vy = (int16_t)fire_oy; ov.vz = (int16_t)fire_oz; ov.pad = 0;
        /* Project the point 3x so avsz3 averages 3 valid SZ regs (a single rtps
           leaves two stale, giving a bogus depth). */
        gte_ldv3(&ov, &ov, &ov);
        gte_rtpt();
        gte_avsz3();
        gte_stotz(&base_otz);
        if (base_otz < SCENE_OT_MIN) base_otz = SCENE_OT_MIN;
        if (base_otz >= OT_LENGTH)   base_otz = OT_LENGTH - 1;
    }

    /* Distance fog matched to the kitchen walls (start 500, end 2200), but for
       an additive flame "fogging out" means fading toward black, so we scale
       the colour by fog_factor (256 near -> 0 far) instead of blending toward
       the fog colour. Beyond fog_end the flame is invisible, so skip it. */
    int32_t dx = fire_ox - cam_x;
    int32_t dz = fire_oz - cam_z;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (dist >= 2200) return;
    int32_t fog = dist < 500 ? 500 : dist;
    int32_t fog_factor = ((2200 - fog) << 8) / (2200 - 500);   /* 256..0 */

    /* Flicker: three out-of-phase sines → natural, non-random oscillation,
       with the fog factor folded into the brightness scale. */
    int32_t s = rsin((fire_time * 21) & 4095)
              + rsin((fire_time * 37) & 4095)
              + rsin((fire_time * 53) & 4095);
    int32_t flick = 230 + (s * 26) / 12288;   /* ~205..255 brightness scale */
    flick = (flick * fog_factor) >> 8;

    for (i = 0; i < MAX_FIRE; i++) {
        Particle *p = &fire[i];
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

        /* age 0 at birth (base) -> 256 at death (tip). */
        int32_t age = 256 - ((p->life << 8) / p->max_life);
        uint8_t r, g, b;
        if (p->kind == FIRE_INNER) {
            /* Hot core: white -> cyan -> blue. */
            r = blend8(210,  60, age);
            g = blend8(235, 170, age);
            b = 255;
        } else {
            /* Cool envelope: mid-blue -> deep dim blue (fades into the dark). */
            r = blend8( 40,  10, age);
            g = blend8(105,  20, age);
            b = blend8(255, 110, age);
        }
        r = (uint8_t)((r * flick) >> 8);
        g = (uint8_t)((g * flick) >> 8);
        b = (uint8_t)((b * flick) >> 8);

        TILE *tile = (TILE *)ctx->next_packet;
        setTile(tile);
        setSemiTrans(tile, 1);
        setRGB0(tile, r, g, b);
        setXY0(tile, screen.vx - p->sw / 2, screen.vy - p->sh / 2);
        setWH(tile, p->sw, p->sh);
        addPrim(&ot[base_otz], tile);
        ctx->next_packet += sizeof(TILE);
        drawn++;
    }

    /* Set the additive blend rate for the flame tiles. Added last so it is the
       first primitive processed in this band, before any tile. */
    if (drawn && ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[base_otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

void reset_particles(void) {
    particle_count = 0;
}

void reset_fire(void) {
    int i;
    for (i = 0; i < MAX_FIRE; i++) fire[i].life = 0;
}
