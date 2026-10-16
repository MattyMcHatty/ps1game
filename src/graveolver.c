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
#include "crawler.h"
#include "tentacle.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "lumberer.h"
#include "gula.h"
#include "lamashtu.h"     /* the one spot on her back that can be hurt */
#include "creep.h"
#include "lms_creep.h"
#include "maggot.h"
#include "rabisu.h"
#include "asag.h"          /* asag_head_box - where the head is         */
#include "asag_fight.h"    /* ...and whether hitting it does anything   */
#include "vampire.h"
#include "particles.h"
#include "sound.h"
#include "bullet_hit.h"
#include "graveolver.h"
#include "weapon.h"
#include "vines.h"      /* the one PROP the gun can destroy */
#include "damage.h"
#include "menu.h"       /* the wheel's boxes show the inventory's ammo icons */

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
/* The aim field's SIZE is per ammo type now - ammo_info[].aim_hw/aim_hh in
   player.c (14 x 14 for Standard and Flame, the old GUN_AIM_RADIUS). */
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

/* --- Reloading and ammo swapping (R2) ---------------------------------------
   R2 has two jobs, told apart by how long it is held:
     TAP  (released before WHEEL_HOLD_FRAMES)  tops the cylinder up to
          GRAVEOLVER_CAPACITY from the chambered type's reserve.
     HOLD (WHEEL_HOLD_FRAMES or longer)        opens the ammo wheel; the d-pad
          picks a box and releasing R2 loads that type.
   Both are decided on RELEASE except the wheel opening itself, so a tap never
   flashes the wheel up and a hold never starts a top-up first.

   Swapping the chambered type costs a FULL reload: the same timer, dip and
   sound as a normal reload, with the type change applied only when the timer
   reaches 0. reload_to is the type the cylinder will hold once it completes;
   for an ordinary top-up it equals graveolver_ammo, so both cases share one
   completion path.

   swap_pending distinguishes the two so the "Loaded ..." log line is posted
   only for a real type change, and only on completion — never at the moment
   R2 is released. Cancelling (a weapon switch mid-reload) drops both, leaving
   the cylinder exactly as it was. */
static AmmoType reload_to     = AMMO_STANDARD;
static int      swap_pending  = 0;

/* --- The ammo wheel ----------------------------------------------------------
   Four boxes in a cross around the screen centre, one per d-pad direction; the
   boxes' layout and look are the shared selection wheel's (weapon.h), which the
   weapon wheel on Triangle uses too. wheel_slots[] is the ONLY place that says
   which type sits where; a box whose ammo is -1 is a placeholder for a type
   that does not exist yet and can never be picked. ADDING AN AMMO TYPE: give it the next free box here.

   A box can be picked when its type is the chambered one (picking it back is
   how a player changes their mind) or the player holds reserve rounds of it.
   Anything else ignores the d-pad, so an empty box simply does nothing. */
static const struct {
    int8_t ammo;        /* AmmoType, or -1 for a box with nothing in it yet */
    int8_t menu_slot;   /* its inventory icon (MENU_SLOT_*)                 */
} wheel_slots[WHEEL_SLOTS] = {
    [WHEEL_UP]    = { AMMO_STANDARD, MENU_SLOT_ROUNDS         },
    [WHEEL_LEFT]  = { AMMO_FLAME,    MENU_SLOT_FLAME_ROUNDS   },
    [WHEEL_RIGHT] = { AMMO_SCATTER,  MENU_SLOT_SCATTER_ROUNDS },
    [WHEEL_DOWN]  = { -1,            -1                       },
};

static int r2_frames   = -1;  /* frames R2 has been down; -1 = not armed       */
static int wheel_open  = 0;
static int wheel_pick  = WHEEL_UP;
/* Set by graveolver_update on every frame the wheel is open and consumed by the
   draw. A frame on which the update did not run at all (a cutscene took it)
   therefore draws no wheel, rather than leaving the last one parked on screen. */
static int wheel_show  = 0;

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
   supplies its aim field and GUN_RANGE where those functions were previously
   reading them out of this file. */

