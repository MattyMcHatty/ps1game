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
#include "mushroom.h"
#include "fatdoor.h"
#include "sound.h"

Mushroom mushrooms[MAX_MUSHROOMS];
int      mushroom_count = 0;

/* ---- Sprites ----------------------------------------------------------------
   Four textures, each owning its own VRAM slot: no texmgr, no re-upload on a
   room transition, just one CD read and one LoadImage apiece at startup. That
   is only affordable because they are 96x96 rather than 128x128 — see the note
   in mushroom.h and tools/VRAM_MAP.txt.

   The UV rect is derived from the TIM's own prect rather than hard-coded,
   exactly as rafflesia.c's load_owned_sprite does it, so moving a slot in
   png_to_tim needs no matching edit here. */
typedef struct { uint16_t tpage, clut; uint8_t u0, v0, u1, v1; } Sprite;

enum { MSH_TEX_RUN, MSH_TEX_BEHIND, MSH_TEX_CLOSED, MSH_TEX_OPEN, MSH_TEX_COUNT };

static Sprite spr[MSH_TEX_COUNT];
static int    tex_loaded = 0;

static const char *msh_tex_file[MSH_TEX_COUNT] = {
    "\\TEX\\MSHYRUN.TIM;1",    /* walking toward the camera  */
    "\\TEX\\MSHYBHND.TIM;1",   /* walking away from it       */
    "\\TEX\\MSHYCLSD.TIM;1",   /* cap shut                   */
    "\\TEX\\MSHYOPEN.TIM;1",   /* cap split, screaming       */
};

/* The floor shadow, shared with the spider and the demon dog. It is a plain
   resident texture at VRAM (640,160) that nobody streams over, so loading it
   again here costs one CD read at startup and no VRAM at all — the second
   LoadImage writes the same bytes to the same place. Cheaper than exporting a
   handle out of spider.c and coupling the two enemies together. */
static uint16_t shadow_tpage = 0, shadow_clut = 0;

/* Texture window the current area expects, restored after each sprite. All four
   bodies sit at Voff 128, so a room's 128-tall window would wrap their V and
   sample whatever is above them in VRAM. Same bracket the spider uses. */
static RECT msh_tw_restore;
static int  msh_tw_active = 0;

void mushrooms_set_texwindow(const RECT *tw) {
    if (tw) { msh_tw_restore = *tw; msh_tw_active = 1; }
    else    { msh_tw_active = 0; }
}

/* Startup loader for a texture that owns its VRAM slot: CD read, one LoadImage,
   keep the tpage/clut and work the UV rect out of the TIM header. Copied from
   rafflesia.c's load_owned_sprite. STARTUP ONLY. */
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
    /* One-texel inset on every edge. These four sit shoulder to shoulder in
       VRAM (x464/512/560), so a magnified quad sampling its own edge pixels
       would drag a stripe of the neighbouring sprite in — the bug the zombie
       hit as a red strip bled up from the row below. */
    s->u0 = (uint8_t)(u_off + 1);
    s->v0 = (uint8_t)((tim.prect->y % 256) + 1);
    s->u1 = (uint8_t)(u_off + tex_w - 2);
    s->v1 = (uint8_t)((tim.prect->y % 256) + tim.prect->h - 2);
    free(buf);
}

static void load_shadow(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\SHADOW.TIM;1")) return;
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
    if (tim.mode & 0x8) { LoadImage(tim.crect, tim.caddr); DrawSync(0); }
    shadow_tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    shadow_clut  = getClut(tim.crect->x, tim.crect->y);
    free(buf);
}

void mushrooms_load_textures(void) {
    int i;
    for (i = 0; i < MSH_TEX_COUNT; i++)
        load_owned_sprite(msh_tex_file[i], &spr[i]);
    load_shadow();
    tex_loaded = (spr[MSH_TEX_RUN].tpage != 0);
}

/* Integer square root (Newton, converging downward). Copied from rafflesia.c,
   including the `t > 0` seed condition — `t > 1` stops one shift early and
   seeds BELOW the root for every input that is not a perfect power of four, at
   which point the loop returns its own seed (isqrt(120) came back 8).

   Everything in this file that is stated as a RADIUS uses this rather than the
   Manhattan sum the steering uses, so a stated radius means the same in every
   direction. The Outside Catacombs is ~8800 x 6600, so the worst-case dx*dx +
   dz*dz is about 1.2e8 — comfortably inside int32. */
static int32_t msh_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* Point the body along (dx,dz). Only the SIGN of the stored vector matters —
   draw_mushrooms uses it to pick the front or the back sprite — so it is
   normalised to a small magnitude that cannot overflow the packed int16s. */
