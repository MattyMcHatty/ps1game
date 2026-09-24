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
#include "dining_table.h"
#include "particles.h"
#include "lumberer.h"
#include "fatdoor.h"
#include "door_anim.h"  /* the three panel halves this sheet borrows */
#include "texmgr.h"
#include "sound.h"

Lumberer lumberers[MAX_LUMBERERS];
int      lumberer_count = 0;

/* The brief is "roughly 35% of the player's maximum HP". Keep the two from
   drifting apart: raising MAX_HEALTH without revisiting this would quietly make
   the shockwave weaker, and the number is meant to be a FRACTION of the bar. */
#define LMB_SHOCK_DAMAGE  (MAX_HEALTH * 7 / 20)
_Static_assert(LMB_SHOCK_DAMAGE * 20 == MAX_HEALTH * 7,
               "LMB_SHOCK_DAMAGE is no longer 35% of the player's maximum");

/* The wave has to be able to catch someone who was in reach when the arm went
   up. If this ever stops holding, standing still becomes a valid answer to the
   attack and the two-second wind-up stops being a decision. */
_Static_assert(LMB_SHOCK_RADIUS > LMB_ATTACK_RADIUS,
               "the shockwave no longer covers the range that triggers it");

/* ---- Sprites ----------------------------------------------------------------
   ONE sheet per ROW of the source art. Six 85x128 frames in a 256x256 image,
   and it cannot be uploaded whole: V is eight bits, so an 8bpp texture has to
   satisfy y%256 + height <= 256 — a 256-row texture must start at VRAM y 0 or
   256, and both bands are full. So it ships as two 256x128 halves, each holding
   one row of three frames:

     sheet 0  x[448,576) y128   frames 0,1,2 (images 1,2,3)
     sheet 1  x[832,960) y128   frames 3,4,5 (images 4,5,6)

   Both are quantised against ONE master palette (see disc.xml), so the walk
   cycle's step from frame 2 to frame 3 — which crosses from one sheet to the
   other — does not shift the creature's colour.

   THE PAGES ARE ON LOAN FROM THE FRONT END, not from another enemy — three
   door-panel leaves and the opening still, none of which Chapter 3 draws. See
   lumberer.h for the whole argument, tools/VRAM_MAP_CATACOMBS.txt for the sweep
   that found them, and door_anim_restore_panels()/intro_start() for the way
   back. */
#define LMB_SHEETS 2
#define LMB_FRAMES 6
static uint16_t lmb_tpage [LMB_SHEETS] = { 0, 0 };
static uint16_t lmb_clut  [LMB_SHEETS] = { 0, 0 };
static int      lmb_tex_id[LMB_SHEETS] = { -1, -1 };
static int      tex_loaded = 0;
static uint16_t shadow_tpage = 0, shadow_clut = 0;

/* Texture window the current area expects, restored after each sprite. Both
   sheets sit at VRAM y=128, i.e. Voff 128, so a room's 128-tall window would
   wrap their V and sample the wrong half of the page. Identical to the
   crawler's bracket and needed for the identical reason. */
static RECT lmb_tw_restore;
static int  lmb_tw_active = 0;

void lumberers_set_texwindow(const RECT *tw) {
    if (tw) { lmb_tw_restore = *tw; lmb_tw_active = 1; }
    else    { lmb_tw_active = 0; }
}

