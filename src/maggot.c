#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "collision.h"   /* apply_ddog_height only — a maggot is a ghost, like
                            the creep, and touches no wall or prop routine */
#include "particles.h"
#include "texmgr.h"
#include "sound.h"       /* SFX_BUZZ, and the player's SFX_HURT / SFX_DIE */
#include "crucifaxe.h"   /* SWING_RANGE, for maggots_try_hit */
#include "maggot.h"

/* Maggot — see maggot.h for how it differs from the creep, and src/creep.h for
   everything it shares with it. */

Maggot maggots[MAX_MAGGOTS];
int    maggot_count = 0;

/* The two-image sheet, one DEFERRED texmgr registration (TEXBANK_CATACOMBS).
   TEXMGR_MAX in src/texmgr.c is the cap and registering past it fails
   SILENTLY; count with py tools/heap_budget.py before adding another. */
static int maggot_tex = -1;

/* The area's texture window, restored after every sprite. Unlike the creep's
   this IS read — see maggots_set_texwindow() in maggot.h for why the slot
   needs it. */
static RECT mgt_tw_restore;
static int  mgt_tw_active = 0;

/* The swarm's single buzz voice: whether it is keyed, and frames until the
   C-side retrigger. One voice for every maggot, not one each (STEP 7b). */
static int mgt_buzz_on    = 0;
static int mgt_buzz_timer = 0;

/* Ticks once per update_maggots(); the flight cycle reads it. */
static uint32_t mgt_anim_tick = 0;

void maggots_set_texwindow(const RECT *tw) {
    if (tw) { mgt_tw_restore = *tw; mgt_tw_active = 1; }
    else    { mgt_tw_active = 0; }
}

void maggots_load_textures(void) {
    /* BANK: Chapter 3 only. py tools/check_tex_banks.py derives this from the
       uploader call graph (main.c's STATE_LOADING line) and fails if it is
       short. Widen it the day a maggot is placed outside the Catacombs. */
    texmgr_set_bank(TEXBANK_CATACOMBS);
    maggot_tex = texmgr_register("\\TEXCTCMB\\MAGGOT.TIM;1");
}

void maggots_upload_textures(void) {
    texmgr_upload(maggot_tex);
}

static void mgt_buzz_off(void) {
    if (mgt_buzz_on) sound_stop(SFX_BUZZ);
    mgt_buzz_on    = 0;
    mgt_buzz_timer = 0;
}

void maggots_init(void)  { maggot_count = 0; mgt_buzz_on = 0; mgt_buzz_timer = 0; }
void maggots_reset(void) { maggot_count = 0; mgt_buzz_off(); }

int maggot_spawn(int32_t x, int32_t z, int32_t anchor_y, GameState area) {
    if (maggot_count >= MAX_MAGGOTS) return -1;
    int i = maggot_count++;
    Maggot *m = &maggots[i];

    *m = (Maggot){0};
    m->x = x; m->z = z;
    m->y = anchor_y;              /* the floor ANCHOR — see maggot.h */
    m->float_off  = (int32_t)((uint32_t)rand() % (2u * MGT_FLOAT_VARY + 1u))
                  - MGT_FLOAT_VARY;
    m->anim_phase = (int)((uint32_t)rand() % (2u * MGT_ANIM_FRAMES));
    m->health = MGT_MAX_HEALTH;
    m->state  = MGT_ATTACKING;    /* no emergence: it attacks from this frame */
    m->active = 1;
    m->area   = area;
    return i;
}

int32_t maggot_body_y(const Maggot *m) {
    return m->y + MGT_Y_OFFSET + m->float_off;
}

int maggots_alive_in(GameState area) {
    int i, n = 0;
    for (i = 0; i < maggot_count; i++) {
        Maggot *m = &maggots[i];
        if (m->active && m->state != MGT_DEAD && m->area == area) n++;
    }
    return n;
}

/* EVERY TYPE AT 1x, listed rather than left to damage_scale()'s "no entry =
   unmodified" default, so the brief ("each weapon does 1x") is a fact readable
   here and not an absence. A new DamageType gets 1x by default too; add it
   here if it ever reaches this enemy. */
