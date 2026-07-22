#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "demondog.h"       /* DDOG_DAMAGE_AMOUNT — same bite damage */
#include "crucifaxe.h"      /* SWING_RANGE */
#include "particles.h"
#include "tentacle.h"
#include "sound.h"

Tentacle tentacles[MAX_TENTACLES];
int      tentacle_count = 0;

static Tentacle tent_defaults[MAX_TENTACLES];

/* One shared looping writhe SFX plays while ANY tentacle is alert. */
static int wrath_on = 0;

/* Per-instance horizontal sprite flip (cosmetic; set in tentacles_init, not
   persisted since it never changes). 1 = mirror the sprite so it faces the
   opposite way. Indexed like the tentacles[] array. */
static int tent_flip[MAX_TENTACLES] = { 0 };

/* Ranges (horizontal Manhattan distance to the player). LIVE_RANGE > crucifaxe
   reach's safe zone: the player can strike from beyond DAMAGE_RANGE (the axe
   reaches ~350) without being hit, but stepping inside DAMAGE_RANGE gets them
   lashed. */
#define TENT_LIVE_RANGE      260   /* player within -> oscillates (a warning)     */
#define TENT_DAMAGE_RANGE    130   /* player within -> lashes out and damages     */
#define TENT_DAMAGE_COOLDOWN  45   /* frames between hits (~0.75s)                */
#define TENT_KNOCKBACK        90   /* player push-back speed on a hit             */
#define TENT_OSC_RATE         30   /* frames per idle/active sprite swap (~0.5s)  */

/* Billboard sizes (world units, half-extents). The active sprite is wider
   (128x64 vs the idle 64x64). Bottom rests on the floor. */
#define TENT_IDLE_HALF_W     131   /* ~75% larger than the original 75 */
#define TENT_HALF_H          149   /* ~75% larger than the original 85 */
#define TENT_ACTV_HALF_W     262   /* ~75% larger than the original 150 */

/* Light-green blood (tentacles bleed green, not red). */
#define TENT_BLOOD_R         150
#define TENT_BLOOD_G         230
#define TENT_BLOOD_B         130

/* Per-sprite VRAM handles, filled at startup. */
typedef struct { uint16_t tpage, clut; uint8_t u0, v0, u1, v1; } Sprite;
static Sprite spr_idle, spr_actv;
static int    tex_loaded = 0;

/* Texture window the area expects, restored around each sprite. The sprites sit
   at VRAM u-offset 128 within their page, so a room's 128 window would wrap
   their U to the wrong texels — bracket each with a full-window reset + restore
   (as the demon dogs do). Set by tentacles_set_texwindow(); off by default. */
static RECT tent_tw_restore;
static int  tent_tw_active = 0;

void tentacles_set_texwindow(const RECT *tw) {
    if (tw) { tent_tw_restore = *tw; tent_tw_active = 1; }
    else    { tent_tw_active = 0; }
}

/* Add an already-filled POLY_FT4 (packet space reserved) to ot[otz], bracketing
   it with a full/unmasked window then the area's restore window when active. */
static void add_ft4_windowed(RenderContext *ctx, int32_t otz, POLY_FT4 *poly) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;
    if (tent_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &tent_tw_restore);
        addPrim(&ot[otz], restore);
        ctx->next_packet += sizeof(DR_TWIN);
        addPrim(&ot[otz], poly);
        RECT full = { 0, 0, 0, 0 };
        DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
        setTexWindow(disable, &full);
        addPrim(&ot[otz], disable);
        ctx->next_packet += sizeof(DR_TWIN);
    } else {
        addPrim(&ot[otz], poly);
    }
}

static void load_sprite(const char *filename, Sprite *s) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return;
    int   sectors = (file.size + 2047) / 2048;
    void *buf     = malloc(sectors * 2048);
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
        s->clut = getClut(tim.crect->x, tim.crect->y);
    }
    s->tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int tex_w    = tim.prect->w * px_mult;
    int tex_h    = tim.prect->h;
    int u_off    = (tim.prect->x & 63) * px_mult;
    s->u0 = (uint8_t)u_off;
    s->v0 = (uint8_t)(tim.prect->y % 256);
    s->u1 = (uint8_t)(u_off + tex_w - 1);
    s->v1 = (uint8_t)(s->v0 + tex_h - 1);
    free(buf);
}

void tentacles_load_assets(void) {
    load_sprite("\\TEX\\TNTCLIDL.TIM;1", &spr_idle);
    load_sprite("\\TEX\\TNTCLACT.TIM;1", &spr_actv);
    if (spr_idle.tpage) tex_loaded = 1;
}