static void load_tim(const char *filename, uint16_t *tpage_out, uint16_t *clut_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return;
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

/* TWO registrations for the whole enemy — one per sheet, six frames in all.
   The shadow is the shared SHADOW.TIM every other enemy uses and stays a plain
   startup load: it is resident, belongs to no bank and nothing overwrites it. */
void lumberers_load_textures(void) {
    /* BANK: Chapter 3 only. The lumberer is placed in the Tomb and can never be
       drawn outside TEXBANK_CATACOMBS — derived, not guessed:
       py tools/check_tex_banks.py walks the uploader call graph and fails the
       build if this mask is short. */
    texmgr_set_bank(TEXBANK_CATACOMBS);
    lmb_tex_id[0] = texmgr_register("\\TEXCTCMB\\LUMBERA.TIM;1");
    lmb_tex_id[1] = texmgr_register("\\TEXCTCMB\\LUMBERB.TIM;1");
    {
        int i;
        for (i = 0; i < LMB_SHEETS; i++) {
            lmb_tpage[i] = texmgr_tpage(lmb_tex_id[i]);
            lmb_clut [i] = texmgr_clut (lmb_tex_id[i]);
        }
    }
    load_tim("\\SHADOW.TIM;1", &shadow_tpage, &shadow_clut);
    tex_loaded = (lmb_tex_id[0] >= 0 && lmb_tex_id[1] >= 0);
}

void lumberers_upload_textures(void) {
    int i;
    for (i = 0; i < LMB_SHEETS; i++) texmgr_upload(lmb_tex_id[i]);
    /* >>> AND SAY WHAT WAS JUST OVERWRITTEN. <<< These two pages are on loan
       from the front end — three door-panel leaves and the opening still (see
       the TEXTURES note in lumberer.h) — and neither owner has any way to know
       it has been clobbered. This is that way: main.c puts the panels back on
       entry to the first room outside Chapter 3, and intro_start() re-reads the
       still on the frame New Game is confirmed. Both are no-ops until this line
       runs. */
    door_anim_panels_taken();
}

/* Integer square root (Newton, converging downward). Copied from mushroom.c,
   including the `t > 0` seed condition — `t > 1` stops one shift early and
   seeds BELOW the root for every input that is not a perfect power of four, at
   which point the function returns its own seed (isqrt(120) came back 8).

   Everything in this file stated as a RADIUS uses this rather than the
   Manhattan sum the steering uses, so a stated radius means the same in every
   direction. The Tomb is 4200 square, so the worst-case dx*dx + dz*dz is about
   3.5e7 — comfortably inside int32. */
static int32_t lmb_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* Point the body along (dx,dz). Only the SIGN of the stored vector matters —
   draw_lumberers uses it to pick the front pair or the back pair — so it is
   normalised to a small magnitude that cannot overflow the packed int16s. */
static void lmb_face(Lumberer *s, int32_t dx, int32_t dz) {
    int32_t d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (d <= 0) return;
    int32_t fx = (dx * 64) / d;
    int32_t fz = (dz * 64) / d;
    s->facing = ((int32_t)(int16_t)fx << 16) | (uint16_t)(int16_t)fz;
}

int lumberer_add(int32_t ax, int32_t az, int32_t bx, int32_t bz,
                 int32_t y, GameState area) {
    if (lumberer_count >= MAX_LUMBERERS) return -1;
    int i = lumberer_count++;
    Lumberer *s = &lumberers[i];
    *s = (Lumberer){0};
    s->x = ax; s->y = y; s->z = az;
    s->pa_x = ax; s->pa_z = az;
    s->pb_x = bx; s->pb_z = bz;
    s->spawn_y = y;
    s->to_b   = 1;                 /* starts at A, walking toward B */
    s->health = LMB_MAX_HEALTH;
    s->state  = LMB_PATROL;
    s->active = 1;
    s->area   = area;
    lmb_face(s, bx - ax, bz - az);
    return i;
}

void lumberers_init(void) {
    /* Placements are seeded on a room's first entry by the world system (see
       world_enter in world.c). The array starts empty. */
    lumberer_count = 0;
}

void lumberers_reset(void) {
    lumberer_count = 0;
}

void lumberers_rest(void) {
    int i;
    for (i = 0; i < lumberer_count; i++) {
        Lumberer *s = &lumberers[i];
        if (!s->active || s->state == LMB_DEAD) continue;
        /* Rebuild exactly as lumberer_add left it: back at patrol point A,
           unalerted, at full health, with no wave in the air. The patrol points
           and the area tag are the only things that survive the wipe. */
        int32_t   ax = s->pa_x, az = s->pa_z;
        int32_t   bx = s->pb_x, bz = s->pb_z;
        int32_t   y  = s->spawn_y;
        GameState a  = s->area;
        *s = (Lumberer){0};
        s->x = ax; s->y = y; s->z = az;
        s->pa_x = ax; s->pa_z = az;
        s->pb_x = bx; s->pb_z = bz;
        s->spawn_y = y;
        s->to_b   = 1;
        s->health = LMB_MAX_HEALTH;
        s->state  = LMB_PATROL;
        s->active = 1;
        s->area   = a;
        lmb_face(s, bx - ax, bz - az);
    }
}

/* No weaknesses, by design: the Lumberer takes 1 from a crucifaxe swing and 1
   from a round of any kind, so it is always exactly LMB_MAX_HEALTH hits. The
   placeholder line keeps the table shape for whoever gives it one later. */
static const Weakness lumberer_weakness[] = {
    { DMG_KINETIC, 100 },   /* 100% = no change */
};

int32_t lumberer_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, lumberer_weakness,
                        WEAKNESS_COUNT(lumberer_weakness));
}

void lumberer_body(const Lumberer *s, int32_t *cyc, int32_t *hh, int32_t *hw) {
    *cyc = s->y + LMB_Y_OFFSET;
    *hh  = LMB_HALF_H;
    *hw  = LMB_HALF_W;
}

void lumberer_damage(Lumberer *s, int dmg) {
    if (!s->active || s->state == LMB_DEAD) return;
    /* Hit while still on patrol: that is an alert. Being shot IS being found,
       so it bypasses the radius test entirely. A hit taken mid-attack does NOT
       restart or cancel the swing — the two seconds are a commitment, and the
       axe's own knockback is the reprieve a well-timed swing earns. */
    if (s->state == LMB_PATROL) {
        s->state = LMB_ALERT;
        lmb_face(s, player_x() - s->x, player_z() - s->z);
    }
    s->health   -= dmg;
    s->hit_timer = LMB_BAR_TIMER_MAX;
    if (s->health <= 0) {
        s->health = 0;
        s->state  = LMB_DEAD;
        /* A wave already in the air dies with the body. It is the arm that
           throws it and there is no arm any more. */
        s->wave_t = 0;
        spawn_blood_burst(s->x, s->y, s->z);
        /* SFX_CRWL_SCRM is the CRAWLER's cry, borrowed. A deliberate stand-in,
           not an oversight — see the SOUND note in lumberer.h. */
        sound_play(SFX_CRWL_SCRM);
    } else {
        sound_play(SFX_AXEHIT);
    }
}

/* ---- Steering ---------------------------------------------------------------
   The Mushroom Head's feeler steering, lifted whole: it runs off a GOAL rather
   than the raw player delta, so a patrolling lumberer's goal is its next patrol
   point and an alerted one's is the player, and everything downstream — the
   feeler, the wall-follow perpendiculars, the "which side gets me closer"
   tie-break — reads the goal.

   The sightline is passed as a TARGET rather than as an answer, and traced only
   when its result can change the outcome. That is msh_steer's frame-rate fix
   and the reasoning is unchanged: `blocked` and `steer_timer` are the only two
   consumers, and both branches are already dead when both are zero.

   Returns 1 if the body actually travelled this frame. */
