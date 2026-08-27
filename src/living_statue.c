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
#include "particles.h"
#include "living_statue.h"
#include "crucifaxe.h"   /* SWING_RANGE — living_statues_try_hit owns the reach */
#include "sound.h"

LivingStatue living_statues[MAX_LIVING_STATUES];
int          living_statue_count = 0;

/* ---- Sprites ----------------------------------------------------------------
   Two textures, each owning its own VRAM slot: no texmgr, no re-upload on a
   room transition, just one CD read and one LoadImage apiece at startup. Same
   arrangement as the Mushroom Head's four, and affordable for the same reason —
   96 rows fit the band under the HUD, which is all VRAM has left.

   The UV rect is derived from the TIM's own prect rather than hard-coded, as
   mushroom.c and rafflesia.c do it, so moving a slot in png_to_tim needs no
   matching edit here. */
typedef struct { uint16_t tpage, clut; uint8_t u0, v0, u1, v1; } Sprite;

enum { LST_TEX_IDLE, LST_TEX_ATK, LST_TEX_COUNT };

static Sprite spr[LST_TEX_COUNT];
static int    tex_loaded = 0;

static const char *lst_tex_file[LST_TEX_COUNT] = {
    "\\TEX\\LSIDLE.TIM;1",     /* still: idle and stalking alike */
    "\\TEX\\LSATK.TIM;1",      /* striking                       */
};

/* Texture window the current area expects, restored after each sprite. Both
   bodies sit at Voff 128, so a room's 128-tall window would wrap their V and
   sample whatever is above them in VRAM. Same bracket the mushroom uses. */
static RECT lst_tw_restore;
static int  lst_tw_active = 0;

void living_statues_set_texwindow(const RECT *tw) {
    if (tw) { lst_tw_restore = *tw; lst_tw_active = 1; }
    else    { lst_tw_active = 0; }
}

/* Startup loader for a texture that owns its VRAM slot: CD read, one LoadImage,
   keep the tpage/clut and work the UV rect out of the TIM header. Copied from
   mushroom.c's, itself from rafflesia.c's load_owned_sprite. STARTUP ONLY. */
static void load_owned_sprite(const char *filename, Sprite *s) {
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
    int u_off    = (tim.prect->x & 63) * px_mult;
    /* One-texel inset on every edge, as every other sprite here takes: a
       magnified quad sampling its own edge pixels drags a stripe of whatever is
       next to it in VRAM into the silhouette. */
    s->u0 = (uint8_t)(u_off + 1);
    s->v0 = (uint8_t)((tim.prect->y % 256) + 1);
    s->u1 = (uint8_t)(u_off + tex_w - 2);
    s->v1 = (uint8_t)((tim.prect->y % 256) + tim.prect->h - 2);
    free(buf);
}

void living_statues_load_textures(void) {
    int i;
    for (i = 0; i < LST_TEX_COUNT; i++)
        load_owned_sprite(lst_tex_file[i], &spr[i]);
    tex_loaded = (spr[LST_TEX_IDLE].tpage != 0);
}

/* ---- Small numeric helpers --------------------------------------------------
   A private LCG, as particles.c keeps: the landing search wants a scatter of
   bearings and there is no shared RNG exported anywhere in this codebase. */
static uint32_t lst_rng_state = 0x1a2b3c4du;

static uint32_t lst_rng(void) {
    lst_rng_state = lst_rng_state * 1103515245u + 12345u;
    return (lst_rng_state >> 16) & 0x7fffu;
}

/* Integer square root (Newton, converging downward). Copied from mushroom.c,
   INCLUDING the `t > 0` seed condition — `t > 1` stops one shift early and
   seeds below the root for every input that is not a perfect power of four, at
   which point the loop returns its own seed (isqrt(120) came back 8).
   Every distance in this file that is stated as a RADIUS goes through it, so a
   stated radius means the same in every direction. Maze Two is 10200 x 4600, so
   the worst-case dx*dx + dz*dz is about 1.2e8 — well inside int32. */
static int32_t lst_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* The standing anchor for a point, straight out of the room's floor zones —
   the same table apply_ddog_height walks, read directly rather than through it
   because a statue must not be dragged toward the floor by gravity while it is
   standing on a plinth. Returns 0 and leaves *out alone if the point is off
   every zone, which is what makes this double as the landing search's "is there
   any floor here at all" test. */