static const Weakness maggot_weakness[] = {
    { DMG_KINETIC, 100 },   /* Standard Rounds          */
    { DMG_FLAME,   100 },   /* Flame Rounds             */
    { DMG_HOLY,    100 },   /* the Helluminator's tick  */
};

int32_t maggot_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, maggot_weakness,
                        WEAKNESS_COUNT(maggot_weakness));
}

void maggot_damage(Maggot *m, int dmg) {
    if (!m->active || m->state == MGT_DEAD) return;
    m->health -= dmg;
    if (m->health <= 0) {
        m->health = 0;
        m->state  = MGT_DEAD;
        spawn_blood_burst(m->x, maggot_body_y(m), m->z);
        /* No death sound, the creep's call: the bank has ~13 KB left after
           the buzz. The buzz itself stops on the next update if this was the
           last of the swarm. */
    }
}

int maggots_try_hit(void) {
    int i;
    for (i = 0; i < maggot_count; i++) {
        Maggot *m = &maggots[i];
        if (!m->active || m->state == MGT_DEAD || m->area != current_area) continue;

        /* To the BODY, not the anchor, plus the half-extent so the reach is to
           its surface — creeps_try_hit()'s test. */
        int32_t dx     = m->x - cam_x;
        int32_t dy     = maggot_body_y(m) - cam_y;
        int32_t dz     = m->z - cam_z;
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
        if (dist3d >= SWING_RANGE + MGT_HALF_W) continue;

        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;

        maggot_damage(m, 1);
        return 1;
    }
    return 0;
}

