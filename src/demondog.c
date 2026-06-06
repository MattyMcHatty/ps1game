#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "collision.h"
#include "crate.h"
#include "particles.h"
#include "demondog.h"

DemonDog demon_dogs[MAX_DEMON_DOGS];
int      demon_dog_count = 0;

static DemonDog ddog_defaults[MAX_DEMON_DOGS];

static uint16_t sleep_tpage  = 0, sleep_clut  = 0;
static uint16_t alert_tpage  = 0, alert_clut  = 0;
static uint16_t alert2_tpage = 0, alert2_clut = 0;
static uint16_t shadow_tpage = 0, shadow_clut = 0;

static void load_tim(const char *filename, uint16_t *tpage_out, uint16_t *clut_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return;
    int sectors = (file.size + 2047) / 2048;
    void *buf = malloc(sectors * 2048);
    if (!buf) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
    }
    *tpage_out = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    *clut_out  = getClut(tim.crect->x, tim.crect->y);
    free(buf);
}

void demon_dogs_init(void) {
    load_tim("\\DDSLEEP.TIM;1", &sleep_tpage,  &sleep_clut);
    load_tim("\\DDALERT.TIM;1", &alert_tpage,  &alert_clut);
    load_tim("\\DDALRT2.TIM;1", &alert2_tpage, &alert2_clut);
    load_tim("\\SHADOW.TIM;1",  &shadow_tpage, &shadow_clut);

    int i = 0;

    demon_dogs[i].x = -3824; demon_dogs[i].y = 0; demon_dogs[i].z = 3800;
    demon_dogs[i].health = DDOG_MAX_HEALTH; demon_dogs[i].state = DDOG_DORMANT; demon_dogs[i].active = 1; i++;

    demon_dogs[i].x = -3824; demon_dogs[i].y = 0; demon_dogs[i].z = 4300;
    demon_dogs[i].health = DDOG_MAX_HEALTH; demon_dogs[i].state = DDOG_DORMANT; demon_dogs[i].active = 1; i++;

    demon_dogs[i].x = -3824; demon_dogs[i].y = 0; demon_dogs[i].z = 4700;
    demon_dogs[i].health = DDOG_MAX_HEALTH; demon_dogs[i].state = DDOG_DORMANT; demon_dogs[i].active = 1; i++;

    demon_dog_count = i;

    int j;
    for (j = 0; j < demon_dog_count; j++)
        ddog_defaults[j] = demon_dogs[j];
}

void demon_dogs_reset(void) {
    int i;
    for (i = 0; i < demon_dog_count; i++)
        demon_dogs[i] = ddog_defaults[i];
}

void update_demon_dogs(void) {
    int i;
    for (i = 0; i < demon_dog_count; i++) {
        DemonDog *d = &demon_dogs[i];
        if (!d->active || d->state == DDOG_DEAD) continue;

        if (d->hit_timer > 0) d->hit_timer--;
        if (d->state == DDOG_ALERT) d->anim_tick++;

        apply_ddog_height(&d->x, &d->y, &d->z, &d->vy,
                          &d->on_upper_floor, &d->on_ramp);

        if (d->kb_vx != 0 || d->kb_vz != 0) {
            d->x += d->kb_vx;
            d->z += d->kb_vz;
            apply_ddog_collision(&d->x, &d->z, d->on_upper_floor, d->on_ramp);
            crates_collide(&d->x, d->y, &d->z, 80);
            if (d->kb_vx > 0) d->kb_vx =  (  d->kb_vx * 7) >> 3;
            else               d->kb_vx = -((-d->kb_vx * 7) >> 3);
            if (d->kb_vz > 0) d->kb_vz =  (  d->kb_vz * 7) >> 3;
            else               d->kb_vz = -((-d->kb_vz * 7) >> 3);
            d->damage_timer = 0;
            continue;
        }

        int32_t dx     = cam_x - d->x;
        int32_t dy     = cam_y - d->y;
        int32_t dz     = cam_z - d->z;
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);

        if (d->state == DDOG_DORMANT) {
            if (dist2d < DDOG_WAKE_RADIUS)
                d->state = DDOG_ALERT;
            else
                continue;
        }

        if (!game_over && dist3d < DDOG_CATCH_DIST) {
            if (++d->damage_timer >= DDOG_DAMAGE_INTERVAL) {
                d->damage_timer = 0;
                player_health--;
                if (player_health <= 0) {
                    player_health = 0;
                    game_over     = 1;
                    flash_timer   = 90;
                }
            }
        } else {
            d->damage_timer = 0;
        }

        if (dist2d < DDOG_CATCH_DIST) continue;
        d->x += (dx * DDOG_SPEED) / dist2d;
        d->z += (dz * DDOG_SPEED) / dist2d;
        apply_ddog_collision(&d->x, &d->z, d->on_upper_floor, d->on_ramp);
        crates_collide(&d->x, d->y, &d->z, 80);
    }
}

