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
#include "bullet_hit.h"
#include "graveolver.h"
#include "weapon.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

#define GRAV_FIRE_COOLDOWN 12   /* frames between shots (revolver cadence) */

/* Hitscan aim, screen-space. Picture a fixed circle around the crosshair: a shot
   hits the CLOSEST thing whose on-screen silhouette falls inside that circle.
   Depth and height don't widen the aim — a constant pixel radius at any range —
   they only decide which candidate is nearer. An enemy is a candidate when any
   part of its body projects inside the circle AND the crosshair line to its
   depth isn't blocked by a nearer wall/prop. */
#define GUN_RANGE        4000  /* max forward distance a shot reaches           */
#define GUN_AIM_RADIUS     14  /* crosshair hit circle, in screen pixels        */
#define GUN_PROJ_H        256  /* projection distance — matches gte_SetGeomScreen*/
#define GUN_DAMAGE         1   /* one crucifaxe hit                           */
#define GUN_FLASH_FRAMES   4   /* white screen-flash duration                */
#define GUN_HIT_BACKOFF   30   /* pull the hit sprite toward the camera a bit */
#define GRAV_RELOAD_FRAMES 180 /* 3 s at 60 fps                               */
#define GRAV_RELOAD_DROP  260  /* view-space Y the model drops off-screen     */
#define GRAV_RELOAD_PITCH 800  /* muzzle-down tilt at full reload dip (angle)  */
#define GRAV_RECOIL_FRAMES  7  /* recoil kick duration in frames              */
#define GRAV_RECOIL_PITCH 320  /* muzzle-up tilt at the instant of firing      */
#define GRAV_AIM_YAW      110  /* model yaw per 100px of LEFT aim offset         */
#define GRAV_AIM_YAW_R    420  /* stronger yaw per 100px of RIGHT offset: the
                                  rest pose already angles left, so aiming right
                                  needs extra rotation to compensate            */
#define GRAV_AIM_PITCH    110  /* model pitch per 100px of vertical aim offset  */

/* Front OT layers for the screen-space overlays (lower = nearer the front;
   HUD owns 0/1, scene/weapon are >=16, so these sit between). */
#define OT_GUN_FLASH     2
#define OT_GUN_RETICULE  3

static SMD  *graveolver_smd  = NULL;
static void *graveolver_buff = NULL;
static int   muzzle_flash     = 0;
static int   reload_timer     = 0;   /* counts down GRAV_RELOAD_FRAMES while reloading */
static int   recoil_timer     = 0;   /* counts down GRAV_RECOIL_FRAMES after a shot   */

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

/* Crosshair centre in screen pixels — its live position (moves while aiming). */
static int gun_crosshair_x(void) { return aim_x; }
static int gun_crosshair_y(void) { return aim_y; }

/* World point along the crosshair line at forward distance `depth`. Back-projects
   the crosshair's screen offset from centre into view X (perp) and view Y, then
   places it: forward*depth + right*viewX, with viewY straight down. Generalises
   the aim ray for an off-centre (aimed) crosshair. */
static void crosshair_ray_point(int32_t fx, int32_t fz, int32_t depth,
                                int32_t *px, int32_t *py, int32_t *pz) {
    int32_t view_x = ((aim_x - SCREEN_XRES / 2) * depth) / GUN_PROJ_H;
    int32_t view_y = ((aim_y - SCREEN_YRES / 2) * depth) / GUN_PROJ_H;
    *px = cam_x + ((fx * depth) >> 12) + ((fz * view_x) >> 12);   /* right = (fz,-fx) */
    *pz = cam_z + ((fz * depth) >> 12) - ((fx * view_x) >> 12);
    *py = cam_y + view_y;
}