static void add_tentacle(int32_t x, int32_t y, int32_t z, GameState area) {
    if (tentacle_count >= MAX_TENTACLES) return;
    Tentacle *t = &tentacles[tentacle_count++];
    t->x = x; t->y = y; t->z = z;
    t->health = TENTACLE_MAX_HEALTH;
    t->active = 1;
    t->anim = 0; t->lively = 0;
    t->damage_timer = 0; t->hit_timer = 0;
    t->area = area;
}

void tentacles_init(void) {
    tentacle_count = 0;

    /* Two in the conservatory, guarding the approach to the copper pot (CP at
       x=-1000, z=1600, pickup radius 220). Placed further out than the pot's
       pickup radius and onto the south/southwest funnel, so the player must pass
       through at least one tentacle's danger zone to reach the pot. Floor y=-149. */
    add_tentacle(-1000, -149, 1250, STATE_CONSERVATORY);   /* south approach     */
    add_tentacle(-1150, -149, 1350, STATE_CONSERVATORY);   /* between the two    */
    add_tentacle(-1300, -149, 1450, STATE_CONSERVATORY);   /* southwest approach */

    /* The east-most tentacle (x=-1000, added first) faces the opposite way from
       the rest — mirror its sprite. */
    tent_flip[0] = 1;

    int i;
    for (i = 0; i < tentacle_count; i++)
        tent_defaults[i] = tentacles[i];
}

void tentacles_reset(void) {
    int i;
    for (i = 0; i < tentacle_count; i++)
        tentacles[i] = tent_defaults[i];
    if (wrath_on) { sound_stop(SFX_TNTCL_WRTH); wrath_on = 0; }
}

void update_tentacles(void) {
    int i, any_lively = 0;
    for (i = 0; i < tentacle_count; i++) {
        Tentacle *t = &tentacles[i];
        if (!t->active || t->health <= 0) continue;
        if (t->area != game_state) continue;

        if (t->hit_timer    > 0) t->hit_timer--;
        if (t->damage_timer > 0) t->damage_timer--;

        int32_t dx   = cam_x - t->x;
        int32_t dz   = cam_z - t->z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

        t->lively = (dist < TENT_LIVE_RANGE);
        if (t->lively) { t->anim++; any_lively = 1; }

        /* Lash out when the player is too close (and not on cooldown). */
        if (!game_over && dist < TENT_DAMAGE_RANGE && t->damage_timer == 0) {
            t->damage_timer = TENT_DAMAGE_COOLDOWN;
            player_health  -= DDOG_DAMAGE_AMOUNT;
            player_knockback(t->x, t->z, TENT_KNOCKBACK);
            sound_play(SFX_HURT);
            if (player_health <= 0) {
                player_health = 0;
                game_over     = 1;
                flash_timer   = 90;
            }
        }
    }

    /* Start/stop the shared looping writhe. Force it off on game-over (the area
       update stops running then, so it wouldn't otherwise be keyed off). */
    if (game_over) any_lively = 0;
    if (any_lively && !wrath_on)      { sound_play(SFX_TNTCL_WRTH); wrath_on = 1; }
    else if (!any_lively && wrath_on) { sound_stop(SFX_TNTCL_WRTH); wrath_on = 0; }
}

/* Apply `amount` damage to a tentacle (never knocked back — tentacles don't
   move). Green blood + death sound on a kill, axe-hit on a non-fatal hit. */
static void tentacle_take_damage(Tentacle *t, int amount) {
    t->health   -= amount;
    t->hit_timer = 30;
    if (t->health <= 0) {
        /* Light-green blood spray at mid-body height (as the other enemies). */
        spawn_burst(t->x, t->y + GROUND_FLOOR_Y - TENT_HALF_H, t->z,
                    TENT_BLOOD_R, TENT_BLOOD_G, TENT_BLOOD_B);
        sound_play(SFX_TNTCL_DIE);
        /* If this was the last alert tentacle the writhe stops next update. */
    } else {
        sound_play(SFX_AXEHIT);
    }
}

/* Aim target for the gun's hitscan: sprite centre height and half-height. */
void tentacle_body(const Tentacle *t, int32_t *cyc, int32_t *hh) {
    *cyc = (t->y + GROUND_FLOOR_Y) - TENT_HALF_H;
    *hh  = TENT_HALF_H;
}

/* A grave-olver round deals 1 HP. */
void tentacle_shoot(Tentacle *t) {
    tentacle_take_damage(t, 1);
}

