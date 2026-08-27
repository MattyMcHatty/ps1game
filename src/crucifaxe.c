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
#include "spider.h"
#include "sound.h"
#include "fatdoor.h"
#include "vines.h"
#include "tentacle.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "living_statue.h"
#include "hadad.h"

static SMD  *crucifaxe_smd  = NULL;
static void *crucifaxe_buff = NULL;

int swing_timer    = 0;
static int square_prev          = 0;   /* Square state last frame, for edge-detect */
static int hit_this_swing       = 0;
static int crate_hit_this_swing = 0;
static int ddog_hit_this_swing  = 0;
static int zomb_hit_this_swing  = 0;
static int spdr_hit_this_swing  = 0;
static int fatdoor_hit_this_swing = 0;
/* Vine curtains get their own per-swing latch rather than sharing the fat
   door's: the two families are independent, and one swing that reaches both
   should damage both. */
static int vine_hit_this_swing    = 0;
static int tent_hit_this_swing  = 0;
static int raf_hit_this_swing   = 0;
static int msh_hit_this_swing   = 0;
static int lst_hit_this_swing   = 0;
static int had_hit_this_swing   = 0;

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
        spdr_hit_this_swing    = 0;
        fatdoor_hit_this_swing = 0;
        vine_hit_this_swing    = 0;
        tent_hit_this_swing    = 0;
        raf_hit_this_swing     = 0;
        msh_hit_this_swing     = 0;
        lst_hit_this_swing     = 0;
        had_hit_this_swing     = 0;
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

        /* Spider hit — checked independently of the other enemies. One
           damage per swing, and spider_damage drops a ceiling spider that gets
           hit before it has noticed the player. Knockback only applies once it
           is on the floor; shoving a hanging or falling one sideways would slide
           it away from the ceiling it is dropping off. */
        if (swing_timer <= SWING_DURATION && !spdr_hit_this_swing) {
            int si;
            for (si = 0; si < spider_count; si++) {
                Spider *s = &spiders[si];
                if (!s->active || s->state == SPD_DEAD ||
                    s->area != current_area) continue;
                int32_t dx     = s->x - cam_x;
                int32_t dy     = s->y - cam_y;
                int32_t dz     = s->z - cam_z;
                int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
                int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
                if (dist3d < SWING_RANGE) {
                    int32_t dot = ((int32_t)dx * isin(cam_rot) +
                                   (int32_t)dz * icos(cam_rot)) >> 12;
                    if (dot > 0) {
                        if (s->state == SPD_ALERT) {
                            s->kb_vx = dist2d > 0 ? (dx * SPD_KNOCKBACK) / dist2d : 0;
                            s->kb_vz = dist2d > 0 ? (dz * SPD_KNOCKBACK) / dist2d : 0;
                        }
                        spider_damage(s, 1);
                        spdr_hit_this_swing = 1;
                        break;
                    }
                }
            }
        }

        /* Tentacle hit — checked independently of the other enemies */
        if (swing_timer <= SWING_DURATION && !tent_hit_this_swing) {
            if (tentacles_try_hit())
                tent_hit_this_swing = 1;
        }

        /* Rafflesia hit — same shape as the tentacle's: rooted enemy, so the
           whole test (reach, facing, one hit per swing) lives in its module. */
        if (swing_timer <= SWING_DURATION && !raf_hit_this_swing) {
            if (rafflesias_try_hit())
                raf_hit_this_swing = 1;
        }

        /* Mushroom Head hit. Same shape as the spider's, with one difference:
           the knockback is MSH_KNOCKBACK (45, decaying 7/8 a frame, so ~360
           units all told) rather than the spider's 20 — the axe is supposed to
           buy real breathing room against this one. It is skipped mid-LEAP,
           because the arc is interpolated between two fixed endpoints and a
           sideways shove would slide the body off the line it is committed to.
           mushroom_damage handles waking a pacing one into its scream. */
        if (swing_timer <= SWING_DURATION && !msh_hit_this_swing) {
            int mi;
            for (mi = 0; mi < mushroom_count; mi++) {
                Mushroom *m = &mushrooms[mi];
                if (!m->active || m->state == MSH_DEAD ||
                    m->area != current_area) continue;
                int32_t dx     = m->x - cam_x;
                int32_t dy     = (m->y + MSH_Y_OFFSET) - cam_y;
                int32_t dz     = m->z - cam_z;
                int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
                int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
                if (dist3d < SWING_RANGE) {
                    int32_t dot = ((int32_t)dx * isin(cam_rot) +
                                   (int32_t)dz * icos(cam_rot)) >> 12;
                    if (dot > 0) {
                        if (!mushroom_airborne(m)) {
                            m->kb_vx = dist2d > 0 ? (dx * MSH_KNOCKBACK) / dist2d : 0;
                            m->kb_vz = dist2d > 0 ? (dz * MSH_KNOCKBACK) / dist2d : 0;
                        }
                        mushroom_damage(m, 1);
                        msh_hit_this_swing = 1;
                        break;
                    }
                }
            }
        }

        /* Living Statue hit. Same shape as the mushroom's above, with two
           differences, both of which are the enemy's design rather than an
           omission:

             - NO KNOCKBACK. It is a block of stone that moves by teleporting;
               there is nowhere for a shove to put it that its own next teleport
               would not immediately overrule.
             - >>> IT IS SKIPPED UNLESS IT IS IN ATTACK. <<< The axe is
               ineffective against an idle or stalking statue, and the skip is
               BEFORE the reach test on purpose: it leaves lst_hit_this_swing
               unset, so the same swing can still go on to land on something
               that can actually be hurt. (The second half of the same rule
               lives in living_statue_damage, which refuses a hit in any other
               state.)

           There is no Grave-olver block for this enemy ANYWHERE, for the same
           reason. The HELLUMINATOR is the one weapon that reaches past this
           rule — it can burn a statue that is still stalking, provided the
           statue has teleported at least once, and doing so drops it into
           ATTACK. That exception lives entirely in living_statue_burn and
           deliberately does NOT come through living_statue_damage, so the axe's
           rule here could not be loosened by a change made for the lantern.

           The whole test lives in the module — the shape the tentacle and the
           Rafflesia already use — rather than being inlined here like the
           dog/zombie/spider/mushroom blocks above. It has to be: a statue's
           solid body holds the player 265 off its centre, which is further than
           the Manhattan dist3d those blocks measure can reach, so the reach must
           be taken to its SURFACE from the same radius the push-out uses. See
           LST_BODY_RADIUS in living_statue.h. */
        if (swing_timer <= SWING_DURATION && !lst_hit_this_swing) {
            if (living_statues_try_hit())
                lst_hit_this_swing = 1;
        }

        /* HADAD — the Living Statue's arrangement in every respect, and for
           every one of the same reasons. The axe is the ONLY thing that can
           hurt him (1 a swing against 100 HP, so a hundred connected swings),
           there is no Grave-olver block for him anywhere, he is SKIPPED rather
           than reported as hit while he is on his plinth or out of the room so
           the swing stays live for anything else, and the reach is measured in
           his own module because his 495 stop distance is far past the
           Manhattan dist3d the inlined blocks above use. See HAD_BODY_RADIUS.

           The other way to kill him is not a weapon at all: under FLAG_HADAD_THREE
           the corridor grinders close on him and empty the whole bar at the end
           of the Hadad Death Scene (src/hadad_grinder.c). */
        if (swing_timer <= SWING_DURATION && !had_hit_this_swing) {
            if (hadads_try_hit())
                had_hit_this_swing = 1;
        }

        /* >>> THERE IS DELIBERATELY NO RABISU BLOCK HERE. <<< The boss is the
           one enemy the crucifaxe cannot damage (see RBS_MAX_HEALTH in
           rabisu.h). The axe is not useless against it — it is the PARRY, and
           rabisu.c reads swing_timer directly for both of its deflect windows,
           so a swing still counts for everything without ever landing a hit.
           Adding a hit block back here would quietly undo the whole fight. */

        /* Crate smash — checked independently of vampire hit */
        if (swing_timer <= SWING_DURATION && !crate_hit_this_swing) {
            if (crate_try_smash())
                crate_hit_this_swing = 1;
        }

        /* Breakable door smash. The doors are a single global array and
           fatdoors_try_smash already skips every door whose area != current_area,
           so areas with no doors (menu/delivery) fall out of its loop for free.
           There used to be an explicit allowlist of door-bearing areas here as
           well; it was a second source of truth that had to be hand-updated per
           room, and the East Hall's door was dead on arrival because it was
           missed. Don't reintroduce it — let the area tag be the only gate. */
        if (swing_timer <= SWING_DURATION && !fatdoor_hit_this_swing) {
            if (fatdoors_try_smash())
                fatdoor_hit_this_swing = 1;
        }
        if (swing_timer <= SWING_DURATION && !vine_hit_this_swing) {
            /* Five hits to clear a destructible curtain; an indestructible
               one reports the hit and loses nothing. vines_try_smash skips
               every curtain whose area != current_area, as the doors do. */
            if (vines_try_smash())
                vine_hit_this_swing = 1;
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
