#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "vampire.h"
#include "particles.h"
#include "bat.h"
#include "crate.h"
#include "demondog.h"

static SMD  *crucifaxe_smd  = NULL;
static void *crucifaxe_buff = NULL;

int swing_timer    = 0;
static int hit_this_swing       = 0;
static int crate_hit_this_swing = 0;
static int ddog_hit_this_swing  = 0;

void bat_init(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\CRFAXE.SMD;1")) return;
    int sectors    = (file.size + 2047) / 2048;
    crucifaxe_buff = malloc(sectors * 2048);
    if (!crucifaxe_buff) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)crucifaxe_buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    crucifaxe_smd = smdInitData(crucifaxe_buff);
}

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

void update_bat(void) {
    if (swing_timer == 0 && pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        if (~pad->btn & PAD_SQUARE) {
            swing_timer          = 1;
            hit_this_swing       = 0;
            crate_hit_this_swing = 0;
            ddog_hit_this_swing  = 0;
        }
    }

    if (swing_timer > 0) {
        if (swing_timer <= SWING_DURATION && !hit_this_swing && vampire_health > 0) {
            int32_t dx     = vampire_x - cam_x;
            int32_t dy     = vampire_y - cam_y;
            int32_t dz     = vampire_z - cam_z;
            int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
            if (dist3d < SWING_RANGE) {
                int32_t dot = ((int32_t)dx * isin(cam_rot) +
                               (int32_t)dz * icos(cam_rot)) >> 12;
                if (dot > 0) {
                    vampire_kb_vx    = dist2d > 0 ? (dx * KNOCKBACK_SPEED) / dist2d : 0;
                    vampire_kb_vz    = dist2d > 0 ? (dz * KNOCKBACK_SPEED) / dist2d : 0;
                    vampire_health--;
                    if (vampire_health <= 0)
                        spawn_blood_burst(vampire_x, vampire_y, vampire_z);
                    vampire_hit_timer = VAMPIRE_BAR_TIMER_MAX;
                    hit_this_swing = 1;
                }
            }
        }
        /* Demon dog hit — checked independently of vampire hit */
        if (swing_timer <= SWING_DURATION && !ddog_hit_this_swing) {
            int di;
            for (di = 0; di < demon_dog_count; di++) {
                DemonDog *d = &demon_dogs[di];
                if (!d->active || d->state == DDOG_DEAD) continue;
                int32_t dx     = d->x - cam_x;
                int32_t dy     = d->y - cam_y;
                int32_t dz     = d->z - cam_z;
                int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
                int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
                if (dist3d < SWING_RANGE) {
                    int32_t dot = ((int32_t)dx * isin(cam_rot) +
                                   (int32_t)dz * icos(cam_rot)) >> 12;
                    if (dot > 0) {
                        d->kb_vx = dist2d > 0 ? (dx * DDOG_KNOCKBACK) / dist2d : 0;
                        d->kb_vz = dist2d > 0 ? (dz * DDOG_KNOCKBACK) / dist2d : 0;
                        d->health--;
                        d->hit_timer = DDOG_BAR_TIMER_MAX;
                        if (d->health <= 0) {
                            d->state = DDOG_DEAD;
                            spawn_blood_burst(d->x, d->y, d->z);
                        }
                        ddog_hit_this_swing = 1;
                        break;
                    }
                }
            }
        }

        /* Crate smash — checked independently of vampire hit */
        if (swing_timer <= SWING_DURATION && !crate_hit_this_swing) {
            if (crate_try_smash())
                crate_hit_this_swing = 1;
        }

        if (++swing_timer > SWING_TOTAL)
            swing_timer = 0;
    }
}