static int lst_floor_anchor(int32_t x, int32_t z, int32_t *out) {
    int i;
    for (i = 0; i < floor_zone_count; i++) {
        FloorZone *fz = &floor_zones[i];
        if (x < fz->min_x || x > fz->max_x) continue;
        if (z < fz->min_z || z > fz->max_z) continue;
        if (fz->type == FLOOR_FLAT || fz->type == FLOOR_UPPER) {
            *out = fz->y - GROUND_FLOOR_Y;
            return 1;
        }
        if (fz->type == FLOOR_RAMP) {
            int32_t len = fz->ramp_axis_end - fz->ramp_axis_start;
            int32_t pos = fz->ramp_along_x ? x : z;
            int32_t t, dy;
            if (len == 0) { *out = fz->ramp_y_start - GROUND_FLOOR_Y; return 1; }
            t = ((pos - fz->ramp_axis_start) << 12) / len;
            if (t <    0) t =    0;
            if (t > 4096) t = 4096;
            dy   = fz->ramp_y_end - fz->ramp_y_start;
            *out = fz->ramp_y_start + ((dy * t) >> 12) - GROUND_FLOOR_Y;
            return 1;
        }
    }
    return 0;
}

int living_statue_add(int32_t x, int32_t z, int32_t y, int living,
                      GameState area) {
    if (living_statue_count >= MAX_LIVING_STATUES) return -1;
    int i = living_statue_count++;
    LivingStatue *s = &living_statues[i];
    *s = (LivingStatue){0};
    s->x = x; s->y = y; s->z = z;
    s->spawn_x = x; s->spawn_y = y; s->spawn_z = z;
    s->health  = LST_MAX_HEALTH;
    s->state   = LST_IDLE;
    s->living  = living;
    s->active  = 1;
    s->area    = area;
    return i;
}

void living_statues_init(void) {
    /* Placements are seeded on a room's first entry by the world system (see
       world_seed_room in world.c). The array starts empty. */
    living_statue_count = 0;
}

void living_statues_reset(void) {
    living_statue_count = 0;
}

void living_statues_rest(void) {
    int i;
    for (i = 0; i < living_statue_count; i++) {
        LivingStatue *s = &living_statues[i];
        if (!s->active || s->state == LST_DEAD) continue;
        /* Rebuild exactly as living_statue_add left it: back on the plinth,
           unarmed, at full health, teleport count zeroed. The spawn, the
           `living` flag and the area tag are the only things that survive the
           wipe — miss any of them and a statue comes back as the wrong kind of
           object in the wrong room. */
        int32_t   sx = s->spawn_x, sy = s->spawn_y, sz = s->spawn_z;
        int       lv = s->living;
        GameState a  = s->area;
        *s = (LivingStatue){0};
        s->x = sx; s->y = sy; s->z = sz;
        s->spawn_x = sx; s->spawn_y = sy; s->spawn_z = sz;
        s->health  = LST_MAX_HEALTH;
        s->state   = LST_IDLE;
        s->living  = lv;
        s->active  = 1;
        s->area    = a;
    }
}

/* No weaknesses. The damage model for this enemy is a STATE GATE, not a
   multiplier: what changes between the crucifaxe and the Helluminator is WHEN a
   hit is allowed to land (living_statue_damage vs living_statue_burn), not how
   much it takes off. Holy fire deliberately does its plain 1 — the lantern's
   privilege is that it can touch a stalking statue at all, and giving it a
   multiplier on top would make the crucifaxe pointless against this enemy. */
static const Weakness living_statue_weakness[] = {
    { DMG_KINETIC, 100 },   /* 100% = no change */
};

int32_t living_statue_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, living_statue_weakness,
                        WEAKNESS_COUNT(living_statue_weakness));
}

void living_statue_body(const LivingStatue *s,
                        int32_t *cyc, int32_t *hh, int32_t *hw) {
    *cyc = s->y + LST_Y_OFFSET;
    *hh  = LST_HALF_H;
    *hw  = LST_HALF_W;      /* the idle width, in every state — see the header */
}

/* Drop a statue into ATTACK, from wherever it currently stands.
   Shared by the two ways in: the fourth teleport arriving one poly out, and a
   stalking statue being burnt (see living_statue_burn). Both need the same
   three things done, and doing them in one place is what stops the second
   entrance being a half-formed version of the first.

   Seeding `facing` toward the player matters as much here as at the teleport:
   ls_steer blends a new heading into the old one, so a statue that woke with a
   zeroed facing would crawl out of the wake-up at a third speed for the first
   few frames — the exact moment it should look like it has decided. */
static void ls_enter_attack(LivingStatue *s) {
    int32_t adx = player_x() - s->x, adz = player_z() - s->z;
    int32_t ad  = (adx < 0 ? -adx : adx) + (adz < 0 ? -adz : adz);
    if (ad > 0) {
        int32_t mx = (adx * LST_CHASE_SPEED) / ad;
        int32_t mz = (adz * LST_CHASE_SPEED) / ad;
        s->facing = ((int32_t)(int16_t)mx << 16) | (uint16_t)(int16_t)mz;
    }
    s->vy    = 0;
    s->state = LST_ATTACK;
}