/* Squared pixel distance from the crosshair to the screen-space segment A-B. */
static int32_t crosshair_seg_dist2(int ax, int ay, int bx, int by) {
    int px = gun_crosshair_x() - ax, py = gun_crosshair_y() - ay;
    int dx = bx - ax,               dy = by - ay;
    int len2 = dx * dx + dy * dy;
    int qx, qy;
    if (len2 <= 0) {
        qx = ax; qy = ay;                         /* degenerate: A==B */
    } else {
        int32_t t = ((int32_t)(px * dx + py * dy) << 12) / len2;   /* fixed 0..4096 */
        if (t < 0)    t = 0;
        if (t > 4096) t = 4096;
        qx = ax + ((dx * t) >> 12);
        qy = ay + ((dy * t) >> 12);
    }
    int ex = gun_crosshair_x() - qx, ey = gun_crosshair_y() - qy;
    return ex * ex + ey * ey;
}

/* Project a world point to screen pixels by hand (no GTE state needed, so this
   is safe in the update phase where firing runs). Rotation is Y-only, so the
   view X is the point's perpendicular offset from the aim axis and the view Z is
   its forward depth; the perspective divide uses the renderer's H and centre. */
static int project_world(int32_t x, int32_t y, int32_t z,
                         int32_t fx, int32_t fz, int *sx, int *sy) {
    int32_t dx = x - cam_x, dy = y - cam_y, dz = z - cam_z;
    int32_t depth = (dx * fx + dz * fz) >> 12;        /* view Z (forward) */
    if (depth <= 0) return 0;
    int32_t vx = (dx * fz - dz * fx) >> 12;           /* view X (perp)    */
    *sx = SCREEN_XRES / 2 + (vx * GUN_PROJ_H) / depth;
    *sy = SCREEN_YRES / 2 + (dy * GUN_PROJ_H) / depth;
    return 1;
}

/* 1 if the enemy's body silhouette passes within the crosshair circle. The body
   is the sprite's vertical extent (centre cyc, half-height hh); we project its
   top and bottom and measure the crosshair's distance to that on-screen line, so
   there are no gaps and it works at any range. out_depth = forward distance for
   nearest-first ordering. */
static int enemy_in_circle(int32_t ex, int32_t cyc, int32_t ez, int32_t hh,
                           int32_t fx, int32_t fz, int32_t *out_depth) {
    int32_t depth = ((ex - cam_x) * fx + (ez - cam_z) * fz) >> 12;
    if (depth <= 0 || depth > GUN_RANGE) return 0;

    int tx, ty, bx, by;
    if (!project_world(ex, cyc - hh, ez, fx, fz, &tx, &ty)) return 0;
    if (!project_world(ex, cyc + hh, ez, fx, fz, &bx, &by)) return 0;

    if (crosshair_seg_dist2(tx, ty, bx, by)
            <= (int32_t)GUN_AIM_RADIUS * GUN_AIM_RADIUS) {
        *out_depth = depth;
        return 1;
    }
    return 0;
}

/* 1 if the crosshair line is clear out to `depth` — i.e. no wall or solid prop
   sits nearer than the target under the crosshair (so a closer table blocks the
   shot even when the enemy is still inside the circle). */
static int crosshair_clear(int32_t fx, int32_t fz, int32_t depth) {
    int32_t px, py, pz;
    crosshair_ray_point(fx, fz, depth, &px, &py, &pz);
    return !collision_segment_blocked(cam_x, cam_y, cam_z, px, py, pz);
}

/* Fire one round: flash + hitscan the nearest enemy under the reticule. The
   caller has already confirmed a round is chambered and spends it. */
