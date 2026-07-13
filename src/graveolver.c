#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "title.h"
#include "player.h"
#include "collision.h"
#include "demondog.h"
#include "zombie.h"
#include "vampire.h"
#include "particles.h"
#include "sound.h"
#include "graveolver.h"
#include "weapon.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

#define GRAV_FIRE_COOLDOWN 12   /* frames between shots (revolver cadence) */

/* Hitscan aim. A target is "under the reticule" if, projected onto the camera's
   forward axis, its perpendicular offset stays within a tolerance that grows
   with distance (an aim cone), and nothing solid blocks the line to it. */
#define GUN_RANGE       4000
#define GUN_AIM_BASE      70   /* horizontal tolerance at point-blank        */
#define GUN_AIM_VBASE    150   /* vertical tolerance (enemies are tall)       */
#define GUN_AIM_SPREAD   300   /* cone growth per unit of depth (/4096)       */
#define GUN_DAMAGE         1   /* one crucifaxe hit                           */
#define GUN_FLASH_FRAMES   4   /* white screen-flash duration                */
#define GUN_INFINITE_AMMO  1   /* 1 = fire freely without spending rounds     */

/* Front OT layers for the screen-space overlays (lower = nearer the front;
   HUD owns 0/1, scene/weapon are >=16, so these sit between). */
#define OT_GUN_FLASH     2
#define OT_GUN_RETICULE  3
#define GUN_RETICULE_Y_OFF 20   /* pixels below screen centre (~an inch) */

static SMD  *graveolver_smd  = NULL;
static void *graveolver_buff = NULL;
static int   muzzle_flash     = 0;

/* --- Hold pose (view space), all easily tunable ------------------------------
   The model's long axis is X (the barrel), so a ~90 deg yaw points it into the
   screen. Position is an offset from the view centre: +X = right, +Y = down,
   +Z = forward (a larger Z shrinks the on-screen size). */
#define GRAV_VS_X    65
#define GRAV_VS_Y    70
#define GRAV_VS_Z   170
#define GRAV_ROT_X    0
#define GRAV_ROT_Y 741    /* yaw: barrel hold angle */
#define GRAV_ROT_Z    0
/* The model's base colours are very dark; brighten them (4096 = 1.0x). */
#define GRAV_BRIGHTNESS 20480   /* 5.0x */

void graveolver_init(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\GRAVOLVR.SMD;1")) return;
    int sectors     = (file.size + 2047) / 2048;
    graveolver_buff = malloc(sectors * 2048);
    if (!graveolver_buff) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)graveolver_buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    graveolver_smd = smdInitData(graveolver_buff);
}

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

/* --- enemy damage (mirrors the crucifaxe's per-enemy handling, no knockback) --- */

static void damage_dog(DemonDog *d) {
    d->health   -= GUN_DAMAGE;
    d->hit_timer = DDOG_BAR_TIMER_MAX;
    if (d->health <= 0) {
        d->state = DDOG_DEAD;
        spawn_blood_burst(d->x, d->y, d->z);
        sound_play(SFX_DOGDIE);
    } else {
        sound_play(SFX_AXEHIT);
    }
}

static void damage_zombie(Zombie *z) {
    z->health   -= GUN_DAMAGE;
    z->hit_timer = ZMB_BAR_TIMER_MAX;
    if (z->health <= 0) {
        z->state = ZMB_DEAD;
        spawn_blood_burst(z->x, z->y, z->z);
        sound_stop(SFX_ZOMBIE);
        sound_play(SFX_ZOMBIEDIE);
    } else {
        sound_play(SFX_AXEHIT);
    }
}

/* Line-of-sight: sample points between the camera and the target; blocked if a
   wall or solid prop lies in the way (so you can't shoot through walls). */
static int los_clear(int32_t dx, int32_t dy, int32_t dz) {
    const int steps = 8;
    int k;
    for (k = 1; k < steps; k++) {
        int32_t sx = cam_x + (dx * k) / steps;
        int32_t sy = cam_y + (dy * k) / steps;
        int32_t sz = cam_z + (dz * k) / steps;
        if (collision_point_blocked(sx, sy, sz, 8)) return 0;
    }
    return 1;
}

/* 1 if (ex,ey,ez) is under the reticule, in range, and unobscured; out_depth
   receives its forward distance so the caller can pick the nearest target. */
static int under_reticule(int32_t ex, int32_t ey, int32_t ez,
                          int32_t fx, int32_t fz, int32_t *out_depth) {
    int32_t dx = ex - cam_x, dy = ey - cam_y, dz = ez - cam_z;
    int32_t depth = (dx * fx + dz * fz) >> 12;         /* along the aim ray */
    if (depth <= 0 || depth > GUN_RANGE) return 0;
    int32_t cone = ((depth * GUN_AIM_SPREAD) >> 12);
    int32_t perp = iabs32((dx * fz - dz * fx) >> 12);  /* off-axis (horizontal) */
    if (perp > GUN_AIM_BASE + cone) return 0;
    /* The crosshair sits GUN_RETICULE_Y_OFF pixels below screen centre; project
       that back to a world-Y drop at this depth (screen_y = world_y * h / depth,
       h = 256 from gte_SetGeomScreen) so the aim point tracks the crosshair. */
    int32_t aim_dy = (depth * GUN_RETICULE_Y_OFF) / 256;
    if (iabs32(dy - aim_dy) > GUN_AIM_VBASE + cone) return 0;
    if (!los_clear(dx, dy, dz)) return 0;
    *out_depth = depth;
    return 1;
}