void update_maggots(void) {
    static int hurt_sfx_cooldown = 0;
    int i, any = 0;
    if (hurt_sfx_cooldown > 0) hurt_sfx_cooldown--;
    mgt_anim_tick++;

    for (i = 0; i < maggot_count; i++) {
        Maggot *m = &maggots[i];
        /* current_area, NEVER game_state — or the inventory menu pauses the
           swarm (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!m->active || m->state == MGT_DEAD || m->area != current_area) continue;
        any = 1;

        if (m->damage_timer > 0) m->damage_timer--;

        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - m->x;
        int32_t dz = pz - m->z;

        /* Floor clamp on the anchor, every frame — the hover is measured from
           it (creep.h). Not gravity: nothing here falls. */
        apply_ddog_height(&m->x, &m->y, &m->z, &m->vy,
                          &m->on_upper_floor, &m->on_ramp);

        /* Vertical gap measured BODY to eye (mistake 14). */
        int32_t dy     = py - maggot_body_y(m);
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

        /* --- Bite, and poison. Horizontal and vertical reach tested
           SEPARATELY (STEP 4's bug, fixed three times). Each maggot runs its
           own timer, so the damage stacks across a swarm exactly as the
           creeps' does. player_poison() REFRESHES rather than stacks, so the
           slow lasts while any of them keeps biting and POISON_DURATION
           after the last bite. It is applied even under DBG_INFINITE_LIFE,
           which suppresses health loss only (STEP 4b). --- */
        if (!game_over && dist2d < MGT_CATCH_DIST &&
            (dy < 0 ? -dy : dy) < MGT_CATCH_DIST && m->damage_timer == 0) {
            m->damage_timer = MGT_DAMAGE_TICK;
            player_hurt(MGT_DAMAGE_AMOUNT);
            player_poison();
            if (hurt_sfx_cooldown == 0) {
                sound_play(SFX_HURT);
                hurt_sfx_cooldown = 30;
            }
            if (player_health <= 0) {
                player_health = 0;
                game_over     = 1;
                flash_timer   = 90;
                sound_play(SFX_DIE);
            }
        }

        /* --- Hold station on a true radius round the player. --- */
        if (dx * dx + dz * dz <= (int32_t)MGT_STOP_DIST * MGT_STOP_DIST) continue;

        /* --- Soft separation from the rest of the swarm. --- */
        int32_t sep_x = 0, sep_z = 0;
        int j;
        for (j = 0; j < maggot_count; j++) {
            if (j == i) continue;
            Maggot *o = &maggots[j];
            if (!o->active || o->state == MGT_DEAD || o->area != current_area) continue;
            int32_t odx   = m->x - o->x;
            int32_t odz   = m->z - o->z;
            int32_t odist = (odx < 0 ? -odx : odx) + (odz < 0 ? -odz : odz);
            if (odist < MGT_SEP_RADIUS && odist > 0) {
                int32_t push = MGT_SEP_RADIUS - odist;
                sep_x += (odx * push) / odist;
                sep_z += (odz * push) / odist;
            }
        }

        /* --- THE STEP: straight at the player, biased by separation. A ghost,
           like the creep — no feeler, no wall-follow, no collision call after
           it, and no heading blend (creep.h on mistake 16). --- */
        int32_t desired_x = dx + sep_x * MGT_SEP_WEIGHT;
        int32_t desired_z = dz + sep_z * MGT_SEP_WEIGHT;
        int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                               (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;

        m->x += (desired_x * MGT_SPEED) / desired_dist;
        m->z += (desired_z * MGT_SPEED) / desired_dist;
    }

    /* --- Hard separation after every body has moved. --- */
    int a, b;
    for (a = 0; a < maggot_count; a++) {
        Maggot *ma = &maggots[a];
        if (!ma->active || ma->state == MGT_DEAD || ma->area != current_area) continue;
        for (b = a + 1; b < maggot_count; b++) {
            Maggot *mb = &maggots[b];
            if (!mb->active || mb->state == MGT_DEAD || mb->area != current_area) continue;
            int32_t cdx  = ma->x - mb->x;
            int32_t cdz  = ma->z - mb->z;
            int32_t dist = (cdx < 0 ? -cdx : cdx) + (cdz < 0 ? -cdz : cdz);
            int32_t min_dist = MGT_BODY_RADIUS * 2;
            if (dist < min_dist && dist > 0) {
                int32_t push    = (min_dist - dist) / 2;
                int32_t push_ax = (cdx * push) / dist;
                int32_t push_az = (cdz * push) / dist;
                ma->x += push_ax; ma->z += push_az;
                mb->x -= push_ax; mb->z -= push_az;
            }
        }
    }

    /* --- THE BUZZ. One voice for the whole swarm, keyed while any maggot is
       alive in this room and re-keyed every MGT_BUZZ_FRAMES (the clip's
       length) — a C-side retrigger, NOT a hardware loop, which would poison
       voice 22 for everyone who borrows it (tools/ADDING_A_SOUND.txt STEP 6).

       Forced off on game_over: the area update stops running then, so a loop
       left keyed would otherwise hold for the rest of its clip on the
       game-over screen. Leaving the room silences it through maggots_reset()
       in world_leave(). --- */
    if (any && !game_over) {
        if (!mgt_buzz_on || --mgt_buzz_timer <= 0) {
            sound_play(SFX_BUZZ);
            mgt_buzz_on    = 1;
            mgt_buzz_timer = MGT_BUZZ_FRAMES;
        }
    } else {
        mgt_buzz_off();
    }
}

/* The area's 128 window wraps U, and this sheet's U is 128..255 (maggot.h), so
   every sprite is bracketed: restore pushed first, then the poly, then the
   full/unmasked window — OT buckets draw last-added first, so the GPU sees
   unmask, poly, restore. spider.c's add_ft4_windowed(), verbatim. */
static void add_ft4_windowed(RenderContext *ctx, int32_t otz, POLY_FT4 *poly) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;

    if (mgt_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &mgt_tw_restore);
        addPrim(&ot[otz], restore);
        ctx->next_packet += sizeof(DR_TWIN);

        addPrim(&ot[otz], poly);

        RECT full = { 0, 0, 0, 0 };   /* mask 0 = no wrapping, full page */
        DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
        setTexWindow(disable, &full);
        addPrim(&ot[otz], disable);
        ctx->next_packet += sizeof(DR_TWIN);
    } else {
        addPrim(&ot[otz], poly);
    }
}