/* Take the health off, and everything that goes with it. The gates that decide
   WHETHER a hit lands live in the two callers below; this is only what happens
   once one has, so the axe and the lantern can never disagree about what a
   wounded or destroyed statue looks and sounds like. */
static void ls_wound(LivingStatue *s, int dmg) {
    s->health   -= dmg;
    s->hit_timer = LST_BAR_TIMER_MAX;
    if (s->health <= 0) {
        s->health = 0;
        s->state  = LST_DEAD;
        /* Grey, because what comes out of a statue is stone. spawn_blood_burst
           is the red one every other enemy uses; this is the same burst with
           the colour swapped, which is all "the blood effect, coloured grey"
           needs — the particle pool carries a per-burst RGB already. */
        spawn_burst(s->x, s->y + LST_Y_OFFSET, s->z,
                    LST_BLOOD_R, LST_BLOOD_G, LST_BLOOD_B);
        sound_play(SFX_RUMBLE);    /* the same grind it moves on */
    } else {
        sound_play(SFX_AXEHIT);
    }
}

void living_statue_damage(LivingStatue *s, int dmg) {
    if (!s->active || s->state == LST_DEAD) return;
    /* >>> ONLY WHILE IT IS STRIKING. <<< A hit on an idle or stalking statue is
       ineffective for everything EXCEPT holy fire, and "ineffective" means
       completely silent: no chip, no health-bar flash, no sound. Anything else
       would read as a weapon that works and a health bar that is not going
       down, which is a worse lie than nothing happening at all. crucifaxe.c
       skips these before its reach test so the swing is not consumed either.
       The lantern's exception does NOT come through here — it has its own door
       in living_statue_burn, so relaxing that rule could not loosen this one by
       accident. */
    if (s->state != LST_ATTACK) return;
    ls_wound(s, dmg);
}

/* ---- Holy fire -------------------------------------------------------------
   The Helluminator's door into this enemy, and the ONE weapon that does not
   have to wait for the statue to strike first. Two things separate it from
   living_statue_damage, and both are the design rather than a shortcut:

     1. IT LANDS IN STALK, BUT ONLY ONCE THE STATUE HAS MOVED. `tp_stage > 0`
        is the test, i.e. it has made at least one teleport, which is exactly
        "it has left its starting position". A statue standing where it was
        authored is still masonry as far as the lantern is concerned — burning
        the garden's ornaments to find out which of them are alive would give
        away every ambush in the game for the price of a few units of oil. The
        moment one has jumped, the player has been shown it is alive and is
        allowed to fight back.
        (`living` is not tested: a plain-masonry statue never leaves LST_IDLE,
        so the state gate below already covers it.)
     2. IT WAKES WHAT IT BURNS. A stalking statue that is hit stops stalking
        immediately and comes for the player — it does not teleport again, and
        the two gates stop applying to it. Holy fire is not a way to whittle a
        statue down from safety while it is frozen by your gaze; it is a way to
        force the confrontation early, on the player's terms but at the cost of
        having it walking at them for the rest of the fight.

   LST_IDLE is refused outright, as is a burn on a statue that has not moved:
   both are silent, for the same reason the crucifaxe's refusal is. */
void living_statue_burn(LivingStatue *s, int dmg) {
    if (!s->active || s->state == LST_DEAD) return;

    if (s->state == LST_STALK) {
        if (s->tp_stage == 0) return;    /* still on its plinth: masonry */
        /* Wake it BEFORE the wound, so that if this same tick is the killing
           one the death still runs from a consistent state rather than a corpse
           in mid-transition. ls_enter_attack only seeds the walk. */
        ls_enter_attack(s);
    } else if (s->state != LST_ATTACK) {
        return;
    }

    ls_wound(s, dmg);
}