static void msh_face(Mushroom *m, int32_t dx, int32_t dz) {
    int32_t d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (d <= 0) return;
    int32_t fx = (dx * 64) / d;
    int32_t fz = (dz * 64) / d;
    m->facing = ((int32_t)(int16_t)fx << 16) | (uint16_t)(int16_t)fz;
}

int mushroom_add(int32_t ax, int32_t az, int32_t bx, int32_t bz,
                 int32_t y, GameState area) {
    if (mushroom_count >= MAX_MUSHROOMS) return -1;
    int i = mushroom_count++;
    Mushroom *m = &mushrooms[i];
    *m = (Mushroom){0};
    m->x = ax; m->y = y; m->z = az;
    m->pa_x = ax; m->pa_z = az;
    m->pb_x = bx; m->pb_z = bz;
    m->spawn_y = y;
    m->to_b   = 1;                 /* starts at A, walking toward B */
    m->health = MSH_MAX_HEALTH;
    m->state  = MSH_PACE;
    m->active = 1;
    m->area   = area;
    msh_face(m, bx - ax, bz - az);
    return i;
}

void mushrooms_init(void) {
    /* Placements are seeded on a room's first entry by the world system (see
       world_seed_room in world.c). The array starts empty. */
    mushroom_count = 0;
}

void mushrooms_reset(void) {
    mushroom_count = 0;
}

void mushrooms_rest(void) {
    int i;
    for (i = 0; i < mushroom_count; i++) {
        Mushroom *m = &mushrooms[i];
        if (!m->active || m->state == MSH_DEAD) continue;
        /* Rebuild exactly as mushroom_add left it: back at patrol point A,
           unalerted, at full health. The patrol points and the area tag are the
           only things that survive the wipe. */
        int32_t   ax = m->pa_x, az = m->pa_z;
        int32_t   bx = m->pb_x, bz = m->pb_z;
        int32_t   y  = m->spawn_y;
        GameState a  = m->area;
        *m = (Mushroom){0};
        m->x = ax; m->y = y; m->z = az;
        m->pa_x = ax; m->pa_z = az;
        m->pb_x = bx; m->pb_z = bz;
        m->spawn_y = y;
        m->to_b   = 1;
        m->health = MSH_MAX_HEALTH;
        m->state  = MSH_PACE;
        m->active = 1;
        m->area   = a;
        msh_face(m, bx - ax, bz - az);
    }
}

/* No weaknesses, by design: the Mushroom Head takes 1 from a crucifaxe swing
   and 1 from a round of any kind, so it is always exactly MSH_MAX_HEALTH hits.
   The placeholder line keeps the table shape for whoever gives it one later. */
static const Weakness mushroom_weakness[] = {
    { DMG_KINETIC, 100 },   /* 100% = no change */
};

int32_t mushroom_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, mushroom_weakness,
                        WEAKNESS_COUNT(mushroom_weakness));
}

int mushroom_airborne(const Mushroom *m) {
    return m->state == MSH_LEAP;
}

void mushroom_body(const Mushroom *m, int32_t *cyc, int32_t *hh, int32_t *hw) {
    *cyc = m->y + MSH_Y_OFFSET;
    *hh  = MSH_HALF_H;
    *hw  = MSH_HALF_W;
}

/* Enter the scream. Rooted for MSH_SCREAM_TOTAL frames, facing the player; the
   cap opens at MSH_SCREAM_CLOSED and the leap goes out at the end. */
static void mushroom_scream(Mushroom *m) {
    m->state       = MSH_SCREAM;
    m->scream_tick = 0;
    m->kb_vx = m->kb_vz = 0;
    msh_face(m, player_x() - m->x, player_z() - m->z);
}

void mushroom_damage(Mushroom *m, int dmg) {
    if (!m->active || m->state == MSH_DEAD) return;
    /* Hit while still on patrol: that is an alert, and it screams like any
       other. A hit taken mid-ritual does NOT restart it — being shot in the
       face during a leap should not buy the player a second wind-up. */
    if (m->state == MSH_PACE) mushroom_scream(m);
    m->health   -= dmg;
    m->hit_timer = MSH_BAR_TIMER_MAX;
    if (m->health <= 0) {
        m->health = 0;
        m->state  = MSH_DEAD;
        spawn_blood_burst(m->x, m->y, m->z);
        /* It dies on its own scream. Retriggering SFX_HISS rather than stopping
           it first is deliberate: the sound sits on a voice of its own (voice
           15, sfx_channel), so a fresh key-on cuts any wind-up already sounding
           and starts the death cry from the top in the same frame. */
        sound_play(SFX_HISS);
    } else {
        sound_play(SFX_AXEHIT);
    }
}