static int lmb_steer(Lumberer *s, int32_t goal_dx, int32_t goal_dz,
                     int32_t speed,
                     int32_t sight_x, int32_t sight_y, int32_t sight_z) {
    int i;

    /* Separation: a soft shove away from any other lumberer standing too close,
       so a pair never stacks into one silhouette. */
    int32_t sep_x = 0, sep_z = 0;
    for (i = 0; i < lumberer_count; i++) {
        Lumberer *o = &lumberers[i];
        if (o == s) continue;
        if (!o->active || o->state == LMB_DEAD || o->area != current_area) continue;
        int32_t odx   = s->x - o->x;
        int32_t odz   = s->z - o->z;
        int32_t odist = (odx < 0 ? -odx : odx) + (odz < 0 ? -odz : odz);
        if (odist < LMB_SEP_RADIUS && odist > 0) {
            int32_t push = LMB_SEP_RADIUS - odist;
            sep_x += (odx * push) / odist;
            sep_z += (odz * push) / odist;
        }
    }

    int32_t desired_x = goal_dx + sep_x * LMB_SEP_WEIGHT;
    int32_t desired_z = goal_dz + sep_z * LMB_SEP_WEIGHT;
    int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                           (desired_z < 0 ? -desired_z : desired_z);
    if (desired_dist == 0) desired_dist = 1;

    /* Probe ahead in the desired direction. */
    int32_t feeler_x = s->x + (desired_x * LMB_FEELER_LEN) / desired_dist;
    int32_t feeler_z = s->z + (desired_z * LMB_FEELER_LEN) / desired_dist;
    int32_t fx = feeler_x, fz = feeler_z;
    crates_collide(&fx, s->y, &fz, 80);
    dining_tables_collide(&fx, s->y, &fz, 75);
    apply_flat_entity_collision(&fx, &fz, LMB_BODY_RADIUS);
    int blocked = (fx != feeler_x || fz != feeler_z);
    if (blocked || s->steer_timer > 0) {
        if (!collision_segment_blocked(s->x, s->y, s->z,
                                       sight_x, sight_y, sight_z)) {
            blocked = 0;
            s->steer_timer = 0;
        }
    }

    int32_t pl_x = -goal_dz, pl_z =  goal_dx;   /* slide left  */
    int32_t pr_x =  goal_dz, pr_z = -goal_dx;   /* slide right */
    int32_t goal_px = s->x + goal_dx;
    int32_t goal_pz = s->z + goal_dz;

    if (blocked && s->steer_timer <= 0) {
        /* Newly blocked: probe both sides and commit to one for a while, so the
           body follows the wall instead of flip-flopping against it. */
        int32_t pl_dist = (pl_x < 0 ? -pl_x : pl_x) + (pl_z < 0 ? -pl_z : pl_z);
        int32_t pr_dist = (pr_x < 0 ? -pr_x : pr_x) + (pr_z < 0 ? -pr_z : pr_z);
        if (pl_dist == 0) pl_dist = 1;
        if (pr_dist == 0) pr_dist = 1;

        int32_t lx = s->x + (pl_x * LMB_FEELER_LEN) / pl_dist;
        int32_t lz = s->z + (pl_z * LMB_FEELER_LEN) / pl_dist;
        int32_t rx = s->x + (pr_x * LMB_FEELER_LEN) / pr_dist;
        int32_t rz = s->z + (pr_z * LMB_FEELER_LEN) / pr_dist;

        int32_t tlx = lx, tlz = lz;
        crates_collide(&tlx, s->y, &tlz, 80);
        dining_tables_collide(&tlx, s->y, &tlz, 75);
        apply_flat_entity_collision(&tlx, &tlz, LMB_BODY_RADIUS);
        int left_blocked = (tlx != lx || tlz != lz);

        int32_t trx = rx, trz = rz;
        crates_collide(&trx, s->y, &trz, 80);
        dining_tables_collide(&trx, s->y, &trz, 75);
        apply_flat_entity_collision(&trx, &trz, LMB_BODY_RADIUS);
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
        s->steer_timer = LMB_STEER_COMMIT;
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
    int32_t blend_x = (prev_mx * (8 - LMB_TURN_RATE) + move_x * LMB_TURN_RATE) >> 3;
    int32_t blend_z = (prev_mz * (8 - LMB_TURN_RATE) + move_z * LMB_TURN_RATE) >> 3;
    s->facing = ((int32_t)(int16_t)blend_x << 16) | (uint16_t)(int16_t)blend_z;

    int32_t was_x = s->x, was_z = s->z;
    s->x += blend_x;
    s->z += blend_z;
    apply_flat_entity_collision(&s->x, &s->z, LMB_BODY_RADIUS);
    crates_collide(&s->x, s->y, &s->z, 80);
    dining_tables_collide(&s->x, s->y, &s->z, 75);
    fatdoors_collide(&s->x, s->y, &s->z, LMB_DOOR_CLEARANCE);

    /* The body's REAL travel, after every push — not the step it asked for.
       That is what the animation clock runs on, so a lumberer pinned against a
       loculus block stops stepping instead of marching on the spot. */
    return (s->x != was_x || s->z != was_z);
}

/* ---- The shockwave ----------------------------------------------------------
   Ticked every frame a wave is up, regardless of what the body has gone on to
   do, so a wave already in the air still lands even if the attack has since
   ended. Launching it is the decision; once it is out it is out. The shape is
   the Rabisu's (rbs_shock_tick), minus the knockback — see lumberer.h. */
static void lmb_shock_tick(Lumberer *s) {
    if (s->wave_t <= 0) return;

    s->wave_t++;
    if (s->wave_t > LMB_SHOCK_EXPAND + LMB_SHOCK_LINGER) { s->wave_t = 0; return; }

    if (s->wave_hit) return;

    int32_t reach = s->wave_t >= LMB_SHOCK_EXPAND
                  ? LMB_SHOCK_RADIUS
                  : (LMB_SHOCK_RADIUS * s->wave_t) / LMB_SHOCK_EXPAND;
    int32_t dx = player_x() - s->x;
    int32_t dz = player_z() - s->z;
    if (lmb_isqrt(dx * dx + dz * dz) > reach) return;

    s->wave_hit = 1;
    if (game_over) return;
    player_hurt(LMB_SHOCK_DAMAGE);
    sound_play(SFX_HURT);
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
        sound_play(SFX_DIE);
    }
}

