#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "vampire.h"
#include "particles.h"
#include "bat.h"

int swing_timer    = 0;
static int hit_this_swing = 0;

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

void update_bat(void) {
    if (swing_timer == 0 && pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        if (~pad->btn & PAD_SQUARE) {
            swing_timer    = 1;
            hit_this_swing = 0;
        }
    }

    if (swing_timer > 0) {
        if (swing_timer <= SWING_DURATION && !hit_this_swing) {
            int32_t dx   = vampire_x - cam_x;
            int32_t dz   = vampire_z - cam_z;
            int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (dist < SWING_RANGE) {
                int32_t dot = ((int32_t)dx * isin(cam_rot) +
                               (int32_t)dz * icos(cam_rot)) >> 12;
                if (dot > 0) {
                    vampire_kb_vx    = (dx * KNOCKBACK_SPEED) / dist;
                    vampire_kb_vz    = (dz * KNOCKBACK_SPEED) / dist;
                    vampire_health--;
                    if (vampire_health <= 0)
                        spawn_blood_burst(vampire_x, 0, vampire_z);
                    vampire_hit_timer = VAMPIRE_BAR_TIMER_MAX;
                    hit_this_swing = 1;
                }
            }
        }
        if (++swing_timer > SWING_TOTAL)
            swing_timer = 0;
    }
}

void draw_bat(RenderContext *ctx) {
    int32_t t;
    if (swing_timer == 0) {
        t = 0;
    } else if (swing_timer <= SWING_DURATION) {
        t = (swing_timer * 256) / SWING_DURATION;
    } else {
        int32_t ret = swing_timer - SWING_DURATION;
        t = 256 - (ret * 256) / SWING_RETURN;
    }

    int32_t fwd_x   =  isin(cam_rot);
    int32_t fwd_z   =  icos(cam_rot);
    int32_t right_x =  icos(cam_rot);
    int32_t right_z = -isin(cam_rot);

    int32_t h_lx = 25 - (( 3 * t) >> 8);
    int32_t h_ly = 40 + (( 7 * t) >> 8);
    int32_t h_lz = 40;

    int32_t p_lx = 12  - (( 92 * t) >> 8);
    int32_t p_ly = -10 + (( 45 * t) >> 8);
    int32_t p_lz = 100 + (( 60 * t) >> 8);

    const int32_t W = 4;
    const int32_t H = 5;

#define CORNER(sv, lx, ly, lz) \
    (sv).vx  = (int16_t)(cam_x + (((lx)*right_x + (lz)*fwd_x) >> 12)); \
    (sv).vy  = (int16_t)(ly); \
    (sv).vz  = (int16_t)(cam_z + (((lx)*right_z + (lz)*fwd_z) >> 12)); \
    (sv).pad = 0

    SVECTOR c[8];
    CORNER(c[0], h_lx-W, h_ly-H, h_lz);
    CORNER(c[1], h_lx+W, h_ly-H, h_lz);
    CORNER(c[2], h_lx+W, h_ly+H, h_lz);
    CORNER(c[3], h_lx-W, h_ly+H, h_lz);
    CORNER(c[4], p_lx-W, p_ly-H, p_lz);
    CORNER(c[5], p_lx+W, p_ly-H, p_lz);
    CORNER(c[6], p_lx+W, p_ly+H, p_lz);
    CORNER(c[7], p_lx-W, p_ly+H, p_lz);
#undef CORNER

    /* Winding is CCW from outside; nclip discards backfaces automatically.
       Vertex order (v0,v1,v2,v3) → poly (x0,x1, x2=v3,x3=v2). */
    static const struct { int8_t i[4]; uint8_t r, g, b; } faces[5] = {
        { {0, 4, 5, 1}, 130,  75, 22 },
        { {0, 3, 7, 4},  80,  46, 14 },
        { {1, 2, 6, 5}, 105,  60, 18 },
        { {4, 5, 6, 7}, 150,  88, 26 },
        { {2, 3, 7, 6},  55,  32, 10 },
    };

    int fi;
    for (fi = 0; fi < 5; fi++) {
        DVECTOR sv[4];
        int32_t otz, nclip;

        gte_ldv3(&c[faces[fi].i[0]], &c[faces[fi].i[1]], &c[faces[fi].i[2]]);
        gte_rtpt();
        gte_stsxy3c(sv);

        gte_nclip();
        gte_stopz(&nclip);
        if (nclip <= 0) continue;

        gte_ldv0(&c[faces[fi].i[3]]);
        gte_rtps();
        gte_stsxy(&sv[3]);

        gte_avsz4();
        gte_stotz(&otz);
        if (otz <= 0 || otz >= OT_LENGTH) continue;

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, faces[fi].r, faces[fi].g, faces[fi].b);

        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
        poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}
