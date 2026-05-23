#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "vampire.h"

int32_t vampire_x      = 1200;
int32_t vampire_z      = 1200;
int     vampire_health = VAMPIRE_MAX_HEALTH;
int32_t vampire_kb_vx  = 0;
int32_t vampire_kb_vz  = 0;

void update_vampire(void) {
    if (vampire_health <= 0) return;

    if (vampire_kb_vx != 0 || vampire_kb_vz != 0) {
        vampire_x += vampire_kb_vx;
        vampire_z += vampire_kb_vz;
        if (vampire_x < -1600) { vampire_x = -1600; vampire_kb_vx = 0; }
        if (vampire_x >  1600) { vampire_x =  1600; vampire_kb_vx = 0; }
        if (vampire_z < -1600) { vampire_z = -1600; vampire_kb_vz = 0; }
        if (vampire_z >  1600) { vampire_z =  1600; vampire_kb_vz = 0; }
        vampire_kb_vx = (vampire_kb_vx * 7) >> 3;
        vampire_kb_vz = (vampire_kb_vz * 7) >> 3;
        damage_timer = 0;
        return;
    }

    int32_t dx   = cam_x - vampire_x;
    int32_t dz   = cam_z - vampire_z;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (!game_over && dist < CATCH_DIST) {
        if (++damage_timer >= DAMAGE_INTERVAL) {
            damage_timer = 0;
            player_health -= 1;
            if (player_health <= 0) {
                player_health = 0;
                game_over   = 1;
                flash_timer = 90;
            }
        }
    } else {
        damage_timer = 0;
    }
    if (dist < VAMPIRE_SPEED) return;
    vampire_x += (dx * VAMPIRE_SPEED) / dist;
    vampire_z += (dz * VAMPIRE_SPEED) / dist;
}

void draw_vampire(RenderContext *ctx) {
    if (vampire_health <= 0) return;
    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((VAMPIRE_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((VAMPIRE_HALF_W * rz) >> 12);

    SVECTOR v[4];
    v[0].vx = vampire_x - dwx; v[0].vy = VAMPIRE_Y - VAMPIRE_HALF_H; v[0].vz = vampire_z - dwz; v[0].pad = 0;
    v[1].vx = vampire_x + dwx; v[1].vy = VAMPIRE_Y - VAMPIRE_HALF_H; v[1].vz = vampire_z + dwz; v[1].pad = 0;
    v[2].vx = vampire_x + dwx; v[2].vy = VAMPIRE_Y + VAMPIRE_HALF_H; v[2].vz = vampire_z + dwz; v[2].pad = 0;
    v[3].vx = vampire_x - dwx; v[3].vy = VAMPIRE_Y + VAMPIRE_HALF_H; v[3].vz = vampire_z - dwz; v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4];
    int32_t otz, nclip;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);

    gte_nclip();
    gte_stopz(&nclip);
    if (nclip <= 0) return;

    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);

    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return;

    gte_avsz4();
    gte_stotz(&otz);

    if (otz <= 0 || otz >= OT_LENGTH) return;

    int32_t dx   = vampire_x - cam_x;
    int32_t dz   = vampire_z - cam_z;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int32_t fog_start  = 500;
    int32_t fog_end    = 3000;
    int32_t fog        = dist < fog_start ? fog_start : dist > fog_end ? fog_end : dist;
    int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

    uint8_t r = (uint8_t)((60 * fog_factor) >> 8);
    uint8_t g = (uint8_t)(( 5 * fog_factor) >> 8);
    uint8_t b = (uint8_t)(( 5 * fog_factor) >> 8);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

    POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
    setPolyF4(poly);
    setRGB0(poly, r, g, b);

    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    ctx->next_packet += sizeof(POLY_F4);
}