void update_lumberers(void) {
    int i;

    for (i = 0; i < lumberer_count; i++) {
        Lumberer *s = &lumberers[i];
        /* current_area, NEVER game_state: gating on game_state would freeze the
           whole enemy for as long as the inventory menu is up, letting the
           player pause a fight with Start (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!s->active || s->state == LMB_DEAD || s->area != current_area) continue;

        if (s->hit_timer > 0) s->hit_timer--;
        lmb_shock_tick(s);

        /* player_x/y/z, not cam_*: a camera-locked puzzle flies the camera off
           to a fixed shot while the PLAYER stays standing where they were, and
           an enemy that chased the camera would lose them and swing at an empty
           spot (camera.h). Enemies keep running through those puzzles. */
        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - s->x;
        int32_t dz = pz - s->z;
        int32_t d2 = dx * dx + dz * dz;

        /* --- Gravity and the floor first, as every other enemy --- */
        apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                          &s->on_upper_floor, &s->on_ramp);

        /* --- Knocked back by the axe: slide and decay. --- */
        int knocked = (s->kb_vx != 0 || s->kb_vz != 0);
        if (knocked) {
            s->x += s->kb_vx;
            s->z += s->kb_vz;
            apply_flat_entity_collision(&s->x, &s->z, LMB_BODY_RADIUS);
            crates_collide(&s->x, s->y, &s->z, 80);
            dining_tables_collide(&s->x, s->y, &s->z, 75);
            fatdoors_collide(&s->x, s->y, &s->z, LMB_DOOR_CLEARANCE);
            if (s->kb_vx > 0) s->kb_vx =  (  s->kb_vx * 7) >> 3;
            else              s->kb_vx = -((-s->kb_vx * 7) >> 3);
            if (s->kb_vz > 0) s->kb_vz =  (  s->kb_vz * 7) >> 3;
            else              s->kb_vz = -((-s->kb_vz * 7) >> 3);
            /* Being shoved suspends walking — but NOT the attack. Those two
               phases drive themselves off a frame counter and do not move the
               body at all, so they tick right through a knockback. Letting a
               player with good axe timing hold an enemy in its wind-up
               indefinitely is the bug the Mushroom Head had (mushroom.c). */
            if (s->state != LMB_WINDUP && s->state != LMB_STRIKE) continue;
        }

        s->moved = 0;

        switch (s->state) {

        case LMB_PATROL: {
            /* --- Noticing the player. Proximity alone: no facing test and no
               sightline, unlike the Mushroom Head's three-part wake. This one
               is specified to wake on range, so it wakes on range. --- */
            if (d2 <= (int32_t)LMB_ALERT_RADIUS * LMB_ALERT_RADIUS) {
                s->state = LMB_ALERT;
                lmb_face(s, dx, dz);
                break;
            }

            /* Walk the patrol. The leg flips when the current point is reached;
               the reach test is Manhattan and generous, because the steering
               blend means it rarely lands exactly on a point. */
            int32_t wx = s->to_b ? s->pb_x : s->pa_x;
            int32_t wz = s->to_b ? s->pb_z : s->pa_z;
            int32_t gx = wx - s->x, gz = wz - s->z;
            if ((gx < 0 ? -gx : gx) + (gz < 0 ? -gz : gz) < LMB_WAYPOINT_REACH) {
                s->to_b = !s->to_b;
                wx = s->to_b ? s->pb_x : s->pa_x;
                wz = s->to_b ? s->pb_z : s->pa_z;
                gx = wx - s->x; gz = wz - s->z;
            }
            /* The sightline shortcut is to the WAYPOINT here, not to the
               player: it is the goal that has to be unobstructed for the
               wall-follow to be safely switched off. */
            s->moved = lmb_steer(s, gx, gz, LMB_WALK_SPEED, wx, s->y, wz);
            break;
        }

        case LMB_ALERT:
            /* In reach: plant and raise the arm. Once alerted it never goes back
               to patrolling — there is no losing-the-player state, by design. */
            if (d2 <= (int32_t)LMB_ATTACK_RADIUS * LMB_ATTACK_RADIUS) {
                s->state    = LMB_WINDUP;
                s->atk_tick = 0;
                s->kb_vx = s->kb_vz = 0;
                lmb_face(s, dx, dz);
                break;
            }
            s->moved = lmb_steer(s, dx, dz, LMB_WALK_SPEED, px, py, pz);
            break;

        case LMB_WINDUP:
            /* Rooted, arm up, facing the player. The arm coming down is the
               event, so the wave is launched on the first frame of the strike
               and not here. */
            lmb_face(s, dx, dz);
            if (++s->atk_tick >= LMB_WINDUP_FRAMES) {
                s->state    = LMB_STRIKE;
                s->atk_tick = 0;
                s->wave_t   = 1;
                s->wave_hit = 0;
                /* SFX_CRWL_SCRM borrowed again — see lumberer.h. */
                sound_play(SFX_CRWL_SCRM);
            }
            break;

        case LMB_STRIKE:
            /* Rooted through the recovery too. At the end it either swings
               again, if the player is still in reach, or goes back to walking
               at them — never back to the patrol. */
            lmb_face(s, dx, dz);
            if (++s->atk_tick >= LMB_STRIKE_FRAMES) {
                s->atk_tick = 0;
                if (d2 <= (int32_t)LMB_ATTACK_RADIUS * LMB_ATTACK_RADIUS)
                    s->state = LMB_WINDUP;
                else
                    s->state = LMB_ALERT;
            }
            break;

        default:
            break;
        }

        /* Tied to the body's real travel, not to a timer: a lumberer held
           against geometry stops stepping rather than marching on the spot
           (tools/ADDING_AN_ENEMY.txt mistake 15). */
        if (s->moved) s->anim_tick++;
    }

    /* --- Lumberer vs lumberer hard collision, after every one has moved. --- */
    int a, b;
    for (a = 0; a < lumberer_count; a++) {
        Lumberer *sa = &lumberers[a];
        if (!sa->active || sa->state == LMB_DEAD || sa->area != current_area) continue;
        for (b = a + 1; b < lumberer_count; b++) {
            Lumberer *sb = &lumberers[b];
            if (!sb->active || sb->state == LMB_DEAD || sb->area != current_area) continue;
            int32_t cdx  = sa->x - sb->x;
            int32_t cdz  = sa->z - sb->z;
            int32_t dist = (cdx < 0 ? -cdx : cdx) + (cdz < 0 ? -cdz : cdz);
            int32_t min_dist = LMB_BODY_RADIUS * 2;
            if (dist < min_dist && dist > 0) {
                int32_t push    = (min_dist - dist) / 2;
                int32_t push_ax = (cdx * push) / dist;
                int32_t push_az = (cdz * push) / dist;
                sa->x += push_ax; sa->z += push_az;
                sb->x -= push_ax; sb->z -= push_az;
            }
        }
    }
}