/* ---- The landing search -----------------------------------------------------
   Find a spot BEHIND the player, no further from them than `range`, that a
   statue can actually stand on. Returns 1 and fills *ox/*oy/*oz, or 0 if none of
   LST_TP_TRIES bearings produced one.

   `fx`/`fz` are the camera's forward vector, so -forward is dead behind. Each
   try picks a bearing within LST_TP_REAR of that and then walks the radius
   inward from `range` in LST_TP_STEP increments — biasing toward the far end of
   the allowance, which is what "within N polys" is meant to feel like, while
   still finding the near spot in a corridor where the far one is inside a hedge.

   A candidate has to clear THREE tests, and the third one is the one that is
   easy to leave out and wrong to:

     1. there is a floor zone under it;
     2. apply_flat_entity_collision does not move it, i.e. it is not crowding a
        wall at the body radius;
     3. >>> THE STRAIGHT LINE FROM THE PLAYER TO IT CROSSES NO WALL. <<<

   Test 2 CANNOT do test 3's job, and this is the trap a teleporting enemy walks
   into that a walking one never does. The walls here are ONE-SIDED: collide_
   wall_frontonly pushes only from the normal side and returns immediately for
   anything behind the plane, and it also rejects anything that does not project
   onto the segment. A point deep inside a hedge is behind that hedge's wall on
   every side, so it reads as perfectly clear — measured on Maze Two, where the
   plinth's own pocket passes the same test, which is why the player can walk
   into the plinth at all. Every other enemy in this game is safe from that only
   because it WALKS, and so can never cross a front-facing wall to get there.
   This one jumps.

   collision_segment_blocked is an exact two-sided crossing test, so a candidate
   reachable from the player in a straight line without crossing anything is in
   the same open region the player is standing in — which is the property that
   actually wanted checking. It also gives the enemy a better read for free: it
   appears in the corridor BEHIND you rather than through the hedge beside you. */
static int ls_pick_landing(int32_t range, int32_t *ox, int32_t *oy, int32_t *oz) {
    int32_t px = player_x(), pz = player_z();
    int32_t py = player_y();
    int32_t fwd_x = isin(cam_rot), fwd_z = icos(cam_rot);
    int t;

    for (t = 0; t < LST_TP_TRIES; t++) {
        /* A bearing in the rear hemisphere: the reverse of the camera forward,
           swung by up to +/-LST_TP_REAR. Built by rotating (-fwd) rather than by
           taking an angle, because cam_rot is already the angle and isin/icos
           are the only trig here. */
        int32_t swing = (int32_t)(lst_rng() % (2 * LST_TP_REAR + 1)) - LST_TP_REAR;
        int32_t ang   = (cam_rot + 2048 + swing) & 4095;
        int32_t dx    = isin(ang), dz = icos(ang);
        int32_t r;

        for (r = range; r >= LST_TP_MIN; r -= LST_TP_STEP) {
            int32_t cx = px + ((dx * r) >> 12);
            int32_t cz = pz + ((dz * r) >> 12);
            int32_t cy;
            int32_t tx = cx, tz = cz;

            /* Behind the player. The bearing already guarantees this for
               |swing| < 1024, but LST_TP_REAR is a tunable and the whole point
               of this enemy is that it never appears in front of you. */
            if ((cx - px) * fwd_x + (cz - pz) * fwd_z > 0) continue;

            if (!lst_floor_anchor(cx, cz, &cy)) continue;

            apply_flat_entity_collision(&tx, &tz, LST_BODY_RADIUS);
            if (tx != cx || tz != cz) continue;   /* crowding a wall */

            /* ...and it is in the same open region the player is in. See the
               note above: this is the test that stops it landing inside a
               hedge, and nothing else here can. */
            if (collision_segment_blocked(px, py, pz, cx, cy + LST_Y_OFFSET, cz))
                continue;

            *ox = cx; *oy = cy; *oz = cz;
            return 1;
        }
    }
    return 0;
}

/* ---- The ATTACK walk --------------------------------------------------------
   The mushroom's feeler steering with the separation pass taken out (statues do
   not come in crowds) and the goal hard-wired back to the player, which is all
   this enemy ever chases. Everything load-bearing is kept: the feeler probe, the
   two-sided wall-follow with a commit timer so the body follows a hedge instead
   of flip-flopping against it, and the turn blend.

   `sight_clear` switches the wall-follow off when nothing stands between the
   statue and the player. It is passed in rather than decided here for the reason
   the mushroom's is: the feeler's point-and-radius test also trips merely
   walking NEAR a hedge, and wall-following on that deadlocks in the maze's
   concave corners, while the real move's own collision push already slides the
   body along anything it grazes. */
