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
#include "sound.h"

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
    static int hurt_sfx_cooldown = 0;
    int i;
    if (hurt_sfx_cooldown > 0) hurt_sfx_cooldown--;
    for (i = 0; i < demon_dog_count; i++) {
        DemonDog *d = &demon_dogs[i];
        if (!d->active || d->state == DDOG_DEAD) continue;

        if (d->hit_timer    > 0) d->hit_timer--;
        if (d->damage_timer > 0) d->damage_timer--;
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
            continue;
        }

        int32_t dx     = cam_x - d->x;
        int32_t dy     = cam_y - d->y;
        int32_t dz     = cam_z - d->z;
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);

        if (d->state == DDOG_DORMANT) {
            if (dist2d < DDOG_WAKE_RADIUS) {
                d->state      = DDOG_ALERT;
                d->bark_timer = DDOG_BARK_INTERVAL;
                sound_play(SFX_DOGBARK);
            } else {
                continue;
            }
        }

        /* Keep barking on an interval while alert and alive. */
        if (d->state == DDOG_ALERT) {
            if (--d->bark_timer <= 0) {
                d->bark_timer = DDOG_BARK_INTERVAL;
                sound_play(SFX_DOGBARK);
            }
        }

        if (!game_over && dist3d < DDOG_CATCH_DIST && d->damage_timer == 0) {
            d->damage_timer = DDOG_DAMAGE_COOLDOWN;
            player_health  -= DDOG_DAMAGE_AMOUNT;
            if (hurt_sfx_cooldown == 0) {
                sound_play(SFX_HURT);
                hurt_sfx_cooldown = 30;
            }
            if (player_health <= 0) {
                player_health = 0;
                game_over     = 1;
                flash_timer   = 90;
            }
        }

        if (dist2d < DDOG_CATCH_DIST) continue;

        /* --- Separation: soft push away from nearby dogs --- */
        int32_t sep_x = 0, sep_z = 0;
        int j;
        for (j = 0; j < demon_dog_count; j++) {
            if (j == i) continue;
            DemonDog *other = &demon_dogs[j];
            if (!other->active || other->state == DDOG_DEAD) continue;
            int32_t odx   = d->x - other->x;
            int32_t odz   = d->z - other->z;
            int32_t odist = (odx < 0 ? -odx : odx) + (odz < 0 ? -odz : odz);
            if (odist < DDOG_SEP_RADIUS && odist > 0) {
                int32_t push = DDOG_SEP_RADIUS - odist;   /* stronger when closer */
                sep_x += (odx * push) / odist;
                sep_z += (odz * push) / odist;
            }
        }

        /* --- Desired direction: toward player, biased by separation --- */
        int32_t desired_x = dx + sep_x * DDOG_SEP_WEIGHT;
        int32_t desired_z = dz + sep_z * DDOG_SEP_WEIGHT;
        int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                               (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;

        /* --- Obstacle feeler: probe ahead in the desired direction --- */
        int32_t feeler_x = d->x + (desired_x * DDOG_FEELER_LEN) / desired_dist;
        int32_t feeler_z = d->z + (desired_z * DDOG_FEELER_LEN) / desired_dist;
        int32_t fx = feeler_x, fz = feeler_z;
        crates_collide(&fx, d->y, &fz, 80);
        apply_ddog_collision(&fx, &fz, d->on_upper_floor, d->on_ramp);
        int blocked = (fx != feeler_x || fz != feeler_z);

        /* Perpendiculars to the player direction — the two ways to slide
           along a wall. */
        int32_t pl_x = -dz, pl_z =  dx;   /* left  */
        int32_t pr_x =  dz, pr_z = -dx;   /* right */

        if (blocked && d->steer_timer <= 0) {
            /* Newly blocked: probe BOTH sides and commit to one for a while so
               the dog follows the wall consistently instead of flip-flopping
               and drifting off (which sent it up the ramp before). */
            int32_t pl_dist = (pl_x < 0 ? -pl_x : pl_x) + (pl_z < 0 ? -pl_z : pl_z);
            int32_t pr_dist = (pr_x < 0 ? -pr_x : pr_x) + (pr_z < 0 ? -pr_z : pr_z);
            if (pl_dist == 0) pl_dist = 1;
            if (pr_dist == 0) pr_dist = 1;

            int32_t lx = d->x + (pl_x * DDOG_FEELER_LEN) / pl_dist;
            int32_t lz = d->z + (pl_z * DDOG_FEELER_LEN) / pl_dist;
            int32_t rx = d->x + (pr_x * DDOG_FEELER_LEN) / pr_dist;
            int32_t rz = d->z + (pr_z * DDOG_FEELER_LEN) / pr_dist;

            int32_t tlx = lx, tlz = lz;
            crates_collide(&tlx, d->y, &tlz, 80);
            apply_ddog_collision(&tlx, &tlz, d->on_upper_floor, d->on_ramp);
            int left_blocked = (tlx != lx || tlz != lz);

            int32_t trx = rx, trz = rz;
            crates_collide(&trx, d->y, &trz, 80);
            apply_ddog_collision(&trx, &trz, d->on_upper_floor, d->on_ramp);
            int right_blocked = (trx != rx || trz != rz);

            if (left_blocked && !right_blocked) {
                d->steer_dir = +1;
            } else if (right_blocked && !left_blocked) {
                d->steer_dir = -1;
            } else {
                /* Both open (or both blocked): pick the side whose probe ends
                   closer to the player, so the dog hugs the wall toward the
                   opening rather than away from it. */
                int32_t ld = (cam_x - lx < 0 ? lx - cam_x : cam_x - lx) +
                             (cam_z - lz < 0 ? lz - cam_z : cam_z - lz);
                int32_t rd = (cam_x - rx < 0 ? rx - cam_x : cam_x - rx) +
                             (cam_z - rz < 0 ? rz - cam_z : cam_z - rz);
                d->steer_dir = (ld <= rd) ? -1 : +1;
            }
            d->steer_timer = DDOG_STEER_COMMIT;
        }

        /* While committed, follow the chosen wall side regardless of small
           changes, so the dog slides past corners cleanly. */
        if (d->steer_timer > 0) {
            if (d->steer_dir < 0) { desired_x = pl_x; desired_z = pl_z; }
            else                  { desired_x = pr_x; desired_z = pr_z; }
            desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                           (desired_z < 0 ? -desired_z : desired_z);
            if (desired_dist == 0) desired_dist = 1;
            d->steer_timer--;
        }

        /* --- Smooth turning: blend new direction with previous (in facing) --- */
        int32_t move_x = (desired_x * DDOG_SPEED) / desired_dist;
        int32_t move_z = (desired_z * DDOG_SPEED) / desired_dist;
        int32_t prev_mx = (int16_t)(d->facing >> 16);
        int32_t prev_mz = (int16_t)(d->facing & 0xFFFF);
        int32_t blend_x = (prev_mx * (8 - DDOG_TURN_RATE) + move_x * DDOG_TURN_RATE) >> 3;
        int32_t blend_z = (prev_mz * (8 - DDOG_TURN_RATE) + move_z * DDOG_TURN_RATE) >> 3;
        d->facing = ((int32_t)(int16_t)blend_x << 16) | (uint16_t)(int16_t)blend_z;

        d->x += blend_x;
        d->z += blend_z;
        apply_ddog_collision(&d->x, &d->z, d->on_upper_floor, d->on_ramp);
        crates_collide(&d->x, d->y, &d->z, 80);
    }

    /* --- Dog vs dog hard collision (after every dog has moved) --- */
    int a, b;
    for (a = 0; a < demon_dog_count; a++) {
        DemonDog *da = &demon_dogs[a];
        if (!da->active || da->state == DDOG_DEAD) continue;
        for (b = a + 1; b < demon_dog_count; b++) {
            DemonDog *db = &demon_dogs[b];
            if (!db->active || db->state == DDOG_DEAD) continue;
            int32_t cdx  = da->x - db->x;
            int32_t cdz  = da->z - db->z;
            int32_t dist = (cdx < 0 ? -cdx : cdx) + (cdz < 0 ? -cdz : cdz);
            int32_t min_dist = DDOG_BODY_RADIUS * 2;
            if (dist < min_dist && dist > 0) {
                int32_t push    = (min_dist - dist) / 2;
                int32_t push_ax = (cdx * push) / dist;
                int32_t push_az = (cdz * push) / dist;
                da->x += push_ax; da->z += push_az;
                db->x -= push_ax; db->z -= push_az;
            }
        }
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
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;

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
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
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

/* NOTE: the dog sprites (VRAM Voff 128/192) and shadow (Voff 160) are drawn with
   NO texture-window bracket, so they are only correct where no 128x128 texture
   window is active — i.e. the delivery area (which resets the window to full).
   If demon dogs are ever placed in a windowed room (kitchen/reception), their
   sprites will wrap to the wrong texels; bracket each sprite with a full-window
   reset + restore like zombie.c's add_ft4_windowed. See tools/VRAM_MAP.txt. */
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