/* ---- The texture-window bracket -------------------------------------------
   Both sheets sit at Voff 128, so a room's 128-tall window would wrap their V
   mod-128 and sample the wrong half of the page. Each body therefore goes down
   under a full/unmasked window with the area's own window restored after it.

   ONE BRACKET FOR THE WHOLE BODY, not one per quad: the sprite is an
   LMB_SUBDIV x LMB_SUBDIV grid and bracketing each piece would put eight
   DR_TWINs in the packet buffer per lumberer. addPrim() PREPENDS within a
   bucket, so the sequence is: add the RESTORE first, then every piece, then the
   DISABLE last — which the GPU then draws as disable, pieces, restore. Both
   twins and all the pieces share one OT bucket, so nothing can be sorted
   between them. The caller reserves the whole body's packet space up front, so
   a buffer that runs out mid-body is impossible — a half-emitted bracket would
   leave the unmasked window switched on for the rest of the frame and
   mis-sample every textured poly after it. */
static void lmb_bracket_begin(RenderContext *ctx, int32_t otz) {
    if (!lmb_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
    setTexWindow(restore, &lmb_tw_restore);
    addPrim(&ot[otz], restore);
    ctx->next_packet += sizeof(DR_TWIN);
}

static void lmb_bracket_end(RenderContext *ctx, int32_t otz) {
    if (!lmb_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    RECT full = { 0, 0, 0, 0 };   /* mask 0 = no wrapping, full page */
    DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
    setTexWindow(disable, &full);
    addPrim(&ot[otz], disable);
    ctx->next_packet += sizeof(DR_TWIN);
}

/* Which of the six frames to draw.

   >>> THE WALK PICKS ITS PAIR FROM WHERE THE BODY IS GOING RELATIVE TO THE
   CAMERA, NOT FROM ITS STATE. <<< Frames 0/1 are the creature seen from the
   front and 2/3 are its back, so the question is whether it is coming toward
   the viewer or going away — which is the same question whether it is pacing a
   patrol or closing on the player, and is exactly how the Mushroom Head chooses
   between mushy_run and mushy_behind (mushroom.c, draw_mushrooms).

   A stationary body holds the first frame of whichever pair it is facing into.
   anim_tick only advances on frames it actually travelled, so that is also what
   a blocked one does. */
static int lmb_frame(const Lumberer *s) {
    if (s->state == LMB_WINDUP) return 4;
    if (s->state == LMB_STRIKE) return 5;

    int32_t fx  = (int16_t)(s->facing >> 16);
    int32_t fz  = (int16_t)(s->facing & 0xFFFF);
    int32_t tox = cam_x - s->x;
    int32_t toz = cam_z - s->z;
    /* Shifted down before multiplying: the two vectors are a small heading and
       a room-scale offset, and the raw product of the latter with itself would
       be the only thing at risk here. */
    int32_t dot  = fx * (tox >> 4) + fz * (toz >> 4);
    int     base = (dot > 0) ? 0 : 2;          /* toward the camera, or away */
    return base + ((s->anim_tick / LMB_ANIM_RATE) & 1);
}

_Static_assert(LMB_SUBDIV == 2, "the UV table below is written for a 2x2 grid");

/* Texel bounds of each PIECE of `frame`, in the order the grid is walked.
   Each sheet is 256x128 8bpp holding THREE frames side by side, so U runs
   0..255 across the row and V, being the row within a 256-tall page, runs
   128..255. Frame `frame` lives in sheet frame/3, in column frame%3 of it.

   The columns are 85, 85 and 86 texels wide (256 does not divide by three), so
   the bounds are computed rather than tabulated — getting a frame's left edge
   from a round 85 would walk one texel of the neighbour into the last column.

   INSET BY ONE TEXEL ON THE FRAME'S OUTER EDGES so a magnified quad's edge
   pixels cannot sample the neighbouring frame — the zombie hit exactly this as
   a strip of the wrong texture bleeding in, and here the neighbour is another
   pose of the same creature. The INTERNAL seam between the two pieces of a
   column is not inset: those texels are genuinely adjacent in the source image
   and pulling them apart would draw a hairline of nothing down the middle of
   the body. */
static void lmb_frame_uv(int frame, uint8_t ulo[LMB_SUBDIV], uint8_t uhi[LMB_SUBDIV],
                                    uint8_t vlo[LMB_SUBDIV], uint8_t vhi[LMB_SUBDIV]) {
    int col = frame % 3;
    int u0  = (col * 256) / 3;          /* 0, 85, 170 — the crop this shipped with */
    int u1  = ((col + 1) * 256) / 3;    /* 85, 170, 256                            */
    int um  = (u0 + u1) / 2;
    ulo[0] = (uint8_t)(u0 + 1);  uhi[0] = (uint8_t)(um - 1);
    ulo[1] = (uint8_t)um;        uhi[1] = (uint8_t)(u1 - 2);
    vlo[0] = 129;                vhi[0] = 191;
    vlo[1] = 192;                vhi[1] = 254;
}

static void draw_lmb_shadow(RenderContext *ctx, Lumberer *s) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int32_t  shade;
    if (!shadow_tpage) return;
    if (ctx->next_packet + sizeof(DR_TPAGE) + sizeof(POLY_FT4) > buf_end) return;

    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((LMB_SHADOW_W * rx) >> 12);
    int16_t dwz = (int16_t)((LMB_SHADOW_W * rz) >> 12);

    int32_t fx  = isin(cam_rot);
    int32_t fz  = icos(cam_rot);
    int16_t ddx = (int16_t)((LMB_SHADOW_D * fx) >> 12);
    int16_t ddz = (int16_t)((LMB_SHADOW_D * fz) >> 12);

    int32_t shadow_y = s->y + LMB_Y_OFFSET + LMB_HALF_H - 2;

    SVECTOR sv[4];
    sv[0].vx = (int16_t)(s->x - dwx - ddx); sv[0].vy = (int16_t)shadow_y; sv[0].vz = (int16_t)(s->z - dwz - ddz); sv[0].pad = 0;
    sv[1].vx = (int16_t)(s->x + dwx - ddx); sv[1].vy = (int16_t)shadow_y; sv[1].vz = (int16_t)(s->z + dwz - ddz); sv[1].pad = 0;
    sv[2].vx = (int16_t)(s->x - dwx + ddx); sv[2].vy = (int16_t)shadow_y; sv[2].vz = (int16_t)(s->z - dwz + ddz); sv[2].pad = 0;
    sv[3].vx = (int16_t)(s->x + dwx + ddx); sv[3].vy = (int16_t)shadow_y; sv[3].vz = (int16_t)(s->z + dwz + ddz); sv[3].pad = 0;

    DVECTOR ssv[4];
    int32_t otz;

    gte_ldv0(&sv[0]); gte_rtps(); gte_stsxy(&ssv[0]);
    gte_ldv0(&sv[1]); gte_rtps(); gte_stsxy(&ssv[1]);
    gte_ldv0(&sv[2]); gte_rtps(); gte_stsxy(&ssv[2]);
    gte_ldv0(&sv[3]); gte_rtps(); gte_stsxy(&ssv[3]);

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return;
    otz += 2;                      /* just in front of the floor poly below it */
    if (otz >= OT_LENGTH - 2) otz = OT_LENGTH - 3;

    /* The shadow fogs on the body's own curve, so the two disappear together
       rather than leaving a crisp black patch where a fogged-out body stood.
       The scale is applied to 128 — the PS1's 1.0x modulation, and what this
       quad has always been drawn at — rather than used as the colour, because
       render_fog_scale returns 256 at the near plane. */
    {
        int32_t sdx = s->x - cam_x, sdz = s->z - cam_z;
        int32_t sd  = (sdx < 0 ? -sdx : sdx) + (sdz < 0 ? -sdz : sdz);
        if (sd >= g_fog_far) return;
        shade = (128 * render_fog_scale(sd)) >> 8;
        if (shade > 255) shade = 255;
        if (shade < 0)   shade = 0;
    }

    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 1, shadow_tpage);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz + 1], tp);
    ctx->next_packet += sizeof(DR_TPAGE);

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, (uint8_t)shade, (uint8_t)shade, (uint8_t)shade);

    poly->x0 = ssv[0].vx; poly->y0 = ssv[0].vy;
    poly->x1 = ssv[1].vx; poly->y1 = ssv[1].vy;
    poly->x2 = ssv[2].vx; poly->y2 = ssv[2].vy;
    poly->x3 = ssv[3].vx; poly->y3 = ssv[3].vy;

    /* Shadow texture at VRAM (640,160): tpage base y=0, so V offset = 160 */
    poly->u0 =  0; poly->v0 = 160;
    poly->u1 = 63; poly->v1 = 160;
    poly->u2 =  0; poly->v2 = 191;
    poly->u3 = 63; poly->v3 = 191;

    poly->clut  = shadow_clut;
    poly->tpage = shadow_tpage;

    ctx->next_packet += sizeof(POLY_FT4);
    /* shadow.tim is at VRAM y=160, i.e. Voff 160, so it needs the same bracket
       the body does. One quad, so the begin/end pair wraps just this poly. */
    lmb_bracket_begin(ctx, otz);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    lmb_bracket_end(ctx, otz);
}