/* ---- Steering ---------------------------------------------------------------
   The spider's feeler steering, lifted whole and generalised over a GOAL rather
   than hard-wired to the player: a pacing mushroom's goal is its next patrol
   point and a chasing one's is the player, and everything downstream — the
   feeler, the wall-follow perpendiculars, the "which side gets me closer"
   tie-break — reads the goal, not cam_x/cam_z.

   `sight_clear` switches the wall-follow off when there is nothing between the
   mushroom and where it is going. That shortcut is only sound when the goal is
   ahead of it, which is why the caller passes it rather than this function
   deciding: the feeler's point-and-radius test also trips merely walking NEAR a
   wall, and wall-following on that deadlocks in concave corners, while the real
   move's own collision push already slides the body along anything it grazes.

   Returns 1 if the body actually travelled this frame. */
static int msh_steer(Mushroom *m, int32_t goal_dx, int32_t goal_dz,
                     int32_t speed, int sight_clear) {
    int i;

    /* Separation: a soft shove away from any other mushroom standing too close,
       so a pair never stacks into one silhouette. */
    int32_t sep_x = 0, sep_z = 0;
    for (i = 0; i < mushroom_count; i++) {
        Mushroom *o = &mushrooms[i];
        if (o == m) continue;
        if (!o->active || o->state == MSH_DEAD || o->area != current_area) continue;
        int32_t odx   = m->x - o->x;
        int32_t odz   = m->z - o->z;
        int32_t odist = (odx < 0 ? -odx : odx) + (odz < 0 ? -odz : odz);
        if (odist < MSH_SEP_RADIUS && odist > 0) {
            int32_t push = MSH_SEP_RADIUS - odist;
            sep_x += (odx * push) / odist;
            sep_z += (odz * push) / odist;
        }
    }

    int32_t desired_x = goal_dx + sep_x * MSH_SEP_WEIGHT;
    int32_t desired_z = goal_dz + sep_z * MSH_SEP_WEIGHT;
    int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                           (desired_z < 0 ? -desired_z : desired_z);
    if (desired_dist == 0) desired_dist = 1;

    /* Probe ahead in the desired direction. */
    int32_t feeler_x = m->x + (desired_x * MSH_FEELER_LEN) / desired_dist;
    int32_t feeler_z = m->z + (desired_z * MSH_FEELER_LEN) / desired_dist;
    int32_t fx = feeler_x, fz = feeler_z;
    crates_collide(&fx, m->y, &fz, 80);
    dining_tables_collide(&fx, m->y, &fz, 75);
    apply_flat_entity_collision(&fx, &fz, MSH_BODY_RADIUS);
    int blocked = (fx != feeler_x || fz != feeler_z);
    if (sight_clear) { blocked = 0; m->steer_timer = 0; }

    int32_t pl_x = -goal_dz, pl_z =  goal_dx;   /* slide left  */
    int32_t pr_x =  goal_dz, pr_z = -goal_dx;   /* slide right */
    int32_t goal_px = m->x + goal_dx;
    int32_t goal_pz = m->z + goal_dz;

    if (blocked && m->steer_timer <= 0) {
        /* Newly blocked: probe both sides and commit to one for a while, so the
           body follows the wall instead of flip-flopping against it. */
        int32_t pl_dist = (pl_x < 0 ? -pl_x : pl_x) + (pl_z < 0 ? -pl_z : pl_z);
        int32_t pr_dist = (pr_x < 0 ? -pr_x : pr_x) + (pr_z < 0 ? -pr_z : pr_z);
        if (pl_dist == 0) pl_dist = 1;
        if (pr_dist == 0) pr_dist = 1;

        int32_t lx = m->x + (pl_x * MSH_FEELER_LEN) / pl_dist;
        int32_t lz = m->z + (pl_z * MSH_FEELER_LEN) / pl_dist;
        int32_t rx = m->x + (pr_x * MSH_FEELER_LEN) / pr_dist;
        int32_t rz = m->z + (pr_z * MSH_FEELER_LEN) / pr_dist;

        int32_t tlx = lx, tlz = lz;
        crates_collide(&tlx, m->y, &tlz, 80);
        dining_tables_collide(&tlx, m->y, &tlz, 75);
        apply_flat_entity_collision(&tlx, &tlz, MSH_BODY_RADIUS);
        int left_blocked = (tlx != lx || tlz != lz);

        int32_t trx = rx, trz = rz;
        crates_collide(&trx, m->y, &trz, 80);
        dining_tables_collide(&trx, m->y, &trz, 75);
        apply_flat_entity_collision(&trx, &trz, MSH_BODY_RADIUS);
        int right_blocked = (trx != rx || trz != rz);

        if (left_blocked && !right_blocked) {
            m->steer_dir = +1;
        } else if (right_blocked && !left_blocked) {
            m->steer_dir = -1;
        } else {
            int32_t ld = (goal_px - lx < 0 ? lx - goal_px : goal_px - lx) +
                         (goal_pz - lz < 0 ? lz - goal_pz : goal_pz - lz);
            int32_t rd = (goal_px - rx < 0 ? rx - goal_px : goal_px - rx) +
                         (goal_pz - rz < 0 ? rz - goal_pz : goal_pz - rz);
            m->steer_dir = (ld <= rd) ? -1 : +1;
        }
        m->steer_timer = MSH_STEER_COMMIT;
    }

    if (m->steer_timer > 0) {
        if (m->steer_dir < 0) { desired_x = pl_x; desired_z = pl_z; }
        else                  { desired_x = pr_x; desired_z = pr_z; }
        desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                       (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;
        m->steer_timer--;
    }

    /* Blend the new heading into the old one so turns are not instant. */
    int32_t move_x  = (desired_x * speed) / desired_dist;
    int32_t move_z  = (desired_z * speed) / desired_dist;
    int32_t prev_mx = (int16_t)(m->facing >> 16);
    int32_t prev_mz = (int16_t)(m->facing & 0xFFFF);
    int32_t blend_x = (prev_mx * (8 - MSH_TURN_RATE) + move_x * MSH_TURN_RATE) >> 3;
    int32_t blend_z = (prev_mz * (8 - MSH_TURN_RATE) + move_z * MSH_TURN_RATE) >> 3;
    m->facing = ((int32_t)(int16_t)blend_x << 16) | (uint16_t)(int16_t)blend_z;

    m->x += blend_x;
    m->z += blend_z;
    apply_flat_entity_collision(&m->x, &m->z, MSH_BODY_RADIUS);
    crates_collide(&m->x, m->y, &m->z, 80);
    dining_tables_collide(&m->x, m->y, &m->z, 75);
    fatdoors_collide(&m->x, m->y, &m->z, MSH_DOOR_CLEARANCE);

    return (blend_x != 0 || blend_z != 0);
}

/* Commit to the arc. Aims at a point MSH_LEAP_STANDOFF short of the player,
   capped at MSH_LEAP_MAX and — this is the part that used to be missing —
   FLOORED at MSH_LEAP_MIN_TRAVEL.

   >>> THIS FUNCTION ALWAYS LEAPS. <<< It used to return straight into the chase
   whenever `travel` came out non-positive, i.e. whenever the player was already
   inside the standoff when the scream ended, and that is the whole of the
   "sometimes it doesn't leap" bug: the enemy would finish a two-second wind-up
   and then simply walk. The only remaining escape is a genuinely degenerate
   d == 0, which means the player is standing exactly on the body and there is
   no direction to jump in.

   >>> NOTHING HERE OR IN THE ARC CONSULTS THE LEVEL. <<< That is the point: the
   leap out of the WAIT state is specified to go through whatever the player is
   hiding behind, and having one leap for both cases is simpler than two. The
   frame it lands, apply_ddog_height and the wall pass in msh_steer take over
   and push it out of anything it came down inside. */
static void mushroom_begin_leap(Mushroom *m) {
    int32_t dx = player_x() - m->x;
    int32_t dz = player_z() - m->z;
    int32_t d  = msh_isqrt(dx * dx + dz * dz);

    msh_face(m, dx, dz);

    if (d <= 0) { m->state = MSH_CHASE; m->regroup_tick = MSH_REGROUP; return; }

    int32_t travel = d - MSH_LEAP_STANDOFF;
    if (travel < MSH_LEAP_MIN_TRAVEL) travel = MSH_LEAP_MIN_TRAVEL;
    if (travel > MSH_LEAP_MAX)        travel = MSH_LEAP_MAX;

    m->leap_x0 = m->x;         m->leap_z0 = m->z;
    m->leap_x1 = m->x + (dx * travel) / d;
    m->leap_z1 = m->z + (dz * travel) / d;
    m->leap_ground_y = m->y;

    int len = travel / MSH_LEAP_SPEED;
    if (len < MSH_LEAP_MIN_FRAMES) len = MSH_LEAP_MIN_FRAMES;
    if (len > MSH_LEAP_MAX_FRAMES) len = MSH_LEAP_MAX_FRAMES;
    m->leap_len  = len;
    m->leap_tick = 0;
    m->vy        = 0;
    /* Clear anything that would tug at the body. Knockback is skipped WHILE
       airborne (the LEAP branch returns before the slide), but a residual
       velocity left over from an axe hit during the wind-up would spring back
       to life on the landing frame and drag the touchdown sideways. */
    m->kb_vx = m->kb_vz = 0;
    m->steer_timer = 0;
    m->state     = MSH_LEAP;
}

void update_mushrooms(void) {
    static int hurt_sfx_cooldown = 0;
    int i;
    if (hurt_sfx_cooldown > 0) hurt_sfx_cooldown--;

    for (i = 0; i < mushroom_count; i++) {
        Mushroom *m = &mushrooms[i];
        /* current_area, NEVER game_state: gating on game_state would freeze the
           whole enemy for as long as the inventory menu is up, letting the
           player pause a chase with Start (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!m->active || m->state == MSH_DEAD || m->area != current_area) continue;

        if (m->hit_timer    > 0) m->hit_timer--;
        if (m->damage_timer > 0) m->damage_timer--;
        if (m->regroup_tick > 0) m->regroup_tick--;
        m->anim_tick++;

        /* player_x/y/z, not cam_*: a camera-locked puzzle flies the camera off
           to a fixed shot while the PLAYER stays standing where they were, and
           an enemy that chased the camera would lose them and swing at an empty
           spot (camera.h). Enemies keep running through those puzzles. */
        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - m->x;
        int32_t dy = py - m->y;
        int32_t dz = pz - m->z;

        /* --- Airborne: the arc owns x, y and z outright. No gravity, no floor
           probe, no wall passes, no knockback. --- */
        if (m->state == MSH_LEAP) {
            m->leap_tick++;
            int32_t t = ((int32_t)m->leap_tick << 12) / m->leap_len;
            if (t > 4096) t = 4096;
            m->x = m->leap_x0 + ((m->leap_x1 - m->leap_x0) * t >> 12);
            m->z = m->leap_z0 + ((m->leap_z1 - m->leap_z0) * t >> 12);
            /* Parabola peaking at MSH_LEAP_HEIGHT halfway along. Factored as
               two shifts so the intermediate product cannot overflow int32:
               ((H*t)>>12) * (4096-t) >> 10 is H at t = 2048. */
            int32_t arc = (((MSH_LEAP_HEIGHT * t) >> 12) * (4096 - t)) >> 10;
            m->y = m->leap_ground_y - arc;
            if (m->leap_tick >= m->leap_len) {
                m->y            = m->leap_ground_y;
                m->state        = MSH_CHASE;
                m->regroup_tick = MSH_REGROUP;
            }
            continue;
        }

        /* --- Grounded: gravity and the floor first, as every other enemy --- */
        apply_ddog_height(&m->x, &m->y, &m->z, &m->vy,
                          &m->on_upper_floor, &m->on_ramp);

        /* --- Knocked back by the axe: slide and decay. --- */
        int knocked = (m->kb_vx != 0 || m->kb_vz != 0);
        if (knocked) {
            m->x += m->kb_vx;
            m->z += m->kb_vz;
            apply_flat_entity_collision(&m->x, &m->z, MSH_BODY_RADIUS);
            crates_collide(&m->x, m->y, &m->z, 80);
            dining_tables_collide(&m->x, m->y, &m->z, 75);
            fatdoors_collide(&m->x, m->y, &m->z, MSH_DOOR_CLEARANCE);
            if (m->kb_vx > 0) m->kb_vx =  (  m->kb_vx * 7) >> 3;
            else              m->kb_vx = -((-m->kb_vx * 7) >> 3);
            if (m->kb_vz > 0) m->kb_vz =  (  m->kb_vz * 7) >> 3;
            else              m->kb_vz = -((-m->kb_vz * 7) >> 3);
            /* Being shoved suspends walking and biting — but NOT the scream.
               The scream branch below drives itself off a frame counter and no
               longer moves the body at all, so it can tick right through a
               knockback. It used to `continue` past it, which let a player with
               good axe timing hold a mushroom in its wind-up indefinitely: the
               other half of the "it doesn't always leap" bug. */
            if (m->state != MSH_SCREAM) continue;
        }

        if (m->state == MSH_PACE) {
            /* --- Noticing the player. All three tests, cheapest first; see the
               note beside MSH_ALERT_RADIUS for why each is there. --- */

            /* 1. In range. TRUE radial, so 1400 means 1400 whichever way the
                  player walks in from. */
            if (dx * dx + dz * dz <= (int32_t)MSH_ALERT_RADIUS * MSH_ALERT_RADIUS) {
                /* 2. Walking toward them — the player is in the 180 degrees
                      ahead of its travel direction. `facing` holds the blended
                      per-frame step, so its components are at most the walk
                      speed and the product stays far inside an int32 at this
                      room's scale. Approach it from behind on the return leg of
                      its patrol and it never turns round. */
                int32_t fvx = (int16_t)(m->facing >> 16);
                int32_t fvz = (int16_t)(m->facing & 0xFFFF);
                if (fvx * dx + fvz * dz > 0) {
                    /* 3. And it can actually SEE them. The one place level
                          geometry blocks this enemy's sight: everything after
                          the alert ignores walls by design. */
                    if (!collision_segment_blocked(m->x, m->y + MSH_Y_OFFSET, m->z,
                                                   px, py, pz)) {
                        mushroom_scream(m);
                        continue;
                    }
                }
            }

            /* Walk the patrol. The leg flips when the current point is reached;
               the reach test is Manhattan and generous, because the steering
               blend means it rarely lands exactly on a point. */
            int32_t wx = m->to_b ? m->pb_x : m->pa_x;
            int32_t wz = m->to_b ? m->pb_z : m->pa_z;
            int32_t gx = wx - m->x, gz = wz - m->z;
            if ((gx < 0 ? -gx : gx) + (gz < 0 ? -gz : gz) < MSH_WAYPOINT_REACH) {
                m->to_b = !m->to_b;
                wx = m->to_b ? m->pb_x : m->pa_x;
                wz = m->to_b ? m->pb_z : m->pa_z;
                gx = wx - m->x; gz = wz - m->z;
            }
            /* The sightline shortcut is to the WAYPOINT here, not to the
               player: it is the goal that has to be unobstructed for the
               wall-follow to be safely switched off. */
            int clear = !collision_segment_blocked(m->x, m->y, m->z,
                                                   wx, m->y, wz);
            msh_steer(m, gx, gz, MSH_WALK_SPEED, clear);
            continue;
        }

        if (m->state == MSH_SCREAM) {
            m->scream_tick++;
            msh_face(m, dx, dz);
            /* The cap splits half a second in. The event and the sound are the
               same frame — a wind-up cue belongs at the start of the wind-up,
               because the player is timing their retreat off it. */
            if (m->scream_tick == MSH_SCREAM_CLOSED) sound_play(SFX_HISS);
            /* Rooted for the whole wind-up, then straight into the air. There is
               deliberately no step-in phase: it carried the body toward the
               player during the very frames that decided whether the leap would
               happen at all, and walking on the spot for half a second reads as
               a shuffle rather than as a creature winding up. */
            if (m->scream_tick >= MSH_SCREAM_TOTAL) mushroom_begin_leap(m);
            continue;
        }

        if (m->state == MSH_WAIT) {
            /* Stopped dead. Two ways out: the player steps back into view, or
               the count runs down and it screams the leap up again. */
            msh_face(m, dx, dz);
            if (!collision_segment_blocked(m->x, m->y + MSH_Y_OFFSET, m->z,
                                           px, py, pz)) {
                m->state        = MSH_CHASE;
                m->regroup_tick = MSH_REGROUP;
                continue;
            }
            if (--m->wait_tick <= 0) mushroom_scream(m);
            continue;
        }

        /* --- MSH_CHASE --- */

        /* Lost sight of the player: stop and count down. Suppressed for
           MSH_REGROUP frames after every landing, or a mushroom that came down
           with the player still behind cover would drop straight back into WAIT
           and loop scream-leap-scream without ever reaching them. */
        if (m->regroup_tick == 0 &&
            collision_segment_blocked(m->x, m->y + MSH_Y_OFFSET, m->z,
                                      px, py, pz)) {
            m->state     = MSH_WAIT;
            m->wait_tick = MSH_WAIT_FRAMES;
            continue;
        }

        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

        /* Contact damage. Horizontal and vertical reach are tested SEPARATELY,
           for the reason the zombie and spider do the same: the player's eye
           sits above the body, and a combined budget eats the whole allowance
           vertically so a stopped, adjacent enemy never lands a hit. */
        if (!game_over && dist2d < MSH_CATCH_DIST &&
            (dy < 0 ? -dy : dy) < MSH_CATCH_DIST && m->damage_timer == 0) {
            m->damage_timer = MSH_DAMAGE_TICK;
            player_hurt(MSH_DAMAGE_AMOUNT);
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

        if (dist2d < MSH_CATCH_DIST) { msh_face(m, dx, dz); continue; }

        {
            int clear = !collision_segment_blocked(m->x, m->y, m->z, px, py, pz);
            msh_steer(m, dx, dz, MSH_CHASE_SPEED, clear);
        }
    }

    /* --- Mushroom vs mushroom hard collision, after every one has moved.
       Airborne bodies are skipped: nothing may nudge a leap off its line. --- */
    int a, b;
    for (a = 0; a < mushroom_count; a++) {
        Mushroom *ma = &mushrooms[a];
        if (!ma->active || ma->state == MSH_DEAD || ma->state == MSH_LEAP ||
            ma->area != current_area) continue;
        for (b = a + 1; b < mushroom_count; b++) {
            Mushroom *mb = &mushrooms[b];
            if (!mb->active || mb->state == MSH_DEAD || mb->state == MSH_LEAP ||
                mb->area != current_area) continue;
            int32_t cdx  = ma->x - mb->x;
            int32_t cdz  = ma->z - mb->z;
            int32_t dist = (cdx < 0 ? -cdx : cdx) + (cdz < 0 ? -cdz : cdz);
            int32_t min_dist = MSH_BODY_RADIUS * 2;
            if (dist < min_dist && dist > 0) {
                int32_t push    = (min_dist - dist) / 2;
                int32_t push_ax = (cdx * push) / dist;
                int32_t push_az = (cdz * push) / dist;
                ma->x += push_ax; ma->z += push_az;
                mb->x -= push_ax; mb->z -= push_az;
            }
        }
    }
}

/* Bracket an already-filled POLY_FT4 with a full/unmasked texture window and a
   restore of the area's own, all within one OT bucket. Identical to the
   spider's, and needed for the same reason: all four sprites sit at Voff 128.
   addPrim() prepends, so adding restore, poly, disable yields the draw order
   disable -> poly -> restore. */
static void add_ft4_windowed(RenderContext *ctx, int32_t otz, POLY_FT4 *poly) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;

    if (msh_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &msh_tw_restore);
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

/* The floor shadow. Drawn at the GROUND the body belongs to, not under the
   body — mid-leap that is the height it took off from, which is what makes the
   arc read as a jump rather than as the whole enemy floating upward. */
static void draw_msh_shadow(RenderContext *ctx, Mushroom *m) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (!shadow_tpage) return;
    if (ctx->next_packet + sizeof(DR_TPAGE) + sizeof(POLY_FT4) > buf_end) return;

    int32_t gy = (m->state == MSH_LEAP) ? m->leap_ground_y : m->y;
    int32_t sx = m->x, sz = m->z;

    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((MSH_SHADOW_W * rx) >> 12);
    int16_t dwz = (int16_t)((MSH_SHADOW_W * rz) >> 12);

    int32_t fx  = isin(cam_rot);
    int32_t fz  = icos(cam_rot);
    int16_t ddx = (int16_t)((MSH_SHADOW_D * fx) >> 12);
    int16_t ddz = (int16_t)((MSH_SHADOW_D * fz) >> 12);

    int32_t shadow_y = gy + MSH_Y_OFFSET + MSH_HALF_H - 2;

    SVECTOR sv[4];
    sv[0].vx = (int16_t)(sx - dwx - ddx); sv[0].vy = (int16_t)shadow_y; sv[0].vz = (int16_t)(sz - dwz - ddz); sv[0].pad = 0;
    sv[1].vx = (int16_t)(sx + dwx - ddx); sv[1].vy = (int16_t)shadow_y; sv[1].vz = (int16_t)(sz + dwz - ddz); sv[1].pad = 0;
    sv[2].vx = (int16_t)(sx - dwx + ddx); sv[2].vy = (int16_t)shadow_y; sv[2].vz = (int16_t)(sz - dwz + ddz); sv[2].pad = 0;
    sv[3].vx = (int16_t)(sx + dwx + ddx); sv[3].vy = (int16_t)shadow_y; sv[3].vz = (int16_t)(sz + dwz + ddz); sv[3].pad = 0;

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

    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 1, shadow_tpage);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz + 1], tp);
    ctx->next_packet += sizeof(DR_TPAGE);

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, 128, 128, 128);

    poly->x0 = ssv[0].vx; poly->y0 = ssv[0].vy;
    poly->x1 = ssv[1].vx; poly->y1 = ssv[1].vy;
    poly->x2 = ssv[2].vx; poly->y2 = ssv[2].vy;
    poly->x3 = ssv[3].vx; poly->y3 = ssv[3].vy;

    /* shadow.tim is 64x32 at VRAM (640,160): tpage base y=0, so V is 160..191 */
    poly->u0 =  0; poly->v0 = 160;
    poly->u1 = 63; poly->v1 = 160;
    poly->u2 =  0; poly->v2 = 191;
    poly->u3 = 63; poly->v3 = 191;

    poly->clut  = shadow_clut;
    poly->tpage = shadow_tpage;

    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, otz, poly);
}