/* ---- One shot's state --------------------------------------------------------
   Set by graveolver_fire for the length of one shot and read by consider(), so
   the target loops below each stay one call long. The field and the spread
   come from the chambered type's row in ammo_info[] (player.h) - which is the
   only place that says a Scattershot is different. */
static int32_t    shot_fx, shot_fz;    /* isin/icos(cam_rot)                     */
static int32_t    shot_rx, shot_ry;    /* aim field half-extents, pixels         */
static int        shot_spread;         /* 1 = hit everything in the field        */
static DamageType shot_dmg;
static int        best_kind, best_idx;
static int32_t    best_depth;

/* How consider() decides whether a target is behind a wall. */
enum {
    CLR_LINE,   /* the ordinary test                                          */
    CLR_BOIL,   /* Asag's boils: cleared short of themselves (asag_fight.h)   */
    CLR_NONE    /* a vine curtain: it IS the prop the test would trip on      */
};

/* Apply one round to one target. `kind`/`idx` are the codes the loops in
   graveolver_fire hand to consider(); the per-target notes live here. */
static void gun_hit(int kind, int idx) {
    DamageType dmg_type = shot_dmg;

    if (kind == 0) {
        demon_dog_damage(&demon_dogs[idx],
                   demon_dog_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 1) {
        zombie_damage(&zombies[idx],
                      zombie_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 3) {
        tentacle_shoot(&tentacles[idx],
                       tentacle_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 4) {
        spider_damage(&spiders[idx],
                      spider_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 11) {
        /* A CRAWLER. Its weakness table's one entry is DMG_HOLY and no round
           this gun chambers carries that type, so every shot does the flat 1 —
           the table is asked all the same, the way every other enemy's is, so
           that a second entry reaches the gun without anyone coming back here.
           Six rounds to kill, and each one also sends an advancing crawler into
           its retreat (see crawler_damage), which is what makes the gun the
           tool for keeping one off you rather than the tool for killing it. */
        crawler_damage(&crawlers[idx],
                       crawler_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 6) {
        /* 1 from a Standard Round, 2 from a Flame Round (its weakness table
           doubles DMG_FLAME) — so four shots to kill, or two. */
        rafflesia_shoot(&rafflesias[idx],
                        rafflesia_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 7) {
        /* No weaknesses: 1 from any round, so five shots to kill whatever is
           chambered (see mushroom.h). */
        mushroom_damage(&mushrooms[idx],
                        mushroom_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 12) {
        /* No weaknesses: 1 from any round, so ten shots to kill whatever is
           chambered (see lumberer.h). Unlike the crawler, a round does NOT move
           it — this one has no retreat and nothing about being shot changes
           what it is doing, beyond waking a patrolling one. */
        lumberer_damage(&lumberers[idx],
                        lumberer_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 13) {
        /* A CREEP. One hit point and no weakness table worth the name, so any
           round of any type kills it outright. creep_scale_damage is asked
           anyway, the way every other enemy here is asked, so that giving the
           thing a weakness later reaches the gun without anyone coming back to
           this line — it would take more than one shot before it mattered. */
        creep_damage(&creeps[idx],
                     creep_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 14) {
        /* A MAGGOT: 2 HP and every round type at 1x, so two shots. */
        maggot_damage(&maggots[idx],
                      maggot_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 8) {
        /* Not an enemy and not scaled by a weakness table: a curtain asks the
           DAMAGE TYPE directly. DMG_FLAME clears a destructible one outright,
           a Standard Round does nothing but scatter leaves. */
        vines_shoot(idx, (int)dmg_type);
    } else if (kind == 5) {
        /* Boss: 1 from a Standard Round, 2 from a Flame Round (its weakness
           table doubles DMG_FLAME) — so 20 or 10 shots to kill. */
        rabisu_damage(&rabisus[idx],
                      rabisu_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 17) {
        /* A LAMASHTU CREEP: the Creep's one hit point, so any round. */
        lms_creep_damage(&lms_creeps[idx],
                         lms_creep_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 16) {
        /* Lamashtu's spot: 1 from any round, no weakness table - every weapon
           is 1x against her (src/lamashtu.h), so ten rounds. */
        lamashtu_damage(GUN_DAMAGE);
    } else if (kind == 15) {
        /* Gula: 1 from any round - her table only names DMG_HOLY. */
        gula_damage(gula_scale_damage(GUN_DAMAGE, dmg_type));
    } else if (kind == 9) {
        /* ASAG'S HEAD. >>> NO WEAKNESS TABLE AND NO damage_scale() CALL. <<<
           "Every weapon deals 1x damage to Asag" was the brief, in as many
           words, so a Flame Round is a Standard Round here and the absence of a
           table is the design rather than an omission. 20 HP means twenty
           connected shots, all of them landed inside exposure windows that add
           up to a few seconds an attack — which is what makes the boils, and
           the faint they buy, the way through the fight rather than a
           decoration on it. */
        asag_damage(GUN_DAMAGE);
    } else if (kind == 10) {
        /* A BOIL. 3 HP, so three rounds of anything this gun chambers — the
           boils DO have a weakness table now, but its one entry is DMG_HOLY
           and no round carries that type. It is asked all the same, the way
           every other enemy above is asked, so a second entry added to that
           table reaches the gun without anyone having to come back here. */
        asag_boil_damage(idx,
                         asag_boil_scale_damage(GUN_DAMAGE, dmg_type));
    } else {
        vampire_health   -= vampire_scale_damage(GUN_DAMAGE, dmg_type);
        vampire_hit_timer = VAMPIRE_BAR_TIMER_MAX;
        if (vampire_health <= 0)
            spawn_blood_burst(vampire_x, vampire_y, vampire_z);
    }
}

/* The point `back` units short of (ex,cy,ez), on the line from the camera to
   it, never pulled back past halfway. `depth` is the point's forward depth. */
static void toward_camera(int32_t ex, int32_t cy, int32_t ez, int32_t depth,
                          int32_t back, int32_t *px, int32_t *py, int32_t *pz) {
    int32_t keep;
    if (back > depth / 2) back = depth / 2;
    if (back < 0)         back = 0;
    keep = depth - back;
    *px = cam_x + ((ex - cam_x) * keep) / depth;
    *py = cam_y + ((cy - cam_y) * keep) / depth;
    *pz = cam_z + ((ez - cam_z) * keep) / depth;
}

/* One candidate. Every target the gun can hit comes through here, so the two
   ways a round can behave are written once:

     NEAREST (spread 0 - Standard and Flame): the round goes to the closest
     thing in the field whose CROSSHAIR line is clear; graveolver_fire applies
     it after the sweep. This is the gun as it always was.

     SPREAD (spread 1 - the Scattershot): every thing in the field is hit, here
     and now, if the line from the camera to IT is clear. Not the crosshair
     line: the field is 3/5 of the screen wide, and a zombie 90 px left of the
     crosshair has nothing to do with a pillar the crosshair is on. The line
     stops short of the body by its own half-width, so a target standing
     against a wall is judged by what is in front of it, not the wall behind.
     A vine curtain is a prop on that line, so it stops the pellets meant for
     whatever is behind it, as a wall would. */
static void consider(int kind, int idx, int32_t ex, int32_t cy, int32_t ez,
                     int32_t hw, int32_t hh, int32_t range, int clear) {
    int32_t depth;
    if (!weapon_aim_in_box(ex, cy, ez, hw, hh, shot_fx, shot_fz,
                           shot_rx, shot_ry, range, &depth))
        return;

    if (shot_spread) {
        int32_t px, py, pz;
        if (clear == CLR_BOIL)
            toward_camera(ex, cy, ez, depth,
                          depth - asag_boil_clear_depth(ez, depth), &px, &py, &pz);
        else
            toward_camera(ex, cy, ez, depth, hw, &px, &py, &pz);
        if (clear != CLR_NONE && !weapon_aim_clear_to(px, py, pz)) return;
        gun_hit(kind, idx);
        toward_camera(ex, cy, ez, depth, GUN_HIT_BACKOFF, &px, &py, &pz);
        bullet_hit_spawn(px, py, pz);
        return;
    }

    if (depth >= best_depth) return;
    if (clear == CLR_LINE && !weapon_aim_clear(shot_fx, shot_fz, depth)) return;
    if (clear == CLR_BOIL &&
        !weapon_aim_clear(shot_fx, shot_fz, asag_boil_clear_depth(ez, depth)))
        return;
    best_depth = depth; best_kind = kind; best_idx = idx;
}

/* Fire one round: flash + hitscan under the reticule - the nearest enemy, or
   for a Scattershot every enemy in its field (see consider). The caller has
   already confirmed a round is chambered and spends it. */
static void graveolver_fire(void) {
    const AmmoInfo *ai = &ammo_info[graveolver_ammo];
    int i;
    muzzle_flash = GUN_FLASH_FRAMES;
    recoil_timer = GRAV_RECOIL_FRAMES;
    /* The flash colour is the chambered ammo's — white for Standard, orange for
       Flame, light green for Scattershot. Latched here rather than read at draw
       time (see flash_r). */
    flash_r = ai->flash_r;
    flash_g = ai->flash_g;
    flash_b = ai->flash_b;
    sound_play(SFX_GR_SHOT);

    shot_fx     = isin(cam_rot);
    shot_fz     = icos(cam_rot);
    shot_rx     = ai->aim_hw;
    shot_ry     = ai->aim_hh;
    shot_spread = ai->spread;
    /* One round is one point of BASE damage; each enemy scales it by its own
       weakness to the chambered ammo's damage type (see damage.h). Flame Rounds
       are not stronger in general — only against what burns. A Scattershot
       deals that same one point to EACH thing it hits. */
    shot_dmg    = ai->damage;
    best_kind   = -1;
    best_idx    = -1;
    best_depth  = 0x7fffffff;

    for (i = 0; i < demon_dog_count; i++) {
        DemonDog *d = &demon_dogs[i];
        if (!d->active || d->state == DDOG_DEAD) continue;
        consider(0, i, d->x, d->y + DDOG_Y_OFFSET, d->z,
                 DDOG_HALF_W, DDOG_HALF_H, GUN_RANGE, CLR_LINE);
    }
    for (i = 0; i < zombie_count; i++) {
        Zombie *z = &zombies[i];
        if (!z->active || z->state == ZMB_DEAD) continue;
        consider(1, i, z->x, z->y + ZMB_Y_OFFSET, z->z,
                 ZMB_HALF_W, ZMB_HALF_H, GUN_RANGE, CLR_LINE);
    }
    for (i = 0; i < spider_count; i++) {
        Spider *s = &spiders[i];
        if (!s->active || s->state == SPD_DEAD || s->area != current_area) continue;
        consider(4, i, s->x, s->y + SPD_Y_OFFSET, s->z,
                 SPD_HALF_W, SPD_HALF_H, GUN_RANGE, CLR_LINE);
    }
    /* The Catacombs' crawler. Aimed at wherever it happens to be — the circle
       test is given the body's real centre, so one clinging to a wall is shot
       off it exactly as a grounded one is shot. */
    for (i = 0; i < crawler_count; i++) {
        Crawler *s = &crawlers[i];
        if (!s->active || s->state == CRW_DEAD || s->area != current_area) continue;
        consider(11, i, s->x, s->y + CRW_Y_OFFSET, s->z,
                 CRW_HALF_W, CRW_HALF_H, GUN_RANGE, CLR_LINE);
    }
    for (i = 0; i < tentacle_count; i++) {
        Tentacle *t = &tentacles[i];
        if (!t->active || t->health <= 0 || t->area != current_area) continue;
        int32_t cyc, hh, hw;
        tentacle_body(t, &cyc, &hh, &hw);
        consider(3, i, t->x, cyc, t->z, hw, hh, GUN_RANGE, CLR_LINE);
    }
    for (i = 0; i < rafflesia_count; i++) {
        Rafflesia *rf = &rafflesias[i];
        if (!rf->active || rf->health <= 0 || rf->area != current_area) continue;
        int32_t cyc, hh, hw;
        rafflesia_body(rf, &cyc, &hh, &hw);
        consider(6, i, rf->x, cyc, rf->z, hw, hh, GUN_RANGE, CLR_LINE);
    }
    for (i = 0; i < mushroom_count; i++) {
        Mushroom *m = &mushrooms[i];
        if (!m->active || m->state == MSH_DEAD || m->area != current_area) continue;
        int32_t cyc, hh, hw;
        mushroom_body(m, &cyc, &hh, &hw);
        consider(7, i, m->x, cyc, m->z, hw, hh, GUN_RANGE, CLR_LINE);
    }
    /* The Catacombs' lumberer. Like the crawler's loop above, aimed at wherever
       it happens to be — the circle is built from lumberer_body(), so a body
       rooted mid-swing is exactly as hittable as a walking one. */
    for (i = 0; i < lumberer_count; i++) {
        Lumberer *l = &lumberers[i];
        if (!l->active || l->state == LMB_DEAD || l->area != current_area) continue;
        int32_t cyc, hh, hw;
        lumberer_body(l, &cyc, &hh, &hw);
        consider(12, i, l->x, cyc, l->z, hw, hh, GUN_RANGE, CLR_LINE);
    }
    /* The Creeps the Room of Arms' crib pours out. The circle is built from the
       same half-extents the sprite is drawn at, centred on creep_body_y() —
       which is where the thing actually IS. A creep FLOATS at eye level while
       cr->y is its floor anchor twenty-odd units below the player's eye, so
       aiming at the anchor would put the crosshair under the body. That function
       is the one place the hover is resolved (src/creep.h); do not open-code the
       offset here. */
    for (i = 0; i < creep_count; i++) {
        Creep *cr = &creeps[i];
        if (!cr->active || cr->state == CRP_DEAD || cr->area != current_area) continue;
        consider(13, i, cr->x, creep_body_y(cr), cr->z,
                 CRP_HALF_W, CRP_HALF_H, GUN_RANGE, CLR_LINE);
    }
    /* ...and Lamashtu's (src/lms_creep.h), the same loop at their own size. */
    for (i = 0; i < lms_creep_count; i++) {
        LmsCreep *lc = &lms_creeps[i];
        if (!lc->active || lc->state == LCR_DEAD || lc->area != current_area) continue;
        consider(17, i, lc->x, lms_creep_body_y(lc), lc->z,
                 LCR_HALF_W, LCR_HALF_H, GUN_RANGE, CLR_LINE);
    }
    /* The Maggots — the creep's loop, aimed through maggot_body_y() for the
       creep's reason: it floats at eye level above its floor anchor. */
    for (i = 0; i < maggot_count; i++) {
        Maggot *mg = &maggots[i];
        if (!mg->active || mg->state == MGT_DEAD || mg->area != current_area) continue;
        consider(14, i, mg->x, maggot_body_y(mg), mg->z,
                 MGT_HALF_W, MGT_HALF_H, GUN_RANGE, CLR_LINE);
    }
    /* Gula, the Kennels' one-off (src/gula.h): one body, rooted, hittable only
       while she fights - not while she fades in, not while she dies. */
    if (gula_hittable()) {
        int32_t gx, gy, gz, ghh, ghw;
        gula_body(&gx, &gy, &gz, &ghh, &ghw);
        consider(15, 0, gx, gy, gz, ghw, ghh, GUN_RANGE, CLR_LINE);
    }
    /* LAMASHTU (src/lamashtu.h): ONLY the glowing spot on her back, and only
       from behind her - lamashtu_anus_target() says no from anywhere else, so
       a round at her face goes through her as Asag's body lets rounds through
       his. Nothing else on her is in this list, on purpose. */
    {
        int32_t lx, ly, lz, lw, lh;
        if (lamashtu_anus_target(&lx, &ly, &lz, &lw, &lh))
            consider(16, 0, lx, ly, lz, lw, lh, GUN_RANGE, CLR_LINE);
    }
    /* NO ADAPA LOOP, AND THAT IS THE DESIGN, NOT AN OVERSIGHT. Only the
       Helluminator can hurt him (src/adapa.h), so a round goes through him as
       if he were not there — he is a ghost — and is spent on whatever is
       behind. helluminator.c's sweep is the one list he is in. */
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *rb = &rabisus[i];
        /* `dying` as well as `dead`: the boss stays on screen through its whole
           death sequence, and a round spent into a corpse should miss. */
        if (!rb->active || rb->dead || rb->dying || rb->area != current_area) continue;
        int32_t cyc, hh, hw;
        rabisu_body(rb, &cyc, &hh, &hw);
        consider(5, i, rb->x, cyc, rb->z, hw, hh, GUN_RANGE, CLR_LINE);
    }
    /* ---- ASAG: THE HEAD WHILE IT IS EXPOSED, AND THE TWO BOILS ------------
       >>> THE HEAD IS ONLY A TARGET WHILE asag_exposed(). <<< That is the whole
       design of this fight: hitting the body never does anything, and the head
       can be hurt only in the windows the attacks open (src/asag_fight.h has
       the table). Testing the box alone would make the boss killable from the
       first frame with nothing on screen to explain it.

       And it is tested BEFORE the boils on purpose, so that a head over a boil
       — which happens, his alcove sits between them — wins the crosshair when
       it is actually vulnerable. `depth < best_depth` sorts the rest. */
    if (asag_exposed()) {
        int32_t hx, hy, hz, hw, hh;
        if (asag_head_box(&hx, &hy, &hz, &hw, &hh))
            consider(9, 0, hx, hy, hz, hw, hh, GUN_RANGE, CLR_LINE);
    }
    for (i = 0; i < ASAG_BOIL_COUNT; i++) {
        int32_t bx, by, bz, bw, bh;
        if (!asag_boil_target(i, &bx, &by, &bz, &bw, &bh)) continue;
        /* >>> CLEARED SHORT OF ITSELF (CLR_BOIL). <<< The boil faces are at
           z=2734 and the collision proxy's back wall is at z=2700 — 34 units IN
           FRONT of them, because the proxy is a rectangle and the lumps stand
           proud of the wall it approximates. weapon_aim_clear() out to the
           boil's own depth therefore reports every shot blocked, by a wall
           standing inside the target. This is the runbook's mistake 2 in a
           different hat and asag_boil_clear_depth() is the documented fix;
           asag_fight.h has the argument, including why the backoff is a
           fraction of the shot rather than a fixed 80 off the end of it (this
           weapon's range hid that; the lantern's did not). */
        /* THE REACH GOES THROUGH asag_boil_reach() TOO, and it changes nothing
           here: this weapon's 4000 is already longer than the boils' 2400 and
           the call hands it straight back. It is written this way for the
           reason the line below it is — one rule, asked by both weapons, so a
           later change to either number cannot leave the two disagreeing about
           where a boil can be hit from. */
        consider(10, i, bx, by, bz, bw, bh, asag_boil_reach(GUN_RANGE), CLR_BOIL);
    }

    if (vampire_health > 0)
        consider(2, 0, vampire_x, vampire_y + VAMPIRE_Y, vampire_z,
                 VAMPIRE_HALF_W, VAMPIRE_HALF_H, GUN_RANGE, CLR_LINE);

    /* VINE CURTAINS, and they are the only PROP that competes for the
       crosshair. They are deliberately tested LAST and with no clear test
       (CLR_NONE): vines_point_solid puts them in props_block_point, so
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
        consider(8, i, vn->x, cyc, vn->z, hw, hh, GUN_RANGE, CLR_NONE);
    }

    /* A Scattershot has already landed every pellet it is going to. Otherwise:
       nothing hittable in the field (or a wall/prop was the nearest thing under
       it) — no damage, no impact sprite; ghit only marks enemy hits. */
    if (shot_spread || best_kind < 0)
        return;

    gun_hit(best_kind, best_idx);

    /* Impact sprite on the struck enemy, pulled a touch toward the camera so it
       sits in front of the body rather than inside it. */
    {
        int32_t d = best_depth - GUN_HIT_BACKOFF;
        if (d < 1) d = 1;
        int32_t hx, hy, hz;
        weapon_aim_ray_point(shot_fx, shot_fz, d, &hx, &hy, &hz);
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
   happened, and no "Loaded ..." line is posted.

   It also shuts the ammo wheel and disarms R2: a weapon switch taken with the
   wheel up must not leave the player rooted, or load a type into a gun that is
   no longer in their hand when R2 comes up. */
void graveolver_cancel_reload(void) {
    wheel_open = 0;
    r2_frames  = -1;
    if (reload_timer > 0) {
        reload_timer = 0;
        swap_pending = 0;
        reload_to    = graveolver_ammo;
        sound_stop(SFX_GR_RELOAD);
    }
}

int graveolver_wheel_open(void) {
    return wheel_open;
}

/* Can the wheel's cursor rest on this box? See the wheel_slots[] note. */
static int wheel_pickable(int slot) {
    int t = wheel_slots[slot].ammo;
    if (t < 0) return 0;
    return t == (int)graveolver_ammo || player_ammo[t] > 0;
}

/* Start the reload animation toward `target`. swap is 1 only for a real type
   change, which is what posts the "Loaded ..." line on completion. */
static void start_reload(AmmoType target, int swap) {
    reload_to    = target;
    swap_pending = swap;
    reload_timer = GRAV_RELOAD_FRAMES;
    sound_play(SFX_GR_RELOAD);
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
    static int dpad_prev   = 0;
    static int cooldown    = 0;
    if (cooldown > 0)    cooldown--;
    if (muzzle_flash > 0) muzzle_flash--;
    if (recoil_timer > 0) recoil_timer--;

    int square_held = 0, r2_held = 0, dpad = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        square_held = (~pad->btn & PAD_SQUARE) ? 1 : 0;
        r2_held     = (~pad->btn & PAD_R2)     ? 1 : 0;
        dpad        = ~pad->btn & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
    }
    int square_just = square_held && !square_prev;
    int r2_just     = r2_held     && !r2_prev;
    int dpad_just   = dpad & ~dpad_prev;
    square_prev = square_held;
    r2_prev     = r2_held;
    dpad_prev   = dpad;

    /* A reload is running: count it down and settle the cylinder when it
       finishes. No firing and no further swapping until it completes — and an
       R2 pressed during it is NOT armed, so holding it through the end of a
       reload does not roll straight into a tap or the wheel. */
    if (reload_timer > 0) {
        reload_timer--;
        if (reload_timer == 0) reload_complete();
        return;
    }

    if (game_state == STATE_MENU) {
        wheel_open = 0;
        r2_frames  = -1;
        return;
    }

    /* R2: tap to top up, hold for the wheel (see the note at reload_to). While
       it is down the gun does nothing else — no shot can go off mid-choice. */
    if (r2_just) r2_frames = 0;
    if (r2_frames >= 0) {
        if (r2_held) {
            if (!wheel_open && ++r2_frames >= WHEEL_HOLD_FRAMES) {
                int s;
                wheel_open = 1;
                wheel_pick = WHEEL_UP;
                for (s = 0; s < WHEEL_SLOTS; s++)
                    if (wheel_slots[s].ammo == (int)graveolver_ammo) wheel_pick = s;
            }
            if (wheel_open) {
                int dir = wheel_dpad_dir(dpad_just);
                if (dir >= 0 && dir != wheel_pick && wheel_pickable(dir)) {
                    wheel_pick = dir;
                    sound_play(SFX_CURSOR);
                }
                wheel_show = 1;
            }
            return;
        }

        /* Released. */
        r2_frames = -1;
        if (wheel_open) {
            wheel_open = 0;
            /* Leaving the cursor on the chambered type is a change of mind,
               not a reload. */
            AmmoType target = (AmmoType)wheel_slots[wheel_pick].ammo;
            if (target != graveolver_ammo) start_reload(target, 1);
        } else if (graveolver_loaded < GRAVEOLVER_CAPACITY &&
                   player_ammo[graveolver_ammo] > 0) {
            /* A tap: top up from the chambered type's own reserve. A full
               cylinder, or nothing left to load, and the tap does nothing. */
            start_reload(graveolver_ammo, 0);
        }
        return;
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
        start_reload(graveolver_ammo, 0);
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

/* The ammo wheel: four boxes in the inventory menu's colours, each showing its
   type's icon and reserve count, and the box under the cursor wearing the
   menu's highlight (blue/white double outline with white corner ticks). */
static void draw_ammo_wheel(RenderContext *ctx) {
    int s;
    for (s = 0; s < WHEEL_SLOTS; s++) {
        int ix, iy;
        wheel_draw_box(ctx, s, s == wheel_pick, &ix, &iy);

        int t = wheel_slots[s].ammo;
        if (t >= 0 && (t == (int)graveolver_ammo || player_ammo[t] > 0)) {
            menu_draw_item_icon_any(ctx, wheel_slots[s].menu_slot, ix, iy,
                                    WHEEL_ICON, OT_WHEEL_ICON);
            int count = menu_item_count(wheel_slots[s].menu_slot);
            if (count)
                menu_draw_count(ctx, ix, iy + WHEEL_ICON, count, 2, OT_WHEEL_COUNT);
        }
    }
    wheel_draw_done(ctx);
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

    /* Aiming reticule: a cross with a centre gap at the crosshair, in the
       chambered type's colour (ammo_info[].reticle_*). Hidden while the ammo
       wheel or the weapon wheel is up — the d-pad is choosing, not aiming, and
       the cross would sit among the wheel's boxes.

       A type with a WIDE field (the Scattershot) also gets four corner brackets
       at the field's own edges, so what the player sees is exactly the box a
       target has to touch (weapon_aim_in_box). The 14 px types are the old
       cross alone - their arms already reach the field's edge. */
    if (wheel_show) {
        draw_ammo_wheel(ctx);
        wheel_show = 0;
    } else if (!weapon_wheel_open()) {
        const AmmoInfo *ai = &ammo_info[graveolver_ammo];
        uint8_t r = ai->reticle_r, g = ai->reticle_g, b = ai->reticle_b;
        int cx = aim_x, cy = aim_y;
        screen_tile(ctx, cx - 14, cy - 1, 8, 2, r, g, b, OT_GUN_RETICULE);
        screen_tile(ctx, cx +  6, cy - 1, 8, 2, r, g, b, OT_GUN_RETICULE);
        screen_tile(ctx, cx - 1, cy - 14, 2, 8, r, g, b, OT_GUN_RETICULE);
        screen_tile(ctx, cx - 1, cy +  6, 2, 8, r, g, b, OT_GUN_RETICULE);
        if (ai->aim_hw > 14 || ai->aim_hh > 14) {
            int x0 = cx - ai->aim_hw, x1 = cx + ai->aim_hw;   /* outer edges */
            int y0 = cy - ai->aim_hh, y1 = cy + ai->aim_hh;
            const int arm = 12;
            screen_tile(ctx, x0,       y0,       arm, 2,   r, g, b, OT_GUN_RETICULE);
            screen_tile(ctx, x0,       y0,       2,   arm, r, g, b, OT_GUN_RETICULE);
            screen_tile(ctx, x1 - arm, y0,       arm, 2,   r, g, b, OT_GUN_RETICULE);
            screen_tile(ctx, x1 - 2,   y0,       2,   arm, r, g, b, OT_GUN_RETICULE);
            screen_tile(ctx, x0,       y1 - 2,   arm, 2,   r, g, b, OT_GUN_RETICULE);
            screen_tile(ctx, x0,       y1 - arm, 2,   arm, r, g, b, OT_GUN_RETICULE);
            screen_tile(ctx, x1 - arm, y1 - 2,   arm, 2,   r, g, b, OT_GUN_RETICULE);
            screen_tile(ctx, x1 - 2,   y1 - arm, 2,   arm, r, g, b, OT_GUN_RETICULE);
        }
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
   the chambered type's aim_hw x aim_hh pixels drawn in 2D at the crosshair. This is the actual hit
   field — an enemy is struck only when part of its body projects inside this
   ring — so you can see directly how much aim slop there is. */
void graveolver_debug_draw(RenderContext *ctx) {
    if (debug_mode != 3) return;  /* aim-circle viz only in full-debug (level 3) */

    const int SEGS = 20;
    int cx = aim_x, cy = aim_y;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int rx = ammo_info[graveolver_ammo].aim_hw;
    int ry = ammo_info[graveolver_ammo].aim_hh;
    int prev_x = cx + rx, prev_y = cy;   /* isin/icos: angle 0 -> +X */
    int s;
    for (s = 1; s <= SEGS; s++) {
        int32_t ang = (s * 4096) / SEGS;             /* 0..4096 = full turn */
        int nx = cx + ((icos(ang) * rx) >> 12);
        int ny = cy + ((isin(ang) * ry) >> 12);
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