/* One camera-facing body, emitted as an LMB_SUBDIV x LMB_SUBDIV grid of quads
   sharing one OT bucket and one texture-window bracket. See LMB_SUBDIV for why
   it is not a single quad.

   NO gte_nclip BACKFACE CULL: a camera-facing quad has no back face, and the
   cull is what threw the spider's rolled sprite away (mistake 4). */
static void draw_lmb_sprite(RenderContext *ctx, Lumberer *s, int frame) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);
    int     gi, gj;

    int32_t cx = s->x, cz = s->z, cy = s->y + LMB_Y_OFFSET;

    int16_t dwx = (int16_t)((LMB_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((LMB_HALF_W * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - LMB_HALF_H);
    int16_t vy_bot = (int16_t)(cy + LMB_HALF_H);

    /* The ring TL, TR, BR, BL, which is what the grid below interpolates. */
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
    /* Sort on the room-geometry scale (raw average Z) so walls between the
       camera and the body occlude it, while the ~40-unit gap the mesh adds to
       its floor polys keeps the sprite off the ground. ONE bucket for the whole
       body: the pieces are coplanar and cannot need sorting against each other,
       and sharing a bucket is what lets them share one bracket. */
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    int32_t fdx  = s->x - cam_x;
    int32_t fdz  = s->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    if (dist >= g_fog_far) return;          /* fully fogged: cull */
    int32_t fs   = render_fog_scale(dist);
    uint8_t fog8 = fs > 255 ? 255 : (uint8_t)fs;

    /* RESERVE THE WHOLE BODY UP FRONT — see the bracket note above. */
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    size_t   need    = (size_t)(LMB_SUBDIV * LMB_SUBDIV) * sizeof(POLY_FT4)
                     + (lmb_tw_active ? 2 * sizeof(DR_TWIN) : 0);
    if (ctx->next_packet + need > buf_end) return;

    /* (LMB_SUBDIV+1)^2 points, bilinearly interpolated between the corners. */
    SVECTOR  g[(LMB_SUBDIV + 1) * (LMB_SUBDIV + 1)];
    DVECTOR  gs[(LMB_SUBDIV + 1) * (LMB_SUBDIV + 1)];
    for (gi = 0; gi <= LMB_SUBDIV; gi++) {
        int32_t tx = v[0].vx + ((int32_t)(v[1].vx - v[0].vx) * gi) / LMB_SUBDIV;
        int32_t ty = v[0].vy + ((int32_t)(v[1].vy - v[0].vy) * gi) / LMB_SUBDIV;
        int32_t tz = v[0].vz + ((int32_t)(v[1].vz - v[0].vz) * gi) / LMB_SUBDIV;
        int32_t bx = v[3].vx + ((int32_t)(v[2].vx - v[3].vx) * gi) / LMB_SUBDIV;
        int32_t by = v[3].vy + ((int32_t)(v[2].vy - v[3].vy) * gi) / LMB_SUBDIV;
        int32_t bz = v[3].vz + ((int32_t)(v[2].vz - v[3].vz) * gi) / LMB_SUBDIV;
        for (gj = 0; gj <= LMB_SUBDIV; gj++) {
            SVECTOR *p = &g[gj * (LMB_SUBDIV + 1) + gi];
            p->vx  = (int16_t)(tx + ((bx - tx) * gj) / LMB_SUBDIV);
            p->vy  = (int16_t)(ty + ((by - ty) * gj) / LMB_SUBDIV);
            p->vz  = (int16_t)(tz + ((bz - tz) * gj) / LMB_SUBDIV);
            p->pad = 0;
        }
    }
    {
        int n = (LMB_SUBDIV + 1) * (LMB_SUBDIV + 1), k;
        for (k = 0; k < n; k++) {
            gte_ldv0(&g[k]);
            gte_rtps();
            gte_stsxy(&gs[k]);
        }
    }

    uint8_t ulo[LMB_SUBDIV], uhi[LMB_SUBDIV], vlo[LMB_SUBDIV], vhi[LMB_SUBDIV];
    lmb_frame_uv(frame, ulo, uhi, vlo, vhi);

    uint16_t tpage = lmb_tpage[frame / 3];
    uint16_t clut  = lmb_clut [frame / 3];

    lmb_bracket_begin(ctx, otz);
    for (gj = 0; gj < LMB_SUBDIV; gj++) {
        for (gi = 0; gi < LMB_SUBDIV; gi++) {
            DVECTOR *tl = &gs[ gj      * (LMB_SUBDIV + 1) + gi    ];
            DVECTOR *tr = &gs[ gj      * (LMB_SUBDIV + 1) + gi + 1];
            DVECTOR *bl = &gs[(gj + 1) * (LMB_SUBDIV + 1) + gi    ];
            DVECTOR *br = &gs[(gj + 1) * (LMB_SUBDIV + 1) + gi + 1];

            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            if (s->hit_timer > 0) setRGB0(poly, fog8, fog8 >> 2, fog8 >> 2);
            else                  setRGB0(poly, fog8, fog8, fog8);

            /* POLY_FT4 wants the quad in Z order: TL, TR, BL, BR. */
            poly->x0 = tl->vx; poly->y0 = tl->vy;
            poly->x1 = tr->vx; poly->y1 = tr->vy;
            poly->x2 = bl->vx; poly->y2 = bl->vy;
            poly->x3 = br->vx; poly->y3 = br->vy;

            poly->u0 = ulo[gi]; poly->v0 = vlo[gj];
            poly->u1 = uhi[gi]; poly->v1 = vlo[gj];
            poly->u2 = ulo[gi]; poly->v2 = vhi[gj];
            poly->u3 = uhi[gi]; poly->v3 = vhi[gj];

            poly->tpage = tpage;
            poly->clut  = clut;

            ctx->next_packet += sizeof(POLY_FT4);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        }
    }
    lmb_bracket_end(ctx, otz);

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

    int16_t fill_w = (int16_t)((s->health * 40) / LMB_MAX_HEALTH);
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

/* One flat-shaded, additively blended quad of the shockwave. The Rabisu's
   rbs_glow_quad without the boss's colour pulse: this wave is out for well
   under a second and a shimmer inside that reads as flicker rather than as
   energy. No backface cull — every face is the same flat colour, so the
   silhouette is identical whichever way the winding runs. */
static void lmb_wave_quad(RenderContext *ctx, const SVECTOR v[4],
                          uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

    DVECTOR sv[4];
    int32_t sz[4], otz;
    int k;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_stsz4c(sz);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz(&sz[3]);
    if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) return;

    /* The GPU DROPS a primitive whose screen extent passes 1023 pixels, so a
       segment of the ring seen from inside it has to be thrown away here rather
       than left to vanish for a frame. */
    for (k = 0; k < 4; k++)
        if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
            sv[k].vy <= -1023 || sv[k].vy >= 1023) return;

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= SCENE_OT_MIN) return;
    otz += 40;   /* the room mesh's own bias, so a wall in front still occludes */
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
    setPolyF4(p);
    setSemiTrans(p, 1);
    setRGB0(p, r, g, b);
    p->x0 = sv[0].vx; p->y0 = sv[0].vy;
    p->x1 = sv[1].vx; p->y1 = sv[1].vy;
    p->x2 = sv[2].vx; p->y2 = sv[2].vy;
    p->x3 = sv[3].vx; p->y3 = sv[3].vy;
    addPrim(&ot[otz], p);
    ctx->next_packet += sizeof(POLY_F4);

    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
    addPrim(&ot[otz], tp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

/* The wave: a bright annulus racing outward across the floor with a short
   vertical skirt riding its leading edge. The ring says where the wave IS; the
   skirt is the part the player actually sees coming, because in first person a
   flat figure on the floor disappears the moment it is more than a few metres
   away. Same construction as draw_rbs_shockwave.

   IT IS DRAWN AT THE BODY'S OWN FOOT HEIGHT, FLAT. The Rabisu's looks up a
   floor height per corner because its arena is terraced; the rooms this enemy
   stands in are single-plane (src/tomb.h), so one Y is correct and a per-corner
   probe would only cost GTE time. Placing one in a stepped room means giving
   this the floor probe, not moving the enemy. */
static void draw_lmb_shockwave(RenderContext *ctx, const Lumberer *s) {
    int32_t reach = s->wave_t >= LMB_SHOCK_EXPAND
                  ? LMB_SHOCK_RADIUS
                  : (LMB_SHOCK_RADIUS * s->wave_t) / LMB_SHOCK_EXPAND;
    if (reach <= 0) return;

    /* Full brightness while it is still travelling, then it dies back over the
       linger rather than blinking out at full reach. */
    int32_t bright = 256;
    if (s->wave_t > LMB_SHOCK_EXPAND) {
        int32_t k = s->wave_t - LMB_SHOCK_EXPAND;
        bright = 256 - (k * 256) / LMB_SHOCK_LINGER;
        if (bright < 0) bright = 0;
    }
    if (bright <= 0) return;

    int32_t inner = reach - LMB_SHOCK_BAND;
    if (inner < 0) inner = 0;

    /* A cold, dusty white — the wave is a shove of air off a stone floor, not
       the Rabisu's fire. */
    uint8_t gr = (uint8_t)((110 * bright) >> 8);
    uint8_t gg = (uint8_t)((100 * bright) >> 8);
    uint8_t gb = (uint8_t)(( 86 * bright) >> 8);
    uint8_t wr = (uint8_t)((160 * bright) >> 8);
    uint8_t wg = (uint8_t)((148 * bright) >> 8);
    uint8_t wb = (uint8_t)((128 * bright) >> 8);

    /* The floor under its feet. LMB_Y_OFFSET + LMB_HALF_H is the drop from the
       anchor to the soles, and the extra 4 lifts the ring clear of the floor
       poly it is drawn over. */
    int32_t fy = s->y + LMB_Y_OFFSET + LMB_HALF_H - 4;

    SVECTOR v[4];
    int k, seg;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    for (seg = 0; seg < LMB_SHOCK_SEGS; seg++) {
        int32_t a0 = (seg * 4096) / LMB_SHOCK_SEGS;
        int32_t a1 = ((seg + 1) * 4096) / LMB_SHOCK_SEGS;
        int32_t s0 = isin(a0), c0 = icos(a0);
        int32_t s1 = isin(a1), c1 = icos(a1);

        int32_t ix0 = s->x + (s0 * inner >> 12), iz0 = s->z + (c0 * inner >> 12);
        int32_t ix1 = s->x + (s1 * inner >> 12), iz1 = s->z + (c1 * inner >> 12);
        int32_t ox0 = s->x + (s0 * reach >> 12), oz0 = s->z + (c0 * reach >> 12);
        int32_t ox1 = s->x + (s1 * reach >> 12), oz1 = s->z + (c1 * reach >> 12);

        /* The band on the ground. */
        v[0].vx = (int16_t)ix0; v[0].vy = (int16_t)fy; v[0].vz = (int16_t)iz0;
        v[1].vx = (int16_t)ox0; v[1].vy = (int16_t)fy; v[1].vz = (int16_t)oz0;
        v[2].vx = (int16_t)ix1; v[2].vy = (int16_t)fy; v[2].vz = (int16_t)iz1;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)fy; v[3].vz = (int16_t)oz1;
        lmb_wave_quad(ctx, v, gr, gg, gb);

        /* The skirt standing on its leading edge. -Y is up. */
        v[0].vx = (int16_t)ox0; v[0].vy = (int16_t)(fy - LMB_SHOCK_WALL_H); v[0].vz = (int16_t)oz0;
        v[1].vx = (int16_t)ox1; v[1].vy = (int16_t)(fy - LMB_SHOCK_WALL_H); v[1].vz = (int16_t)oz1;
        v[2].vx = (int16_t)ox0; v[2].vy = (int16_t)fy;                      v[2].vz = (int16_t)oz0;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)fy;                      v[3].vz = (int16_t)oz1;
        lmb_wave_quad(ctx, v, wr, wg, wb);
    }
}