/* Fire one round: flash + hitscan the nearest enemy under the reticule. Returns
   1 if a round was spent. */
static int graveolver_fire(void) {
#if !GUN_INFINITE_AMMO
    if (player_rounds <= 0) return 0;
    player_rounds--;
#endif
    muzzle_flash = GUN_FLASH_FRAMES;

    int32_t fx = isin(cam_rot), fz = icos(cam_rot);
    int      best_kind = -1, best_idx = -1;
    int32_t  best_depth = 0x7fffffff, depth;
    int i;

    for (i = 0; i < demon_dog_count; i++) {
        DemonDog *d = &demon_dogs[i];
        if (!d->active || d->state == DDOG_DEAD) continue;
        if (under_reticule(d->x, d->y, d->z, fx, fz, &depth) && depth < best_depth) {
            best_depth = depth; best_kind = 0; best_idx = i;
        }
    }
    for (i = 0; i < zombie_count; i++) {
        Zombie *z = &zombies[i];
        if (!z->active || z->state == ZMB_DEAD) continue;
        if (under_reticule(z->x, z->y, z->z, fx, fz, &depth) && depth < best_depth) {
            best_depth = depth; best_kind = 1; best_idx = i;
        }
    }
    if (vampire_health > 0 &&
        under_reticule(vampire_x, vampire_y, vampire_z, fx, fz, &depth) &&
        depth < best_depth) {
        best_kind = 2;
    }

    if (best_kind == 0) {
        damage_dog(&demon_dogs[best_idx]);
    } else if (best_kind == 1) {
        damage_zombie(&zombies[best_idx]);
    } else if (best_kind == 2) {
        vampire_health   -= GUN_DAMAGE;
        vampire_hit_timer = VAMPIRE_BAR_TIMER_MAX;
        if (vampire_health <= 0)
            spawn_blood_burst(vampire_x, vampire_y, vampire_z);
    }
    return 1;
}

void graveolver_update(void) {
    /* Edge-detect Square so one press fires once; a short cooldown paces taps. */
    static int square_prev = 0;
    static int cooldown    = 0;
    if (cooldown > 0)    cooldown--;
    if (muzzle_flash > 0) muzzle_flash--;

    int square_held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        square_held = (~pad->btn & PAD_SQUARE) ? 1 : 0;
    }
    int square_just = square_held && !square_prev;
    square_prev = square_held;

    if (game_state != STATE_MENU && square_just && cooldown == 0) {
        if (graveolver_fire())
            cooldown = GRAV_FIRE_COOLDOWN;
    }
}

/* Screen-space filled rect helper (2D, no GTE). */
static void screen_tile(RenderContext *ctx, int x, int y, int w, int h,
                        uint8_t r, uint8_t g, uint8_t b, int ot) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *t = (TILE *)ctx->next_packet;
    setTile(t);
    setRGB0(t, r, g, b);
    setXY0(t, x, y);
    setWH(t, w, h);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot], t);
    ctx->next_packet += sizeof(TILE);
}

void draw_graveolver(RenderContext *ctx) {
    if (!graveolver_smd) return;

    /* The held model. */
    SVECTOR rot = {GRAV_ROT_X, GRAV_ROT_Y, GRAV_ROT_Z, 0};
    MATRIX  weapon_vs;
    RotMatrix(&rot, &weapon_vs);
    weapon_vs.t[0] = GRAV_VS_X;
    weapon_vs.t[1] = GRAV_VS_Y;
    weapon_vs.t[2] = GRAV_VS_Z;
    weapon_render_model(ctx, graveolver_smd, &weapon_vs, GRAV_BRIGHTNESS);

    /* Overlays are hidden behind the inventory menu, so skip them there. */
    if (game_state == STATE_MENU) return;

    /* Aiming reticule: a white cross with a centre gap, nudged below centre. */
    {
        int cx = SCREEN_XRES / 2, cy = SCREEN_YRES / 2 + GUN_RETICULE_Y_OFF;
        screen_tile(ctx, cx - 14, cy - 1, 8, 2, 255, 255, 255, OT_GUN_RETICULE);
        screen_tile(ctx, cx +  6, cy - 1, 8, 2, 255, 255, 255, OT_GUN_RETICULE);
        screen_tile(ctx, cx - 1, cy - 14, 2, 8, 255, 255, 255, OT_GUN_RETICULE);
        screen_tile(ctx, cx - 1, cy +  6, 2, 8, 255, 255, 255, OT_GUN_RETICULE);
    }

    /* Muzzle flash: a brief semi-transparent white wash over the whole screen.
       The TILE is added first and the DR_TPAGE (abr=0, 50% blend) last so the
       GPU processes the blend mode before the tile (LIFO within the OT node). */
    if (muzzle_flash > 0) {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(TILE) <= buf_end) {
            TILE *t = (TILE *)ctx->next_packet;
            setTile(t);
            setSemiTrans(t, 1);
            setRGB0(t, 255, 255, 255);
            setXY0(t, 0, 0);
            setWH(t, SCREEN_XRES, SCREEN_YRES);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_GUN_FLASH], t);
            ctx->next_packet += sizeof(TILE);
        }
        if (ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
            DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
            setDrawTPage(dp, 0, 0, getTPage(0, 0, 0, 0));
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_GUN_FLASH], dp);
            ctx->next_packet += sizeof(DR_TPAGE);
        }
    }
}