static void ls_steer(LivingStatue *s, int32_t goal_dx, int32_t goal_dz,
                     int32_t speed, int sight_clear) {
    int32_t desired_x = goal_dx, desired_z = goal_dz;
    int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                           (desired_z < 0 ? -desired_z : desired_z);
    if (desired_dist == 0) desired_dist = 1;

    int32_t feeler_x = s->x + (desired_x * LST_FEELER_LEN) / desired_dist;
    int32_t feeler_z = s->z + (desired_z * LST_FEELER_LEN) / desired_dist;
    int32_t fx = feeler_x, fz = feeler_z;
    apply_flat_entity_collision(&fx, &fz, LST_BODY_RADIUS);
    int blocked = (fx != feeler_x || fz != feeler_z);
    if (sight_clear) { blocked = 0; s->steer_timer = 0; }

    int32_t pl_x = -goal_dz, pl_z =  goal_dx;   /* slide left  */
    int32_t pr_x =  goal_dz, pr_z = -goal_dx;   /* slide right */
    int32_t goal_px = s->x + goal_dx;
    int32_t goal_pz = s->z + goal_dz;

    if (blocked && s->steer_timer <= 0) {
        int32_t pl_dist = (pl_x < 0 ? -pl_x : pl_x) + (pl_z < 0 ? -pl_z : pl_z);
        int32_t pr_dist = (pr_x < 0 ? -pr_x : pr_x) + (pr_z < 0 ? -pr_z : pr_z);
        if (pl_dist == 0) pl_dist = 1;
        if (pr_dist == 0) pr_dist = 1;

        int32_t lx = s->x + (pl_x * LST_FEELER_LEN) / pl_dist;
        int32_t lz = s->z + (pl_z * LST_FEELER_LEN) / pl_dist;
        int32_t rx = s->x + (pr_x * LST_FEELER_LEN) / pr_dist;
        int32_t rz = s->z + (pr_z * LST_FEELER_LEN) / pr_dist;

        int32_t tlx = lx, tlz = lz;
        apply_flat_entity_collision(&tlx, &tlz, LST_BODY_RADIUS);
        int left_blocked = (tlx != lx || tlz != lz);

        int32_t trx = rx, trz = rz;
        apply_flat_entity_collision(&trx, &trz, LST_BODY_RADIUS);
        int right_blocked = (trx != rx || trz != rz);

        if (left_blocked && !right_blocked) {
            s->steer_dir = +1;
        } else if (right_blocked && !left_blocked) {
            s->steer_dir = -1;
        } else {
            int32_t ld = (goal_px - lx < 0 ? lx - goal_px : goal_px - lx) +
                         (goal_pz - lz < 0 ? lz - goal_pz : goal_pz - lz);
            int32_t rd = (goal_px - rx < 0 ? rx - goal_px : goal_px - rx) +
                         (goal_pz - rz < 0 ? rz - goal_pz : goal_pz - rz);
            s->steer_dir = (ld <= rd) ? -1 : +1;
        }
        s->steer_timer = LST_STEER_COMMIT;
    }

    if (s->steer_timer > 0) {
        if (s->steer_dir < 0) { desired_x = pl_x; desired_z = pl_z; }
        else                  { desired_x = pr_x; desired_z = pr_z; }
        desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                       (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;
        s->steer_timer--;
    }

    /* Blend the new heading into the old one so turns are not instant. */
    int32_t move_x  = (desired_x * speed) / desired_dist;
    int32_t move_z  = (desired_z * speed) / desired_dist;
    int32_t prev_mx = (int16_t)(s->facing >> 16);
    int32_t prev_mz = (int16_t)(s->facing & 0xFFFF);
    int32_t blend_x = (prev_mx * (8 - LST_TURN_RATE) + move_x * LST_TURN_RATE) >> 3;
    int32_t blend_z = (prev_mz * (8 - LST_TURN_RATE) + move_z * LST_TURN_RATE) >> 3;
    s->facing = ((int32_t)(int16_t)blend_x << 16) | (uint16_t)(int16_t)blend_z;

    s->x += blend_x;
    s->z += blend_z;
    apply_flat_entity_collision(&s->x, &s->z, LST_BODY_RADIUS);
}

/* The stage's allowance, in world units. Stage 3 and every stage after it is
   one poly — a statue that has arrived does not back off again. */
static int32_t ls_stage_range(int stage) {
    switch (stage) {
    case 0:  return LST_TP_R0;
    case 1:  return LST_TP_R1;
    case 2:  return LST_TP_R2;
    default: return LST_TP_R3;
    }
}

void update_living_statues(void) {
    static int hurt_sfx_cooldown = 0;
    int i;
    if (hurt_sfx_cooldown > 0) hurt_sfx_cooldown--;

    for (i = 0; i < living_statue_count; i++) {
        LivingStatue *s = &living_statues[i];
        /* current_area, NEVER game_state: gating on game_state would freeze the
           whole enemy for as long as the inventory menu is up, letting the
           player pause a stalk with Start (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!s->active || s->state == LST_DEAD || s->area != current_area)
            continue;

        if (s->hit_timer    > 0) s->hit_timer--;
        if (s->damage_timer > 0) s->damage_timer--;

        /* Authored as plain masonry: solid, drawn, and otherwise not an enemy at
           all. Nothing below this line ever runs for one. */
        if (!s->living) continue;

        /* player_x/y/z, not cam_*: a camera-locked puzzle flies the camera off
           to a fixed shot while the PLAYER stays standing where they were, and
           an enemy that tracked the camera would close on an empty spot
           (camera.h). Enemies keep acting through those puzzles. */
        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - s->x;
        int32_t dy = py - (s->y + LST_Y_OFFSET);
        int32_t dz = pz - s->z;
        int32_t dist = lst_isqrt(dx * dx + dz * dz);

        /* --- Arming. One-way: a statue that has been approached once stays
           armed for the rest of the visit, and only living_statues_rest (a room
           change or a save) puts it back to sleep. --- */
        if (s->state == LST_IDLE) {
            if (dist <= LST_ACTIVATE_RADIUS) {
                s->state    = LST_STALK;
                /* Arm the countdown HERE and not at spawn. A fresh entity's
                   timers are zero, and a `if (--t <= 0) teleport();` cooldown on
                   a zeroed timer fires on the very first frame it becomes
                   eligible — mistake 8 in tools/ADDING_AN_ENEMY.txt. */
                s->tp_timer = LST_TP_INTERVAL;
            }
            continue;
        }

        /* --- ATTACK's damage. Reach is tested horizontally and vertically
           SEPARATELY, for the reason every other enemy here does the same: the
           player's eye sits above the body's centre, and a combined budget eats
           the whole allowance vertically so an adjacent enemy never lands a
           hit. The horizontal half is the RADIAL `dist`, not the Manhattan sum
           the other enemies use — see LST_CATCH_DIST, which does not clear the
           statue's own push-out on the diagonals if summed. --- */
        if (s->state == LST_ATTACK) {
            if (!game_over && dist < LST_CATCH_DIST &&
                (dy < 0 ? -dy : dy) < LST_CATCH_DIST && s->damage_timer == 0) {
                s->damage_timer = LST_DAMAGE_TICK;
                player_hurt(LST_DAMAGE_AMOUNT);
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

            /* >>> AND IT WALKS. <<< ATTACK is not a frozen pose — once a statue
               has arrived it stops teleporting for good and simply follows, at
               the player's own walk speed, until one of them is dead. The two
               gates above do NOT apply here: it comes after you whether you are
               looking at it or not, which is the whole reversal the state is
               for. Its stillness was only ever a stalking behaviour.

               The floor is probed each frame now that it moves, so it walks down
               off the plinth (or off anything else it was teleported onto)
               instead of sliding along at the height it arrived at. */
            apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                              &s->on_upper_floor, &s->on_ramp);

            /* Walk until its own cylinder is what is stopping it — LST_HOLD_DIST,
               not the strike range. Stopping at the strike range instead would
               park it 35 units short of contact, where its push-out never even
               engages; pressing all the way in means the body itself settles the
               distance, and the reach is sized to clear that (see
               LST_CATCH_DIST). Past that point more walking only grinds the
               push-out against the steering. */
            if (dist >= LST_HOLD_DIST) {
                int clear = !collision_segment_blocked(s->x, s->y + LST_Y_OFFSET,
                                                       s->z, px, py, pz);
                ls_steer(s, dx, dz, LST_CHASE_SPEED, clear);
            }
            continue;
        }

        /* --- The two gates. Both are re-tested every frame and either one
           failing RESETS the countdown rather than pausing it, so a player who
           keeps glancing back never lets one arrive. --- */

        /* 1. Being watched. The statue is in the half-plane the camera faces. */
        int watched = (dx * isin(cam_rot) + dz * icos(cam_rot)) < 0;

        /* 2. Far enough away. Only the FIRST stage's landing range (1200) is
              outside this gate, which is what the re-freeze note in the header
              is about. LST_REFREEZE drops the gate once the sequence has
              started, for whoever wants the four teleports to run through. */
        int far_enough = (dist >= LST_STALK_GATE) ||
                         (!LST_REFREEZE && s->tp_stage > 0);

        if (watched || !far_enough) {
            s->tp_timer = LST_TP_INTERVAL;
            continue;
        }

        if (s->tp_timer > 0) s->tp_timer--;
        if (s->tp_timer > 0) continue;

        /* --- Teleport. --- */
        int32_t range = ls_stage_range(s->tp_stage);
        /* "Unless the player is closer than this when the teleport is
           activated, in which case they will teleport within 1 poly." A statue
           armed at close quarters does not reappear six polys away. */
        if (dist < range) range = LST_TP_R3;

        int32_t nx, ny, nz;
        if (!ls_pick_landing(range, &nx, &ny, &nz)) {
            /* Nowhere to stand behind them this frame — every bearing was in a
               hedge. Leave the countdown expired and try again next frame
               rather than teleporting into the geometry; do NOT burn the stage
               on a move that did not happen. */
            continue;
        }

        s->x = nx; s->y = ny; s->z = nz;
        if (s->tp_stage < LST_TP_STAGES) s->tp_stage++;
        s->tp_timer = LST_TP_INTERVAL;
        sound_play(SFX_RUMBLE);

        /* One poly away is the definition of arrival, whether it got there by
           working through the stages or by the close-quarters shortcut above.
           ls_enter_attack seeds the facing it will walk on — see it for why a
           statue must never arrive with a zeroed one. It is the same door holy
           fire uses to wake a stalking statue early (living_statue_burn). */
        if (range == LST_TP_R3)
            ls_enter_attack(s);
    }
}

/* Crucifaxe strike. Reach is measured to the body's SURFACE, not to its centre,
   because the wall-like standoff puts the centre out of the axe's range: the
   push holds the player LST_BODY_RADIUS + 195 = 265 off, and 265 radial is up
   to 375 Manhattan on a diagonal, past SWING_RANGE's 350. Measured from the
   surface the same stand gives a 195 horizontal gap and, with the player's eye
   inside the body's vertical span, no vertical gap at all — 195 against 350,
   with room to spare from every bearing.

   Horizontal and vertical gaps are computed SEPARATELY and then summed, which
   is the same rule the contact tests keep and for the same reason: a combined
   budget lets the rise to the body centre eat the whole allowance.
   The horizontal gap is derived from LST_BODY_RADIUS — the very constant
   living_statues_collide pushes with — so the two cannot disagree about where
   the edge is and leave the player held at a distance the axe cannot cross. */
int living_statues_try_hit(void) {
    int i;
    for (i = 0; i < living_statue_count; i++) {
        LivingStatue *s = &living_statues[i];
        /* Not in ATTACK: skipped, and NOT reported as a hit, so the swing is
           still live for whatever else is in range. */
        if (!s->active || s->state != LST_ATTACK || s->area != current_area)
            continue;

        int32_t dx = s->x - cam_x;
        int32_t dz = s->z - cam_z;
        int32_t d  = lst_isqrt(dx * dx + dz * dz);
        int32_t gh = d > LST_BODY_RADIUS ? d - LST_BODY_RADIUS : 0;

        /* Vertical: clamp the eye into the body's span; the gap is how far it
           had to move. Zero whenever the eye is level with any part of it. */
        int32_t top = s->y + LST_Y_OFFSET - LST_HALF_H;
        int32_t bot = s->y + LST_Y_OFFSET + LST_HALF_H;
        int32_t ey  = cam_y;
        int32_t gv  = (ey < top) ? (top - ey) : (ey > bot ? ey - bot : 0);

        if (gh + gv >= SWING_RANGE) continue;

        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;

        living_statue_damage(s, 1);
        return 1;
    }
    return 0;
}

void living_statues_collide(int32_t *px, int32_t py, int32_t *pz,
                            int32_t radius) {
    int i;
    for (i = 0; i < living_statue_count; i++) {
        LivingStatue *s = &living_statues[i];
        if (!s->active || s->state == LST_DEAD || s->area != current_area)
            continue;

        /* Vertical gate against the player's body span, so a statue on a plinth
           the player could somehow get above does not push them in mid-air. The
           body runs from its top to its feet; the player's span is their eye
           down to the floor under them. */
        int32_t top = s->y + LST_Y_OFFSET - LST_HALF_H;
        int32_t bot = s->y + LST_Y_OFFSET + LST_HALF_H;
        if (py + GROUND_FLOOR_Y < top || py - 30 > bot) continue;

        /* A radial push, not the crates' AABB: a billboard has no facing, so the
           stop distance must be identical from every bearing, and that needs a
           true sqrt (see tools/ADDING_A_3D_ENEMY.txt STEP 6). */
        int32_t dx   = *px - s->x;
        int32_t dz   = *pz - s->z;
        int32_t need = LST_BODY_RADIUS + radius;
        int32_t d    = lst_isqrt(dx * dx + dz * dz);
        if (d >= need) continue;
        if (d == 0) { *px = s->x + need; continue; }   /* dead centre */
        *px = s->x + (dx * need) / d;
        *pz = s->z + (dz * need) / d;
    }
}

/* Bracket an already-filled POLY_FT4 with a full/unmasked texture window and a
   restore of the area's own, all within one OT bucket. Identical to the
   mushroom's, and needed for the same reason: both sprites sit at Voff 128.
   addPrim() prepends, so adding restore, poly, disable yields the draw order
   disable -> poly -> restore. */
static void add_ft4_windowed(RenderContext *ctx, int32_t otz, POLY_FT4 *poly) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;

    if (lst_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &lst_tw_restore);
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

/* One camera-facing body quad. No mirror flip and no roll: a statue has one
   pose per state and never turns. There is deliberately no gte_nclip backface
   cull — a camera-facing quad has no back face to cull.
   `half_w` is passed rather than read from a constant because the attack frame
   flares to LST_ATK_HALF_W; the flare is about the quad's CENTRE, so it does
   not move the body. */
static void draw_lst_sprite(RenderContext *ctx, LivingStatue *s,
                            const Sprite *sp, int32_t half_w) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);

    int32_t cx = s->x, cz = s->z, cy = s->y + LST_Y_OFFSET;

    int16_t dwx = (int16_t)((half_w * rx) >> 12);
    int16_t dwz = (int16_t)((half_w * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - LST_HALF_H);
    int16_t vy_bot = (int16_t)(cy + LST_HALF_H);

    SVECTOR v[4];
    v[0].vx = (int16_t)(cx - dwx); v[0].vy = vy_top; v[0].vz = (int16_t)(cz - dwz); v[0].pad = 0;
    v[1].vx = (int16_t)(cx + dwx); v[1].vy = vy_top; v[1].vz = (int16_t)(cz + dwz); v[1].pad = 0;
    v[2].vx = (int16_t)(cx + dwx); v[2].vy = vy_bot; v[2].vz = (int16_t)(cz + dwz); v[2].pad = 0;
    v[3].vx = (int16_t)(cx - dwx); v[3].vy = vy_bot; v[3].vz = (int16_t)(cz - dwz); v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4], otz;

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
    /* Sort on the room-geometry scale (raw average Z) so hedges between the
       camera and the body occlude it, with the SCENE_OT_MIN clamp every sprite
       here takes. */
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    int32_t fdx  = s->x - cam_x;
    int32_t fdz  = s->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    if (dist >= g_fog_far) return;          /* fully fogged: cull */
    int32_t fs   = render_fog_scale(dist);
    uint8_t fog8 = fs > 255 ? 255 : (uint8_t)fs;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    if (s->hit_timer > 0) setRGB0(poly, fog8, fog8 >> 2, fog8 >> 2);
    else                  setRGB0(poly, fog8, fog8, fog8);

    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

    poly->u0 = sp->u0; poly->v0 = sp->v0;
    poly->u1 = sp->u1; poly->v1 = sp->v0;
    poly->u2 = sp->u0; poly->v2 = sp->v1;
    poly->u3 = sp->u1; poly->v3 = sp->v1;

    poly->tpage = sp->tpage;
    poly->clut  = sp->clut;

    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, otz, poly);

    if (s->hit_timer <= 0) return;

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

    int16_t fill_w = (int16_t)((s->health * 40) / LST_MAX_HEALTH);
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