void draw_lumberers(RenderContext *ctx) {
    if (!tex_loaded) return;
    int i;
    for (i = 0; i < lumberer_count; i++) {
        Lumberer *s = &lumberers[i];
        if (!s->active || s->state == LMB_DEAD || s->area != current_area) continue;

        int32_t dx = s->x - cam_x;
        int32_t dz = s->z - cam_z;
        int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

        /* Distance cull at the ROOM'S OWN fog. The body already culls itself at
           g_fog_far (draw_lmb_sprite), so past that there is nothing here but a
           shadow under fog that has taken the floor around it to black. */
        if (wdist >= g_fog_far) continue;

        /* Behind a wall: skip body, shadow AND wave. See collision.h — the OT
           sort makes the wall paint over the sprite but the GPU has already
           filled it, and a 346x520 billboard is a real fill. The exemption is
           the same one the Mushroom Head makes: anything inside the radius at
           which it can reach the player is already in their fight, whatever is
           standing in between. The 3/2 converts the Manhattan wdist to cover a
           true radius on every bearing. */
        if (wdist > (LMB_SHOCK_RADIUS * 3) / 2 &&
            collision_hidden_from_camera(s->x, s->y + LMB_Y_OFFSET, s->z))
            continue;

        if (s->wave_t > 0) draw_lmb_shockwave(ctx, s);

        draw_lmb_shadow(ctx, s);
        draw_lmb_sprite(ctx, s, lmb_frame(s));
    }
}