/* The body quad: the creep's camera-facing billboard, no backface cull
   (mistake 4), with the flight cycle choosing which half of the sheet. */
static void draw_mgt_sprite(RenderContext *ctx, Maggot *m, int frame, int flip) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);

    int32_t cx = m->x, cz = m->z, cy = maggot_body_y(m);

    int16_t hw_x = (int16_t)((MGT_HALF_W * rx) >> 12);
    int16_t hw_z = (int16_t)((MGT_HALF_W * rz) >> 12);

    SVECTOR v[4];
    v[0].vx = (int16_t)(cx - hw_x); v[0].vy = (int16_t)(cy - MGT_HALF_H); v[0].vz = (int16_t)(cz - hw_z); v[0].pad = 0;
    v[1].vx = (int16_t)(cx + hw_x); v[1].vy = (int16_t)(cy - MGT_HALF_H); v[1].vz = (int16_t)(cz + hw_z); v[1].pad = 0;
    v[2].vx = (int16_t)(cx - hw_x); v[2].vy = (int16_t)(cy + MGT_HALF_H); v[2].vz = (int16_t)(cz - hw_z); v[2].pad = 0;
    v[3].vx = (int16_t)(cx + hw_x); v[3].vy = (int16_t)(cy + MGT_HALF_H); v[3].vz = (int16_t)(cz + hw_z); v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4];
    int32_t otz;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return;

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN)    otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1)  otz = OT_LENGTH - 2;

    int32_t fdx  = m->x - cam_x;
    int32_t fdz  = m->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    if (dist >= g_fog_far) return;          /* fully fogged: cull */
    int32_t fs   = render_fog_scale(dist);
    uint8_t fog8 = fs > 255 ? 255 : (uint8_t)fs;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, fog8, fog8, fog8);
    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
    poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;

    /* 128x64 4bpp at VRAM (672,320). The tpage is x640 y256, so:
         U = (672 - 640) * 4 = 128 for the sheet's left edge — image 0 is
             U 128..191 and image 1 is U 192..255;
         V = 320 - 256 = 64, so both images are V 64..127.
       Inset one texel on every edge, as every sprite here is, so a magnified
       edge does not sample its neighbour — which for image 0's right edge is
       image 1. */
    uint8_t u0 = (uint8_t)(128 + 64 * frame);
    uint8_t u_left  = flip ? (uint8_t)(u0 + 62) : (uint8_t)(u0 + 1);
    uint8_t u_right = flip ? (uint8_t)(u0 + 1)  : (uint8_t)(u0 + 62);
    poly->u0 = u_left;  poly->v0 =  65;
    poly->u1 = u_right; poly->v1 =  65;
    poly->u2 = u_left;  poly->v2 = 126;
    poly->u3 = u_right; poly->v3 = 126;

    poly->tpage = texmgr_tpage(maggot_tex);
    poly->clut  = texmgr_clut(maggot_tex);

    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, otz, poly);
}

void draw_maggots(RenderContext *ctx) {
    int i;
    for (i = 0; i < maggot_count; i++) {
        Maggot *m = &maggots[i];
        if (!m->active || m->state == MGT_DEAD || m->area != current_area) continue;

        int32_t dx = m->x - cam_x;
        int32_t dz = m->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 4000) continue;

        int frame = (int)(((mgt_anim_tick + (uint32_t)m->anim_phase)
                           / MGT_ANIM_FRAMES) & 1);

        /* Mirror toward the player. >>> THE ART FACES RIGHT, THE CREEP'S FACES
           LEFT, SO THE TEST IS THE OTHER WAY ROUND FROM draw_creeps(). <<< dot
           is the maggot's offset along the camera's right vector: positive
           means it is on the player's right and must face left, i.e. flip. */
        int32_t dot = ((dx >> 4) * (icos(cam_rot) >> 4))
                    - ((dz >> 4) * (isin(cam_rot) >> 4));
        draw_mgt_sprite(ctx, m, frame, dot > 0);
    }
}