static void draw_ddog_shadow(RenderContext *ctx, DemonDog *d) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(DR_TPAGE) + sizeof(POLY_FT4) > buf_end) return;

    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((DDOG_SHADOW_W * rx) >> 12);
    int16_t dwz = (int16_t)((DDOG_SHADOW_W * rz) >> 12);

    int32_t fx  = isin(cam_rot);
    int32_t fz  = icos(cam_rot);
    int16_t ddx = (int16_t)((DDOG_SHADOW_D * fx) >> 12);
    int16_t ddz = (int16_t)((DDOG_SHADOW_D * fz) >> 12);

    int32_t shadow_y = d->y + DDOG_Y_OFFSET + DDOG_HALF_H - 2;

    SVECTOR sv[4];
    sv[0].vx = (int16_t)(d->x - dwx - ddx); sv[0].vy = (int16_t)shadow_y; sv[0].vz = (int16_t)(d->z - dwz - ddz); sv[0].pad = 0;
    sv[1].vx = (int16_t)(d->x + dwx - ddx); sv[1].vy = (int16_t)shadow_y; sv[1].vz = (int16_t)(d->z + dwz - ddz); sv[1].pad = 0;
    sv[2].vx = (int16_t)(d->x - dwx + ddx); sv[2].vy = (int16_t)shadow_y; sv[2].vz = (int16_t)(d->z - dwz + ddz); sv[2].pad = 0;
    sv[3].vx = (int16_t)(d->x + dwx + ddx); sv[3].vy = (int16_t)shadow_y; sv[3].vz = (int16_t)(d->z + dwz + ddz); sv[3].pad = 0;

    DVECTOR ssv[4];
    int32_t otz;

    gte_ldv0(&sv[0]); gte_rtps(); gte_stsxy(&ssv[0]);
    gte_ldv0(&sv[1]); gte_rtps(); gte_stsxy(&ssv[1]);
    gte_ldv0(&sv[2]); gte_rtps(); gte_stsxy(&ssv[2]);
    gte_ldv0(&sv[3]); gte_rtps(); gte_stsxy(&ssv[3]);

    gte_avsz4();
    gte_stotz(&otz);

    if (otz <= 0 || otz >= OT_LENGTH) return;

    int32_t shadow_otz = otz + 2 < OT_LENGTH ? otz + 2 : OT_LENGTH - 1;

    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 1, shadow_tpage);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[shadow_otz + 1], tp);
    ctx->next_packet += sizeof(DR_TPAGE);

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, 128, 128, 128);

    poly->x0 = ssv[0].vx; poly->y0 = ssv[0].vy;
    poly->x1 = ssv[1].vx; poly->y1 = ssv[1].vy;
    poly->x2 = ssv[2].vx; poly->y2 = ssv[2].vy;
    poly->x3 = ssv[3].vx; poly->y3 = ssv[3].vy;

    /* Texture at VRAM (640,160): tpage base y=0, so V offset = 160 */
    poly->u0 =  0; poly->v0 = 160;
    poly->u1 = 63; poly->v1 = 160;
    poly->u2 =  0; poly->v2 = 191;
    poly->u3 = 63; poly->v3 = 191;

    poly->clut  = shadow_clut;
    poly->tpage = shadow_tpage;

    addPrim(&ctx->buffers[ctx->active_buffer].ot[shadow_otz], poly);
    ctx->next_packet += sizeof(POLY_FT4);
}

