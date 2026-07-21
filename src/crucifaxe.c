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
#include "crucifaxe.h"
#include "weapon.h"
#include "crate.h"
#include "title.h"
#include "demondog.h"
#include "zombie.h"
#include "sound.h"
#include "fatdoor.h"

static SMD  *crucifaxe_smd  = NULL;
static void *crucifaxe_buff = NULL;

int swing_timer    = 0;
static int square_prev          = 0;   /* Square state last frame, for edge-detect */
static int hit_this_swing       = 0;
static int crate_hit_this_swing = 0;
static int ddog_hit_this_swing  = 0;
static int zomb_hit_this_swing  = 0;
static int fatdoor_hit_this_swing = 0;

void crucifaxe_init(void) {
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

void update_crucifaxe(void) {
    /* Edge-detect Square so the axe swings ONCE per press — holding it no longer
       auto-repeats. The swing_timer==0 gate already blocks a new swing until the
       current swing+return animation finishes (SWING_TOTAL frames), so that whole
       animation doubles as the cooldown: a fresh press is required after it. */
    int square_held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        square_held = (~pad->btn & PAD_SQUARE) ? 1 : 0;
    }
    int square_just = square_held && !square_prev;
    square_prev = square_held;

    if (game_state != STATE_MENU && swing_timer == 0 && square_just) {
        swing_timer            = 1;
        hit_this_swing         = 0;
        crate_hit_this_swing   = 0;
        ddog_hit_this_swing    = 0;
        zomb_hit_this_swing    = 0;
        fatdoor_hit_this_swing = 0;
        sound_play(SFX_SWING);
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
                            sound_play(SFX_DOGDIE);
                        } else {
                            sound_play(SFX_AXEHIT);
                        }
                        ddog_hit_this_swing = 1;
                        break;
                    }
                }
            }
        }

        /* Zombie hit — checked independently of vampire and dog hits */
        if (swing_timer <= SWING_DURATION && !zomb_hit_this_swing) {
            int zi;
            for (zi = 0; zi < zombie_count; zi++) {
                Zombie *z = &zombies[zi];
                if (!z->active || z->state == ZMB_DEAD) continue;
                int32_t dx     = z->x - cam_x;
                int32_t dy     = z->y - cam_y;
                int32_t dz     = z->z - cam_z;
                int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
                int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
                if (dist3d < SWING_RANGE) {
                    int32_t dot = ((int32_t)dx * isin(cam_rot) +
                                   (int32_t)dz * icos(cam_rot)) >> 12;
                    if (dot > 0) {
                        z->kb_vx = dist2d > 0 ? (dx * ZMB_KNOCKBACK) / dist2d : 0;
                        z->kb_vz = dist2d > 0 ? (dz * ZMB_KNOCKBACK) / dist2d : 0;
                        z->health--;
                        z->hit_timer = ZMB_BAR_TIMER_MAX;
                        if (z->health <= 0) {
                            z->state = ZMB_DEAD;
                            spawn_blood_burst(z->x, z->y, z->z);
                            sound_stop(SFX_ZOMBIE);     /* cut the groan immediately */
                            sound_play(SFX_ZOMBIEDIE);  /* play the death sound */
                        } else {
                            sound_play(SFX_AXEHIT);    /* non-fatal hit */
                        }
                        zomb_hit_this_swing = 1;
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

        /* Breakable door smash — only in areas that have fat doors. The doors
           are a single global array (fatdoors_try_smash already skips doors whose
           area != game_state), but menu/delivery have none, so gate here too. */
        if ((game_state == STATE_KITCHEN_DINING || game_state == STATE_RECEPTION ||
             game_state == STATE_CONSERVATORY) &&
            swing_timer <= SWING_DURATION && !fatdoor_hit_this_swing) {
            if (fatdoors_try_smash())
                fatdoor_hit_this_swing = 1;
        }

        if (++swing_timer > SWING_TOTAL)
            swing_timer = 0;
    }
}

void draw_crucifaxe(RenderContext *ctx) {
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

    /* Weapon position in view/camera space — fixed offset from camera centre,
       plus the weapon-switch slide (off the bottom when swapping). */
    int32_t vs_x =  40;
    int32_t vs_y =  80 + (( 7 * t) >> 8) + weapon_switch_offset();
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

    /* Flat-shaded view-space render (shared with the grave-olver); restores the
       camera view matrix before returning. */
    weapon_render_model(ctx, crucifaxe_smd, &weapon_vs, 4096 /* 1.0x */);
}