void draw_living_statues(RenderContext *ctx) {
    if (!tex_loaded) return;
    int i;
    for (i = 0; i < living_statue_count; i++) {
        LivingStatue *s = &living_statues[i];
        if (!s->active || s->state == LST_DEAD || s->area != current_area)
            continue;

        int32_t dx = s->x - cam_x;
        int32_t dz = s->z - cam_z;
        int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (wdist >= g_fog_far) continue;

        /* >>> THERE IS DELIBERATELY NO SIGHTLINE CULL HERE. <<< The other sprite
           enemies skip themselves when collision_hidden_from_camera says a wall
           stands between them and the camera, to save the fill on a billboard
           that would be painted over anyway. This enemy must not, and the reason
           is where it stands: that test is a flat XZ wall-crossing, so it
           discards the statue whenever a hedge is on the line — measured against
           Maze Two's real collision mesh, from 78% of the standable positions
           inside the fog radius. Its plinth sits in a dead-end alcove behind two
           hedge runs, so in practice the thing was invisible from almost
           everywhere a player would look for it.

           The OT sort already handles the occlusion correctly and does it
           per-pixel rather than per-sprite, which is also the only way the head
           can show above a hedge that covers the body. The fill it costs back is
           much smaller than it was: the body is 128 x 322 now, not 184 x 460. */

        int tex = (s->state == LST_ATTACK) ? LST_TEX_ATK : LST_TEX_IDLE;
        int32_t half_w = (s->state == LST_ATTACK) ? LST_ATK_HALF_W : LST_HALF_W;
        draw_lst_sprite(ctx, s, &spr[tex], half_w);
    }
}