/* One camera-facing body quad. `flip` mirrors it in U, which is the entire walk
   cycle: alternating a frame with its own mirror image reads as a gait without
   costing a second texture. There is deliberately no gte_nclip backface cull —
   a camera-facing quad has no back face, and the mirror would reverse its
   winding and throw it away. */
static void draw_msh_sprite(RenderContext *ctx, Mushroom *m,
                            const Sprite *s, int flip) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);

    int32_t cx = m->x, cz = m->z, cy = m->y + MSH_Y_OFFSET;

    int16_t dwx = (int16_t)((MSH_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((MSH_HALF_W * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - MSH_HALF_H);
    int16_t vy_bot = (int16_t)(cy + MSH_HALF_H);

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
       its floor polys keeps the sprite off the ground. */
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

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
    if (m->hit_timer > 0) setRGB0(poly, fog8, fog8 >> 2, fog8 >> 2);
    else                  setRGB0(poly, fog8, fog8, fog8);

    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

    uint8_t u_left  = flip ? s->u1 : s->u0;
    uint8_t u_right = flip ? s->u0 : s->u1;
    poly->u0 = u_left;  poly->v0 = s->v0;
    poly->u1 = u_right; poly->v1 = s->v0;
    poly->u2 = u_left;  poly->v2 = s->v1;
    poly->u3 = u_right; poly->v3 = s->v1;

    poly->tpage = s->tpage;
    poly->clut  = s->clut;

    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, otz, poly);

    if (m->hit_timer <= 0) return;

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

    int16_t fill_w = (int16_t)((m->health * 40) / MSH_MAX_HEALTH);
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

void draw_mushrooms(RenderContext *ctx) {
    if (!tex_loaded) return;
    int i;
    for (i = 0; i < mushroom_count; i++) {
        Mushroom *m = &mushrooms[i];
        if (!m->active || m->state == MSH_DEAD || m->area != current_area) continue;

        int32_t dx = m->x - cam_x;
        int32_t dz = m->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 4000) continue;

        draw_msh_shadow(ctx, m);

        int tex, flip = 0;
        switch (m->state) {
        case MSH_SCREAM:
            /* Cap shut for the first half-second, split for the rest of it. */
            tex = (m->scream_tick < MSH_SCREAM_CLOSED) ? MSH_TEX_CLOSED
                                                       : MSH_TEX_OPEN;
            break;
        case MSH_LEAP:
            tex = MSH_TEX_OPEN;    /* the lunge comes out of the scream */
            break;
        case MSH_WAIT:
            tex = MSH_TEX_CLOSED;  /* stopped, listening */
            break;
        default: {
            /* Walking. Which of the two travel sprites depends on whether the
               body is coming toward the CAMERA or going away from it — that is
               what decides whether its front or its back is showing, and it is
               the same test whether it is pacing a patrol or charging. The
               mirror flip on top of it is the gait. */
            int32_t fx  = (int16_t)(m->facing >> 16);
            int32_t fz  = (int16_t)(m->facing & 0xFFFF);
            int32_t tox = cam_x - m->x;
            int32_t toz = cam_z - m->z;
            /* Shifted down before multiplying: the two vectors are a small
               heading and a room-scale offset, and the raw product of the
               latter with itself would be the only thing at risk here. */
            int32_t dot = fx * (tox >> 4) + fz * (toz >> 4);
            tex  = (dot > 0) ? MSH_TEX_RUN : MSH_TEX_BEHIND;
            /* Two cadences: the amble and the run. See MSH_ANIM_RATE_PACE. */
            int rate = (m->state == MSH_PACE) ? MSH_ANIM_RATE_PACE
                                              : MSH_ANIM_RATE_CHASE;
            flip = ((m->anim_tick / rate) & 1);
            break;
        }
        }

        draw_msh_sprite(ctx, m, &spr[tex], flip);
    }
}