static void graveolver_fire(void) {
    muzzle_flash = GUN_FLASH_FRAMES;
    recoil_timer = GRAV_RECOIL_FRAMES;
    sound_play(SFX_GR_SHOT);

    int32_t fx = isin(cam_rot), fz = icos(cam_rot);
    int      best_kind = -1, best_idx = -1;
    int32_t  best_depth = 0x7fffffff, depth;
    int i;

    for (i = 0; i < demon_dog_count; i++) {
        DemonDog *d = &demon_dogs[i];
        if (!d->active || d->state == DDOG_DEAD) continue;
        if (enemy_in_circle(d->x, d->y + DDOG_Y_OFFSET, d->z, DDOG_HALF_H, fx, fz, &depth) &&
            depth < best_depth && crosshair_clear(fx, fz, depth)) {
            best_depth = depth; best_kind = 0; best_idx = i;
        }
    }
    for (i = 0; i < zombie_count; i++) {
        Zombie *z = &zombies[i];
        if (!z->active || z->state == ZMB_DEAD) continue;
        if (enemy_in_circle(z->x, z->y + ZMB_Y_OFFSET, z->z, ZMB_HALF_H, fx, fz, &depth) &&
            depth < best_depth && crosshair_clear(fx, fz, depth)) {
            best_depth = depth; best_kind = 1; best_idx = i;
        }
    }
    if (vampire_health > 0 &&
        enemy_in_circle(vampire_x, vampire_y + VAMPIRE_Y, vampire_z, VAMPIRE_HALF_H,
                        fx, fz, &depth) &&
        depth < best_depth && crosshair_clear(fx, fz, depth)) {
        best_kind = 2;
    }

    /* Nothing hittable in the circle (or a wall/prop was the nearest thing under
       it): no damage, no impact sprite — ghit only marks enemy hits. */
    if (best_kind < 0)
        return;

    if (best_kind == 0) {
        damage_dog(&demon_dogs[best_idx]);
    } else if (best_kind == 1) {
        damage_zombie(&zombies[best_idx]);
    } else {
        vampire_health   -= GUN_DAMAGE;
        vampire_hit_timer = VAMPIRE_BAR_TIMER_MAX;
        if (vampire_health <= 0)
            spawn_blood_burst(vampire_x, vampire_y, vampire_z);
    }

    /* Impact sprite on the struck enemy, pulled a touch toward the camera so it
       sits in front of the body rather than inside it. */
    {
        int32_t d = best_depth - GUN_HIT_BACKOFF;
        if (d < 1) d = 1;
        int32_t hx, hy, hz;
        crosshair_ray_point(fx, fz, d, &hx, &hy, &hz);
        bullet_hit_spawn(hx, hy, hz);
    }
}

int graveolver_is_reloading(void) {
    return reload_timer > 0;
}