static void draw_ddog_sprite(RenderContext *ctx, DemonDog *d,
                              uint16_t tpage, uint16_t clut, uint8_t v_start, int flip) {
    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((DDOG_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((DDOG_HALF_W * rz) >> 12);

    int16_t vy_top = (int16_t)(d->y + DDOG_Y_OFFSET - DDOG_HALF_H);
    int16_t vy_bot = (int16_t)(d->y + DDOG_Y_OFFSET + DDOG_HALF_H);

    SVECTOR v[4];
    v[0].vx = d->x - dwx; v[0].vy = vy_top; v[0].vz = d->z - dwz; v[0].pad = 0;
    v[1].vx = d->x + dwx; v[1].vy = vy_top; v[1].vz = d->z + dwz; v[1].pad = 0;
    v[2].vx = d->x + dwx; v[2].vy = vy_bot; v[2].vz = d->z + dwz; v[2].pad = 0;
    v[3].vx = d->x - dwx; v[3].vy = vy_bot; v[3].vz = d->z - dwz; v[3].pad = 0;

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
    otz >>= 2;
    if (otz < 2) otz = 2;
    if (otz >= OT_LENGTH) return;

    int32_t fdx        = d->x - cam_x;
    int32_t fdz        = d->z - cam_z;
    int32_t dist       = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    int32_t fog_start  = 500;
    int32_t fog_end    = 3000;
    int32_t fog        = dist < fog_start ? fog_start : dist > fog_end ? fog_end : dist;
    int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);
    uint8_t fog8       = fog_factor > 255 ? 255 : (uint8_t)fog_factor;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, fog8, fog8, fog8);

    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

    uint8_t u_left  = flip ? 63 : 0;
    uint8_t u_right = flip ?  0 : 63;
    poly->u0 = u_left;  poly->v0 = v_start;
    poly->u1 = u_right; poly->v1 = v_start;
    poly->u2 = u_left;  poly->v2 = v_start + 63;
    poly->u3 = u_right; poly->v3 = v_start + 63;

    poly->tpage = tpage;
    poly->clut  = clut;

    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    ctx->next_packet += sizeof(POLY_FT4);

    if (d->hit_timer <= 0) return;

    int16_t bar_cx  = (sv[0].vx + sv[1].vx) / 2;
    int16_t bar_top = (sv[0].vy < sv[1].vy ? sv[0].vy : sv[1].vy) - 8;
    int16_t bar_x   = bar_cx - 20;
    int32_t bar_otz = otz > 0 ? otz - 1 : 0;

    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setRGB0(bg, 40, 40, 40);
        setXY0(bg, bar_x, bar_top);
        setWH(bg, 40, 5);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[bar_otz + 1], bg);
        ctx->next_packet += sizeof(TILE);
    }

    int16_t fill_w = (int16_t)((d->health * 40) / DDOG_MAX_HEALTH);
    if (fill_w > 0 && ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *fill = (TILE *)ctx->next_packet;
        setTile(fill);
        setRGB0(fill, 200, 20, 20);
        setXY0(fill, bar_x, bar_top);
        setWH(fill, fill_w, 5);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[bar_otz], fill);
        ctx->next_packet += sizeof(TILE);
    }
}

void draw_demon_dogs(RenderContext *ctx) {
    int i;
    for (i = 0; i < demon_dog_count; i++) {
        DemonDog *d = &demon_dogs[i];
        if (!d->active || d->state == DDOG_DEAD) continue;

        int32_t dx = d->x - cam_x;
        int32_t dz = d->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 4000) continue;

        draw_ddog_shadow(ctx, d);

        if (d->state == DDOG_DORMANT) {
            draw_ddog_sprite(ctx, d, sleep_tpage, sleep_clut, 128, 0);
        } else {
            /* Flip the sprite when the dog is to the player's right so it
               always faces toward the player. Camera right vector = (icos, -isin). */
            int32_t dot = ((dx >> 4) * (icos(cam_rot) >> 4))
                        - ((dz >> 4) * (isin(cam_rot) >> 4));
            int flip = dot <= 0;
            /* Alternate between open-mouth and closed-mouth every 15 frames */
            if ((d->anim_tick / 25) & 1)
                draw_ddog_sprite(ctx, d, alert2_tpage, alert2_clut,   0, flip);
            else
                draw_ddog_sprite(ctx, d, alert_tpage,  alert_clut,  192, flip);
        }
    }
}
