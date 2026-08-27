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
#include "spider.h"
#include "tentacle.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "rabisu.h"
#include "vampire.h"
#include "particles.h"
#include "sound.h"
#include "bullet_hit.h"
#include "graveolver.h"
#include "weapon.h"
#include "vines.h"      /* the one PROP the gun can destroy */
#include "damage.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

#define GRAV_FIRE_COOLDOWN 12   /* frames between shots (revolver cadence) */

/* Hitscan aim, screen-space. Picture a fixed circle around the crosshair: a shot
   hits the CLOSEST thing whose on-screen silhouette falls inside that circle.
   Depth and height don't widen the aim — a constant pixel radius at any range —
   they only decide which candidate is nearer. An enemy is a candidate when any
   part of its body projects inside the circle AND the crosshair line to its
   depth isn't blocked by a nearer wall/prop. "Body" means the sprite's whole
   on-screen rectangle, width included — see weapon_aim_in_circle in weapon.h,
   which owns the test itself. */
#define GUN_RANGE        4000  /* max forward distance a shot reaches           */
#define GUN_AIM_RADIUS     14  /* crosshair hit circle, in screen pixels        */
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

/* Colour of the flash currently burning off — latched when the shot fires, so
   it stays correct even if the chambered type somehow changed mid-flash. */
static uint8_t flash_r = 255, flash_g = 255, flash_b = 255;

/* --- Ammo swapping (R2) -----------------------------------------------------
   Swapping the chambered type costs a FULL reload: the same timer, dip and
   sound as a normal reload, with the type change applied only when the timer
   reaches 0. reload_to is the type the cylinder will hold once it completes;
   for an ordinary top-up it equals graveolver_ammo, so both cases share one
   completion path.

   swap_pending distinguishes the two so the "Loaded ..." log line is posted
   only for a real type change, and only on completion — never at the moment
   R2 is pressed. Cancelling (a weapon switch mid-reload) drops both, leaving
   the cylinder exactly as it was. */
static AmmoType reload_to     = AMMO_STANDARD;
static int      swap_pending  = 0;

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

/* --- enemy damage -----------------------------------------------------------
   damage_dog() and damage_zombie() used to live here as statics. They are now
   demon_dog_damage() and zombie_damage(), in the enemies' own modules alongside
   spider_damage() and mushroom_damage(), because the Helluminator burns the same
   two from a second call site (see demondog.h). Nothing about the behaviour
   changed in the move. */

/* The aim geometry — the crosshair ray, the screen projection, the circle test
   and the blocked-line test — was hoisted into the shared weapon layer when the
   Helluminator needed the same maths with a wider circle. See weapon.h; the gun
   supplies GUN_AIM_RADIUS and GUN_RANGE where those functions were previously
   reading them out of this file. */

/* Fire one round: flash + hitscan the nearest enemy under the reticule. The
   caller has already confirmed a round is chambered and spends it. */