void graveolver_update(void) {
    /* Edge-detect Square so one press fires once; a short cooldown paces taps. */
    static int square_prev = 0;
    static int cooldown    = 0;
    if (cooldown > 0)    cooldown--;
    if (muzzle_flash > 0) muzzle_flash--;
    if (recoil_timer > 0) recoil_timer--;

    int square_held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        square_held = (~pad->btn & PAD_SQUARE) ? 1 : 0;
    }
    int square_just = square_held && !square_prev;
    square_prev = square_held;

    /* A reload is running: count it down and, when finished, top the cylinder up
       from the reserve. No firing until it completes. */
    if (reload_timer > 0) {
        reload_timer--;
        if (reload_timer == 0) {
            int need = GRAVEOLVER_CAPACITY - graveolver_loaded;
            int take = need < player_rounds ? need : player_rounds;
            graveolver_loaded += take;
            player_rounds     -= take;
        }
        return;
    }

    if (game_state == STATE_MENU || !square_just || cooldown != 0)
        return;

    if (graveolver_loaded > 0) {
        graveolver_fire();
        graveolver_loaded--;
        cooldown = GRAV_FIRE_COOLDOWN;
    } else if (player_rounds > 0) {
        /* Empty cylinder + trigger pull with rounds in reserve: start reloading. */
        reload_timer = GRAV_RELOAD_FRAMES;
        sound_play(SFX_GR_RELOAD);
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

    /* Reload dip: over the reload the model drops off the bottom of the screen
       (first third), stays down (middle third), then rises back (last third).
       +Y is down in view space, so add the drop to the hold-pose Y. The weapon-
       switch slide feeds the same `drop`, so switching in/out mirrors the reload
       motion (dip + tilt) — just without the reload sound. */
    int32_t drop = weapon_switch_offset();
    if (reload_timer > 0) {
        int32_t third   = GRAV_RELOAD_FRAMES / 3;
        int32_t elapsed = GRAV_RELOAD_FRAMES - reload_timer;
        if (elapsed < third)          drop = (GRAV_RELOAD_DROP * elapsed) / third;
        else if (elapsed < 2 * third) drop = GRAV_RELOAD_DROP;
        else                          drop = (GRAV_RELOAD_DROP *
                                              (GRAV_RELOAD_FRAMES - elapsed)) / third;
    }

    /* Pitch (about the barrel-cross axis):
       - Reload: tilt the muzzle DOWN as it sinks and level off as it rises. The
         tilt ramps 3x faster than the drop so it reaches full angle while the
         gun is still on screen (a drop-proportional tilt peaks only once it's
         fully off screen, where you can't see it).
       - Recoil: a sharp muzzle-UP kick the instant a shot fires, decaying back
         to the rest pose over GRAV_RECOIL_FRAMES. */
    int32_t pitch_drop = 3 * drop;
    if (pitch_drop > GRAV_RELOAD_DROP) pitch_drop = GRAV_RELOAD_DROP;
    int32_t reload_pitch = -(GRAV_RELOAD_PITCH * pitch_drop) / GRAV_RELOAD_DROP;
    int32_t recoil_pitch =  (GRAV_RECOIL_PITCH * recoil_timer) / GRAV_RECOIL_FRAMES;

    /* Aim-follow: while aiming, angle the model toward the crosshair — yaw with
       its horizontal offset from centre, pitch with its vertical offset (down
       crosshair => muzzle down, matching the reload/recoil pitch sign). */
    int32_t aim_yaw = 0, aim_pitch = 0;
    if (aiming) {
        int32_t dx = aim_x - SCREEN_XRES / 2;
        int32_t yaw_gain = (dx > 0) ? GRAV_AIM_YAW_R : GRAV_AIM_YAW;
        aim_yaw   =  (dx * yaw_gain) / 100;
        aim_pitch = -((aim_y - SCREEN_YRES / 2) * GRAV_AIM_PITCH) / 100;
    }

    /* The held model. */
    SVECTOR rot = {GRAV_ROT_X + (int16_t)(reload_pitch + recoil_pitch + aim_pitch),
                   GRAV_ROT_Y + (int16_t)aim_yaw, GRAV_ROT_Z, 0};
    MATRIX  weapon_vs;
    RotMatrix(&rot, &weapon_vs);
    weapon_vs.t[0] = GRAV_VS_X;
    weapon_vs.t[1] = GRAV_VS_Y + drop;
    weapon_vs.t[2] = GRAV_VS_Z;
    weapon_render_model(ctx, graveolver_smd, &weapon_vs, GRAV_BRIGHTNESS);

    /* Overlays are hidden behind the inventory menu, so skip them there. */
    if (game_state == STATE_MENU) return;

    /* Aiming reticule: a white cross with a centre gap at the crosshair. */
    {
        int cx = aim_x, cy = aim_y;
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

/* Debug overlay: the crosshair hit circle itself, a yellow ring of exactly
   GUN_AIM_RADIUS pixels drawn in 2D at the crosshair. This is the actual hit
   field — an enemy is struck only when part of its body projects inside this
   ring — so you can see directly how much aim slop there is. */
void graveolver_debug_draw(RenderContext *ctx) {
    if (!debug_mode) return;

    const int SEGS = 20;
    int cx = gun_crosshair_x(), cy = gun_crosshair_y();
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int prev_x = cx + GUN_AIM_RADIUS, prev_y = cy;   /* isin/icos: angle 0 -> +X */
    int s;
    for (s = 1; s <= SEGS; s++) {
        int32_t ang = (s * 4096) / SEGS;             /* 0..4096 = full turn */
        int nx = cx + ((icos(ang) * GUN_AIM_RADIUS) >> 12);
        int ny = cy + ((isin(ang) * GUN_AIM_RADIUS) >> 12);
        if (ctx->next_packet + sizeof(LINE_F2) > buf_end) return;
        LINE_F2 *ln = (LINE_F2 *)ctx->next_packet;
        setLineF2(ln);
        setRGB0(ln, 255, 240, 0);
        setXY2(ln, prev_x, prev_y, nx, ny);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_GUN_RETICULE], ln);
        ctx->next_packet += sizeof(LINE_F2);
        prev_x = nx; prev_y = ny;
    }
}