void draw_bat(RenderContext *ctx) {
    if (!crucifaxe_smd) return;

    int32_t t;
    if (swing_timer == 0) {
        t = 0;
    } else if (swing_timer <= SWING_DURATION) {
        t = (swing_timer * 256) / SWING_DURATION;
    } else {
        int32_t ret = swing_timer - SWING_DURATION;
        t = 256 - (ret * 256) / SWING_RETURN;
    }

    /* Weapon position in view/camera space — fixed offset from camera centre */
    int32_t vs_x =  40;
    int32_t vs_y =  80 + (( 7 * t) >> 8);
    int32_t vs_z = 125;

    /* Swing: reversed (away from camera), axis at 35° to match weapon yaw.
       cos(35°)/cos(45°) ≈ 1.158 → vx; sin(35°)/sin(45°) ≈ 0.812 → vz.
       Base pitch of -150 tilts the weapon top toward the player at rest
       so the top face is slightly visible. */
    int32_t swing_mag   = (t * 1024) >> 8;
    int16_t swing_vx    = (int16_t)((-(swing_mag * 4744) >> 12) - 150);
    int16_t swing_vz    = (int16_t)(-(swing_mag * 3326) >> 12);

    /* Build a pure view-space weapon matrix.
       Rotation: diagonal pitch+roll swing (X+Z), 35° yaw for hold (Y). */
    SVECTOR swing_rot = {swing_vx, 398, swing_vz, 0};
    MATRIX  weapon_vs;
    RotMatrix(&swing_rot, &weapon_vs);
    weapon_vs.t[0] = vs_x;
    weapon_vs.t[1] = vs_y;
    weapon_vs.t[2] = vs_z;

    gte_SetRotMatrix(&weapon_vs);
    gte_SetTransMatrix(&weapon_vs);

    /* Manual render — skips NCLIP so all faces draw regardless of winding.
       Applies diffuse shading via dot product of face normal with a fixed
       light direction, giving visible angle variation across faces. */
    {
        /* Light direction in model space: upper-right-front.
           (1,-1,1)/sqrt(3) * 4096 ≈ (2365,-2365,2365) */
        const int32_t lx =  2365, ly = -2365, lz = 2365;

        uint8_t *p       = (uint8_t *)crucifaxe_smd->p_prims;
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        int pi;
        for (pi = 0; pi < crucifaxe_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
            uint8_t       stride = pt->len;
            int           is_quad = (pt->type >= 2);
            uint16_t     *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &crucifaxe_smd->p_verts[vi[0]];
            SVECTOR *v1 = &crucifaxe_smd->p_verts[vi[1]];
            SVECTOR *v2 = &crucifaxe_smd->p_verts[vi[2]];

            DVECTOR sv[4];
            int32_t sz[4], otz;

            gte_ldv3(v0, v1, v2);
            gte_rtpt();
            gte_stsxy3c(sv);
            gte_stsz4c(sz);
            if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

            if (is_quad) {
                SVECTOR *v3 = &crucifaxe_smd->p_verts[vi[3]];
                gte_ldv0(v3); gte_rtps(); gte_stsxy(&sv[3]);
                gte_avsz4();
            } else {
                gte_avsz3();
            }
            gte_stotz(&otz);
            if (otz <= 0 || otz >= OT_LENGTH) { p += stride; continue; }

            /* Per-face normal shading: 40% ambient + primary + dim fill */
            uint16_t n0_idx = *(uint16_t *)(p + 12);
            SVECTOR *norm   = &crucifaxe_smd->p_norms[n0_idx];
            int32_t dot = ((int32_t)norm->vx * lx +
                           (int32_t)norm->vy * ly +
                           (int32_t)norm->vz * lz) >> 12;
            int32_t dot2 = -dot;  /* fill light from opposite direction */
            if (dot  < 0) dot  = 0;
            if (dot2 < 0) dot2 = 0;
            /* primary: 0-60%, fill: 0-20%, ambient: 40% — total 40-100% */
            int32_t shade = 1638 + ((dot * 2458) >> 12) + ((dot2 * 820) >> 12);
            if (shade > 4096) shade = 4096;

            uint8_t r = (uint8_t)(((int32_t)p[16] * shade) >> 12);
            uint8_t g = (uint8_t)(((int32_t)p[17] * shade) >> 12);
            uint8_t b = (uint8_t)(((int32_t)p[18] * shade) >> 12);

            if (is_quad) {
                if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
                POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
                setPolyF4(poly);
                setRGB0(poly, r, g, b);
                poly->x0=sv[0].vx; poly->y0=sv[0].vy;
                poly->x1=sv[1].vx; poly->y1=sv[1].vy;
                poly->x2=sv[2].vx; poly->y2=sv[2].vy;
                poly->x3=sv[3].vx; poly->y3=sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F4);
            } else {
                if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
                POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
                setPolyF3(poly);
                setRGB0(poly, r, g, b);
                poly->x0=sv[0].vx; poly->y0=sv[0].vy;
                poly->x1=sv[1].vx; poly->y1=sv[1].vy;
                poly->x2=sv[2].vx; poly->y2=sv[2].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F3);
            }
            p += stride;
        }
    }

    /* Restore view matrix */
    MATRIX view;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &view);
    VECTOR vt = {-cam_x, -cam_y, -cam_z};
    ApplyMatrixLV(&view, &vt, &vt);
    view.t[0] = vt.vx;
    view.t[1] = vt.vy;
    view.t[2] = vt.vz;
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