int tentacles_try_hit(void) {
    int i;
    for (i = 0; i < tentacle_count; i++) {
        Tentacle *t = &tentacles[i];
        if (!t->active || t->health <= 0) continue;
        if (t->area != game_state) continue;

        /* Strike point at mid-sprite height (bottom on floor + half height). */
        int32_t hit_y  = (t->y + GROUND_FLOOR_Y) - TENT_HALF_H;
        int32_t dx     = t->x - cam_x;
        int32_t dy     = hit_y - cam_y;
        int32_t dz     = t->z - cam_z;
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
        if (dist3d >= SWING_RANGE) continue;

        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;

        tentacle_take_damage(t, 1);
        return 1;
    }
    return 0;
}

/* Emit one camera-facing billboard for tentacle `t` using sprite `s`, `half_w`
   wide. Bottom edge rests on the floor. */
static void draw_billboard(RenderContext *ctx, Tentacle *t, const Sprite *s,
                           int32_t half_w, int flip) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) + 2 * sizeof(DR_TWIN) > buf_end) return;

    int32_t dx = t->x - cam_x;
    int32_t dz = t->z - cam_z;
    int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (wdist >= g_fog_far) return;
    if (((dx * isin(cam_rot) + dz * icos(cam_rot)) >> 12) <= 0) return;  /* behind camera */

    /* Sprite centre: horizontally on the tentacle, vertically one half-height
       above the floor so the bottom edge sits on it. */
    int32_t floor_y = t->y + GROUND_FLOOR_Y;
    int32_t cy      = floor_y - TENT_HALF_H;

    /* Build the billboard as a WORLD-space quad along the camera's right axis and
       project its corners, so the GTE gives true perspective scaling (like the
       dogs). The old screen-space size (half_w * 256 / Manhattan-dist) made the
       sprite grow/shrink as the player circled it at a constant distance. */
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);
    int16_t dwx = (int16_t)((half_w * rx) >> 12);
    int16_t dwz = (int16_t)((half_w * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - TENT_HALF_H);
    int16_t vy_bot = (int16_t)(cy + TENT_HALF_H);

    SVECTOR v[4];
    v[0].vx = (int16_t)(t->x - dwx); v[0].vy = vy_top; v[0].vz = (int16_t)(t->z - dwz); v[0].pad = 0;
    v[1].vx = (int16_t)(t->x + dwx); v[1].vy = vy_top; v[1].vz = (int16_t)(t->z + dwz); v[1].pad = 0;
    v[2].vx = (int16_t)(t->x + dwx); v[2].vy = vy_bot; v[2].vz = (int16_t)(t->z + dwz); v[2].pad = 0;
    v[3].vx = (int16_t)(t->x - dwx); v[3].vy = vy_bot; v[3].vz = (int16_t)(t->z - dwz); v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4], otz;
    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz4c(sz);
    if (!sz[1] || !sz[2] || !sz[3]) return;
    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz > OT_LENGTH - 2)  otz = OT_LENGTH - 2;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    {   /* distance fog to match the room mesh; flash red on a fresh hit */
        uint8_t fc = (uint8_t)((128 * render_fog_scale(wdist)) >> 8);
        if (t->hit_timer > 0) setRGB0(poly, fc, fc >> 2, fc >> 2);
        else                  setRGB0(poly, fc, fc, fc);
    }
    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;
    /* Horizontal flip swaps the left/right U columns. */
    uint8_t ul = flip ? s->u1 : s->u0;
    uint8_t ur = flip ? s->u0 : s->u1;
    poly->u0 = ul; poly->v0 = s->v0;
    poly->u1 = ur; poly->v1 = s->v0;
    poly->u2 = ul; poly->v2 = s->v1;
    poly->u3 = ur; poly->v3 = s->v1;
    poly->clut  = s->clut;
    poly->tpage = s->tpage;
    /* Reserve before the bracket may allocate DR_TWINs (sprite sits at u-off 128
       so a room's 128 window wraps it without the bracket). */
    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, otz, poly);
}

void draw_tentacles(RenderContext *ctx) {
    if (!tex_loaded) return;
    int i;
    for (i = 0; i < tentacle_count; i++) {
        Tentacle *t = &tentacles[i];
        if (!t->active || t->health <= 0) continue;
        if (t->area != game_state) continue;

        /* Idle by default; while the player is in range, oscillate between the
           idle and active sprites. */
        int show_active = t->lively && ((t->anim / TENT_OSC_RATE) & 1);
        if (show_active) draw_billboard(ctx, t, &spr_actv, TENT_ACTV_HALF_W, tent_flip[i]);
        else             draw_billboard(ctx, t, &spr_idle, TENT_IDLE_HALF_W, tent_flip[i]);
    }
}