static void graveolver_fire(void) {
    muzzle_flash = GUN_FLASH_FRAMES;
    recoil_timer = GRAV_RECOIL_FRAMES;
    /* The flash colour is the chambered ammo's — white for Standard, orange for
       Flame. Latched here rather than read at draw time (see flash_r). */
    flash_r = ammo_info[graveolver_ammo].flash_r;
    flash_g = ammo_info[graveolver_ammo].flash_g;
    flash_b = ammo_info[graveolver_ammo].flash_b;
    sound_play(SFX_GR_SHOT);

    int32_t fx = isin(cam_rot), fz = icos(cam_rot);
    int      best_kind = -1, best_idx = -1;
    int32_t  best_depth = 0x7fffffff, depth;
    int i;

    for (i = 0; i < demon_dog_count; i++) {
        DemonDog *d = &demon_dogs[i];
        if (!d->active || d->state == DDOG_DEAD) continue;
        if (weapon_aim_in_circle(d->x, d->y + DDOG_Y_OFFSET, d->z,
                            DDOG_HALF_W, DDOG_HALF_H, fx, fz, GUN_AIM_RADIUS, GUN_RANGE, &depth) &&
            depth < best_depth && weapon_aim_clear(fx, fz, depth)) {
            best_depth = depth; best_kind = 0; best_idx = i;
        }
    }
    for (i = 0; i < zombie_count; i++) {
        Zombie *z = &zombies[i];
        if (!z->active || z->state == ZMB_DEAD) continue;
        if (weapon_aim_in_circle(z->x, z->y + ZMB_Y_OFFSET, z->z,
                            ZMB_HALF_W, ZMB_HALF_H, fx, fz, GUN_AIM_RADIUS, GUN_RANGE, &depth) &&
            depth < best_depth && weapon_aim_clear(fx, fz, depth)) {
            best_depth = depth; best_kind = 1; best_idx = i;
        }
    }
    for (i = 0; i < spider_count; i++) {
        Spider *s = &spiders[i];
        if (!s->active || s->state == SPD_DEAD || s->area != current_area) continue;
        if (weapon_aim_in_circle(s->x, s->y + SPD_Y_OFFSET, s->z,
                            SPD_HALF_W, SPD_HALF_H, fx, fz, GUN_AIM_RADIUS, GUN_RANGE, &depth) &&
            depth < best_depth && weapon_aim_clear(fx, fz, depth)) {
            best_depth = depth; best_kind = 4; best_idx = i;
        }
    }
    for (i = 0; i < tentacle_count; i++) {
        Tentacle *t = &tentacles[i];
        if (!t->active || t->health <= 0 || t->area != current_area) continue;
        int32_t cyc, hh, hw;
        tentacle_body(t, &cyc, &hh, &hw);
        if (weapon_aim_in_circle(t->x, cyc, t->z, hw, hh, fx, fz, GUN_AIM_RADIUS, GUN_RANGE, &depth) &&
            depth < best_depth && weapon_aim_clear(fx, fz, depth)) {
            best_depth = depth; best_kind = 3; best_idx = i;
        }
    }
    for (i = 0; i < rafflesia_count; i++) {
        Rafflesia *rf = &rafflesias[i];
        if (!rf->active || rf->health <= 0 || rf->area != current_area) continue;
        int32_t cyc, hh, hw;
        rafflesia_body(rf, &cyc, &hh, &hw);
        if (weapon_aim_in_circle(rf->x, cyc, rf->z, hw, hh, fx, fz, GUN_AIM_RADIUS, GUN_RANGE, &depth) &&
            depth < best_depth && weapon_aim_clear(fx, fz, depth)) {
            best_depth = depth; best_kind = 6; best_idx = i;
        }
    }
    for (i = 0; i < mushroom_count; i++) {
        Mushroom *m = &mushrooms[i];
        if (!m->active || m->state == MSH_DEAD || m->area != current_area) continue;
        int32_t cyc, hh, hw;
        mushroom_body(m, &cyc, &hh, &hw);
        if (weapon_aim_in_circle(m->x, cyc, m->z, hw, hh, fx, fz, GUN_AIM_RADIUS, GUN_RANGE, &depth) &&
            depth < best_depth && weapon_aim_clear(fx, fz, depth)) {
            best_depth = depth; best_kind = 7; best_idx = i;
        }
    }
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *rb = &rabisus[i];
        /* `dying` as well as `dead`: the boss stays on screen through its whole
           death sequence, and a round spent into a corpse should miss. */
        if (!rb->active || rb->dead || rb->dying || rb->area != current_area) continue;
        int32_t cyc, hh, hw;
        rabisu_body(rb, &cyc, &hh, &hw);
        if (weapon_aim_in_circle(rb->x, cyc, rb->z, hw, hh, fx, fz, GUN_AIM_RADIUS, GUN_RANGE, &depth) &&
            depth < best_depth && weapon_aim_clear(fx, fz, depth)) {
            best_depth = depth; best_kind = 5; best_idx = i;
        }
    }
    if (vampire_health > 0 &&
        weapon_aim_in_circle(vampire_x, vampire_y + VAMPIRE_Y, vampire_z,
                        VAMPIRE_HALF_W, VAMPIRE_HALF_H, fx, fz, GUN_AIM_RADIUS, GUN_RANGE, &depth) &&
        depth < best_depth && weapon_aim_clear(fx, fz, depth)) {
        best_kind = 2;
    }

    /* VINE CURTAINS, and they are the only PROP that competes for the
       crosshair. They are deliberately tested LAST and outside the pattern
       above: vines_point_solid puts them in props_block_point, so
       weapon_aim_clear would reject a curtain on account of the curtain itself
       if it were tested the way an enemy is. Testing it here, against the best
       depth the enemies produced, gets both halves right — a curtain nearer
       than the enemy behind it eats the round, and one further away does not.
       The `depth < best_depth` comparison is the whole of that logic. */
    for (i = 0; i < vine_count; i++) {
        Vine *vn = &vines[i];
        if (!vn->active || vn->state != VINE_INTACT || vn->area != current_area) continue;
        int32_t cyc, hh, hw;
        vines_body(i, &cyc, &hh, &hw);
        if (weapon_aim_in_circle(vn->x, cyc, vn->z, hw, hh, fx, fz, GUN_AIM_RADIUS, GUN_RANGE, &depth) &&
            depth < best_depth) {
            best_depth = depth; best_kind = 8; best_idx = i;
        }
    }

    /* Nothing hittable in the circle (or a wall/prop was the nearest thing under
       it): no damage, no impact sprite — ghit only marks enemy hits. */
    if (best_kind < 0)
        return;

    /* One round is one point of BASE damage; each enemy scales it by its own
       weakness to the chambered ammo's damage type (see damage.h). Flame Rounds
       are not stronger in general — only against what burns. */
    DamageType dmg_type = ammo_info[graveolver_ammo].damage;

    if (best_kind == 0) {
        demon_dog_damage(&demon_dogs[best_idx],
                   demon_dog_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (best_kind == 1) {
        zombie_damage(&zombies[best_idx],
                      zombie_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (best_kind == 3) {
        tentacle_shoot(&tentacles[best_idx],
                       tentacle_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (best_kind == 4) {
        spider_damage(&spiders[best_idx],
                      spider_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (best_kind == 6) {
        /* 1 from a Standard Round, 2 from a Flame Round (its weakness table
           doubles DMG_FLAME) — so four shots to kill, or two. */
        rafflesia_shoot(&rafflesias[best_idx],
                        rafflesia_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (best_kind == 7) {
        /* No weaknesses: 1 from any round, so five shots to kill whatever is
           chambered (see mushroom.h). */
        mushroom_damage(&mushrooms[best_idx],
                        mushroom_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (best_kind == 8) {
        /* Not an enemy and not scaled by a weakness table: a curtain asks the
           DAMAGE TYPE directly. DMG_FLAME clears a destructible one outright,
           a Standard Round does nothing but scatter leaves. */
        vines_shoot(best_idx, (int)dmg_type);
    } else if (best_kind == 5) {
        /* Boss: 1 from a Standard Round, 2 from a Flame Round (its weakness
           table doubles DMG_FLAME) — so 20 or 10 shots to kill. */
        rabisu_damage(&rabisus[best_idx],
                      rabisu_scale_damage(GUN_DAMAGE, dmg_type));
    } else {
        vampire_health   -= vampire_scale_damage(GUN_DAMAGE, dmg_type);
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
        weapon_aim_ray_point(fx, fz, d, &hx, &hy, &hz);
        bullet_hit_spawn(hx, hy, hz);
    }
}

int graveolver_is_reloading(void) {
    return reload_timer > 0;
}

/* Abort an in-progress reload WITHOUT topping up the cylinder (the refill only
   happens when the timer counts down to 0 on its own). The cylinder is left at
   its current count AND its current type, so switching weapons part-way through
   an ammo swap keeps whatever was already chambered — the swap simply never
   happened, and no "Loaded ..." line is posted. */
void graveolver_cancel_reload(void) {
    if (reload_timer > 0) {
        reload_timer = 0;
        swap_pending = 0;
        reload_to    = graveolver_ammo;
        sound_stop(SFX_GR_RELOAD);
    }
}

/* The next ammo type the player can actually chamber: one they hold reserve
   rounds of, skipping the current type. Returns the current type when there is
   nothing else to switch to, which is how R2 stays inert with only one type. */
static AmmoType next_available_ammo(void) {
    int i;
    for (i = 1; i < MAX_AMMO_TYPES; i++) {
        AmmoType t = (AmmoType)((graveolver_ammo + i) % MAX_AMMO_TYPES);
        if (player_ammo[t] > 0) return t;
    }
    return graveolver_ammo;
}

/* Finish a reload: empty whatever is chambered back into ITS OWN reserve, then
   fill from the target type's. Unloading first means a swap never destroys
   rounds — four Standard left in the cylinder go back to the Standard reserve
   and can be re-chambered later. For an ordinary top-up reload_to equals the
   chambered type, so the unload/reload pair is a no-op on the count. */
static void reload_complete(void) {
    player_ammo[graveolver_ammo] += graveolver_loaded;
    graveolver_loaded             = 0;
    graveolver_ammo               = reload_to;

    int take = GRAVEOLVER_CAPACITY;
    if (take > player_ammo[graveolver_ammo]) take = player_ammo[graveolver_ammo];
    graveolver_loaded            += take;
    player_ammo[graveolver_ammo] -= take;

    /* Only a genuine type change announces itself, and only now that the
       animation has played out. */
    if (swap_pending) {
        show_pickup_msg_raw(ammo_info[graveolver_ammo].load_msg);
        swap_pending = 0;
    }
}

void graveolver_update(void) {
    /* Edge-detect Square so one press fires once; a short cooldown paces taps. */
    static int square_prev = 0;
    static int r2_prev     = 0;
    static int cooldown    = 0;
    if (cooldown > 0)    cooldown--;
    if (muzzle_flash > 0) muzzle_flash--;
    if (recoil_timer > 0) recoil_timer--;

    int square_held = 0, r2_held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        square_held = (~pad->btn & PAD_SQUARE) ? 1 : 0;
        r2_held     = (~pad->btn & PAD_R2)     ? 1 : 0;
    }
    int square_just = square_held && !square_prev;
    int r2_just     = r2_held     && !r2_prev;
    square_prev = square_held;
    r2_prev     = r2_held;

    /* A reload is running: count it down and settle the cylinder when it
       finishes. No firing and no further swapping until it completes. */
    if (reload_timer > 0) {
        reload_timer--;
        if (reload_timer == 0) reload_complete();
        return;
    }

    if (game_state == STATE_MENU)
        return;

    /* R2 swaps the chambered ammo type, at the cost of a full reload. Inert
       unless there is another type in reserve to switch TO, so with only
       Standard Rounds the button does nothing. */
    if (r2_just) {
        AmmoType target = next_available_ammo();
        if (target != graveolver_ammo) {
            reload_to    = target;
            swap_pending = 1;
            reload_timer = GRAV_RELOAD_FRAMES;
            sound_play(SFX_GR_RELOAD);
            return;
        }
    }

    if (!square_just || cooldown != 0)
        return;

    if (graveolver_loaded > 0) {
        graveolver_fire();
        graveolver_loaded--;
        cooldown = GRAV_FIRE_COOLDOWN;
    } else if (player_ammo[graveolver_ammo] > 0) {
        /* Empty cylinder + trigger pull with rounds of the chambered type in
           reserve: an ordinary top-up, so the target type is the current one. */
        reload_to    = graveolver_ammo;
        swap_pending = 0;
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

    /* Muzzle flash: a brief semi-transparent wash over the whole screen, in the
       fired ammo's colour — white for Standard Rounds, orange for Flame.
       The TILE is added first and the DR_TPAGE (abr=0, 50% blend) last so the
       GPU processes the blend mode before the tile (LIFO within the OT node). */
    if (muzzle_flash > 0) {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(TILE) <= buf_end) {
            TILE *t = (TILE *)ctx->next_packet;
            setTile(t);
            setSemiTrans(t, 1);
            setRGB0(t, flash_r, flash_g, flash_b);
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
    if (debug_mode != 3) return;  /* aim-circle viz only in full-debug (level 3) */

    const int SEGS = 20;
    int cx = aim_x, cy = aim_y;
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
