#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"      /* player_x/y/z, cam_* */
#include "player.h"      /* player_hurt, player_health, game_over */
#include "collision.h"   /* apply_ddog_height, collision_segment_blocked */
#include "crucifaxe.h"   /* swing_timer — the deflect window reads it */
#include "particles.h"
#include "door_anim.h"   /* door_anim_panels_taken — see nessies_upload_textures */
#include "texmgr.h"
#include "sound.h"
#include "nessie.h"

Nessie nessies[MAX_NESSIES];
int    nessie_count = 0;

/* ---- Sprites ----------------------------------------------------------------
   ONE sheet per ROW of the source art, Adapa's footprint: see nessie.h. Both are
   quantised against one master palette (tools/split_nessie_sheet.py), so the
   attack's step from frame 2 to frame 3 — which crosses sheets — keeps its
   colour. */
#define NSS_SHEETS 2
static uint16_t nss_tpage [NSS_SHEETS] = { 0, 0 };
static uint16_t nss_clut  [NSS_SHEETS] = { 0, 0 };
static int      nss_tex_id[NSS_SHEETS] = { -1, -1 };
static int      tex_loaded = 0;

/* Texture window the current area expects, restored after each sprite. Both
   sheets sit at VRAM y=128, i.e. Voff 128, so a room's 128-tall window would
   wrap their V and sample the wrong half of the page. The lumberer's bracket,
   for the lumberer's reason. */
static RECT nss_tw_restore;
static int  nss_tw_active = 0;

void nessies_set_texwindow(const RECT *tw) {
    if (tw) { nss_tw_restore = *tw; nss_tw_active = 1; }
    else    { nss_tw_active = 0; }
}

void nessies_load_textures(void) {
    /* BANK: Chapter 4 only. Derived, not guessed: py tools/check_tex_banks.py
       walks the uploader call graph and fails the build if this is short. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    nss_tex_id[0] = texmgr_register("\\TEXUNDW\\NESSIEA.TIM;1");
    nss_tex_id[1] = texmgr_register("\\TEXUNDW\\NESSIEB.TIM;1");
    {
        int i;
        for (i = 0; i < NSS_SHEETS; i++) {
            nss_tpage[i] = texmgr_tpage(nss_tex_id[i]);
            nss_clut [i] = texmgr_clut (nss_tex_id[i]);
        }
    }
    tex_loaded = (nss_tex_id[0] >= 0 && nss_tex_id[1] >= 0);
}

void nessies_upload_textures(void) {
    int i;
    for (i = 0; i < NSS_SHEETS; i++) texmgr_upload(nss_tex_id[i]);
    /* >>> AND SAY WHAT WAS JUST OVERWRITTEN. <<< The pages are the exit door's
       two leaves (and Adapa's, who brings his own back), so the restore has to
       know to run on the way out of the chapter — Adapa's uploader does the
       same. A no-op flag until this line runs. */
    door_anim_panels_taken();
}

/* The rooms whose entry uploads the sheet. main.c keys the upload on this, and
   tools/gen_tex_rooms.py mirrors it as NESSIE_ROOMS — keep the two in step, or
   the texture late-loads (logged, still correct). */
int nessie_room_has_sheet(GameState area) {
    return area == STATE_SHARURS_TRIAL;
}

/* Integer square root, lumberer.c's — including the `t > 0` seed (mistake 11 in
   tools/ADDING_AN_ENEMY.txt). The worst case here is a fireball's 3D distance
   across Sharur's Trial, about 4.8e7 squared: inside int32. */
static int32_t nss_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* ---- The pool ----------------------------------------------------------------
   Is (x,z) lava belonging to this Nessie's pool? See nessie_pools.h. Off the
   mask's own rectangle is simply "no". */
static int nss_in_pool(const Nessie *s, int32_t x, int32_t z) {
    const NessiePool *p = &nessie_pools[s->pool];
    int32_t u = x - p->x0, v = z - p->z0;
    if (u < 0 || v < 0) return 0;
    u /= NSS_POOL_CELL;
    v /= NSS_POOL_CELL;
    if (u >= p->w || v >= p->h) return 0;
    return (p->bits[v * p->stride + (u >> 3)] >> (u & 7)) & 1;
}

static void nss_random_heading(Nessie *s) {
    int32_t a = rand() & 4095;
    s->hx  = isin(a);
    s->hz  = icos(a);
    s->leg = NSS_LEG_FRAMES;
}

/* Rebuild exactly as nessie_add leaves one. Only the spawn, the pool and the
   area survive the wipe. */
static void nss_spawn(Nessie *s, int32_t x, int32_t y, int32_t z,
                      NessiePoolId pool, GameState area) {
    *s = (Nessie){0};
    s->x = x; s->y = y; s->z = z;
    s->qx = x << 8; s->qz = z << 8;
    s->spawn_x = x; s->spawn_z = z; s->spawn_y = y;
    s->pool   = pool;
    s->health = NSS_MAX_HEALTH;
    s->state  = NSS_IDLE;
    s->active = 1;
    s->area   = area;
    nss_random_heading(s);
}

int nessie_add(int32_t x, int32_t z, NessiePoolId pool, GameState area) {
    if (nessie_count >= MAX_NESSIES) return -1;
    int i = nessie_count++;
    /* The standing anchor of a y=0 floor: a floor SURFACE less GROUND_FLOOR_Y,
       what apply_ddog_height settles every body on. Authored, so a save rebuild
       needs no floor probe; apply_ddog_height corrects it on the first frame in
       a room whose lava is not at 0. */
    nss_spawn(&nessies[i], x, -GROUND_FLOOR_Y, z, pool, area);
    return i;
}

/* ===========================================================================
 * FIREBALLS — the Rabisu's (src/rabisu.c), with a fixed SPEED instead of a
 * fixed flight time (see nessie.h). Transient: not in the save blob.
 * =========================================================================== */
typedef struct {
    int32_t   x, y, z;
    int32_t   vx, vy, vz;
    int32_t   life;        /* frames left before it fizzles; 0 = free slot     */
    int32_t   age;         /* frames flown, so the parry window can be timed   */
    int32_t   flight;      /* nominal frames to the aim point                  */
    int32_t   deflected;   /* 1 = turned around and hunting its owner          */
    int32_t   owner;       /* index into nessies[] — who eats it if deflected  */
    GameState area;
} NssFireball;

static NssFireball nss_fireballs[MAX_NSS_FIREBALLS];

static void nss_fireballs_reset(void) {
    int i;
    for (i = 0; i < MAX_NSS_FIREBALLS; i++) nss_fireballs[i].life = 0;
}

static void nss_fireball_spawn(int owner, int32_t x, int32_t y, int32_t z,
                               int32_t tx, int32_t ty, int32_t tz,
                               GameState area) {
    int i;
    for (i = 0; i < MAX_NSS_FIREBALLS; i++)
        if (nss_fireballs[i].life <= 0) break;
    if (i >= MAX_NSS_FIREBALLS) return;

    int32_t dx = tx - x, dy = ty - y, dz = tz - z;
    int32_t len = nss_isqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0) return;

    NssFireball *f = &nss_fireballs[i];
    f->x = x; f->y = y; f->z = z;
    f->vx = (dx * NSS_FB_SPEED) / len;
    f->vy = (dy * NSS_FB_SPEED) / len;
    f->vz = (dz * NSS_FB_SPEED) / len;
    f->flight = len / NSS_FB_SPEED;
    if (f->flight < 1) f->flight = 1;
    /* Outlives the nominal arrival so a DODGED ball keeps sailing past and
       fizzles in the open rather than vanishing at the player's shoulder. */
    f->life      = f->flight * 2 + 30;
    f->age       = 0;
    f->deflected = 0;
    f->owner     = owner;
    f->area      = area;
    sound_play(SFX_FIREBALL);   /* the ball leaving the mouth */
}

/* Turn a ball around: double speed, straight back at the head. Re-aimed once
   here and then straight, so the deflect is a good hit rather than a sure one. */
static void nss_fireball_deflect(NssFireball *f) {
    Nessie *s = &nessies[f->owner];
    int32_t dx = s->x - f->x;
    int32_t dy = (s->y + NSS_Y_OFFSET) - f->y;
    int32_t dz = s->z - f->z;
    int32_t len = nss_isqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0) { f->life = 0; return; }

    f->vx = (dx * NSS_FB_SPEED * 2) / len;
    f->vy = (dy * NSS_FB_SPEED * 2) / len;
    f->vz = (dz * NSS_FB_SPEED * 2) / len;
    f->deflected = 1;
    f->life      = (len / (NSS_FB_SPEED * 2)) * 3 + 10;   /* only has to cross once */
    sound_play(SFX_AXEHIT);
}

static void nss_damage_sfx(Nessie *s, int dmg, SfxID hit_sfx);

static void nss_fireballs_update(void) {
    int i;
    for (i = 0; i < MAX_NSS_FIREBALLS; i++) {
        NssFireball *f = &nss_fireballs[i];
        if (f->life <= 0) continue;
        if (f->area != current_area) { f->life = 0; continue; }
        if (--f->life <= 0) continue;
        f->age++;

        int32_t ox = f->x, oy = f->y, oz = f->z;
        f->x += f->vx;
        f->y += f->vy;
        f->z += f->vz;

        /* The SEGMENT, not the new point, so a fast ball cannot tunnel through
           an outcrop between frames. */
        if (collision_segment_blocked(ox, oy, oz, f->x, f->y, f->z)) {
            f->life = 0;
            continue;
        }

        if (f->deflected) {
            /* Homeward leg: the only thing it can hit is its owner. */
            Nessie *s = &nessies[f->owner];
            if (s->active && s->state != NSS_DEAD) {
                int32_t hx = s->x - f->x;
                int32_t hy = (s->y + NSS_Y_OFFSET) - f->y;
                int32_t hz = s->z - f->z;
                if (hx * hx + hy * hy + hz * hz <=
                    (int32_t)NSS_FB_HEAD_RADIUS * NSS_FB_HEAD_RADIUS) {
                    f->life = 0;
                    spawn_blood_burst(f->x, f->y, f->z);
                    /* Its own shot going back in: the fireball's report, as the
                       Rabisu's is. */
                    nss_damage_sfx(s, 1, SFX_FIREBALL);
                }
            } else {
                f->life = 0;   /* it died to something else mid-return */
            }
            continue;
        }

        /* THE DEFLECT, live only in the last NSS_FB_PARRY frames before the
           nominal arrival — swing as it closes, not when it is halfway across
           the lava. swing_timer is only non-zero with the axe equipped. */
        if (f->age >= f->flight - NSS_FB_PARRY &&
            swing_timer > 0 && swing_timer <= NSS_FB_PARRY) {
            nss_fireball_deflect(f);
            continue;
        }

        /* Contact, radial in all three axes and against the SAME dropped point
           it was aimed at (the Rabisu's reasoning). Missing this IS the dodge. */
        if (!game_over) {
            int32_t hx = player_x() - f->x;
            int32_t hy = (player_y() + NSS_FB_AIM_DROP) - f->y;
            int32_t hz = player_z() - f->z;
            if (hx * hx + hy * hy + hz * hz <=
                (int32_t)NSS_FB_HIT_RADIUS * NSS_FB_HIT_RADIUS) {
                f->life = 0;
                player_hurt(NSS_FB_DAMAGE);
                sound_play(SFX_HURT);
                if (player_health <= 0) {
                    player_health = 0;
                    game_over     = 1;
                    flash_timer   = 90;
                    sound_play(SFX_DIE);
                }
            }
        }
    }
}

void nessies_init(void) {
    /* Placements are seeded on a room's first entry by world.c (world_seed_room).
       The array starts empty. */
    nessie_count = 0;
    nss_fireballs_reset();
}

void nessies_reset(void) {
    nessie_count = 0;
    nss_fireballs_reset();
}

void nessies_rest(void) {
    int i;
    for (i = 0; i < nessie_count; i++) {
        Nessie *s = &nessies[i];
        if (!s->active || s->state == NSS_DEAD) continue;
        nss_spawn(s, s->spawn_x, s->spawn_y, s->spawn_z, s->pool, s->area);
    }
    nss_fireballs_reset();
}

/* No weaknesses: 1 from any swing or round, so NSS_MAX_HEALTH hits. The
   placeholder line keeps the table shape. */
static const Weakness nessie_weakness[] = {
    { DMG_KINETIC, 100 },   /* 100% = no change */
};

/* >>> FLAME ROUNDS ARE REFUSED HERE, NOT IN THE TABLE. <<< damage_scale never
   rounds a hit below 1 (damage.h: "an enemy that ignored a whole ammo type
   would read as a broken gun"), so a { DMG_FLAME, 0 } entry would still do 1.
   Immunity is that exception, asked for by name - she is made of lava - so it
   is answered before the table and returns 0, which nessie_damage treats as a
   hit that wakes her and does nothing else. DMG_HOLY (the Helluminator) is not
   flame and still burns her for 1. */
int32_t nessie_scale_damage(int32_t base, DamageType type) {
    if (type == DMG_FLAME) return 0;
    return damage_scale(base, type, nessie_weakness,
                        WEAKNESS_COUNT(nessie_weakness));
}

void nessie_body(const Nessie *s, int32_t *cyc, int32_t *hh, int32_t *hw) {
    *cyc = s->y + NSS_Y_OFFSET;
    *hh  = NSS_HALF_H;
    *hw  = NSS_HALF_W;
}

/* The burst's burn: NSS_SPLASH_DAMAGE to a player within NSS_SPLASH_RADIUS
   (true radial, see nessie.h for why it is measured past the hold distance),
   once, on the frame she dies. The player's body span must overlap hers, as
   nessies_collide asks, so it can never land on someone a storey away. Gated
   on game_over like the fireball's contact: a player already dead is not hurt
   again. */
static void nss_lava_splash(const Nessie *s) {
    if (game_over) return;
    int32_t py  = player_y();
    int32_t top = s->y + NSS_Y_OFFSET - NSS_HALF_H;
    int32_t bot = s->y + NSS_Y_OFFSET + NSS_HALF_H;
    if (py + GROUND_FLOOR_Y < top || py - 30 > bot) return;
    int32_t dx = player_x() - s->x;
    int32_t dz = player_z() - s->z;
    if (dx * dx + dz * dz > (int32_t)NSS_SPLASH_RADIUS * NSS_SPLASH_RADIUS) return;
    player_hurt(NSS_SPLASH_DAMAGE);
    sound_play(SFX_HURT);
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
        sound_play(SFX_DIE);
    }
}

static void nss_damage_sfx(Nessie *s, int dmg, SfxID hit_sfx) {
    if (!s->active || s->state == NSS_DEAD) return;
    /* Hit while idle: that is an alert, whatever the distance. The countdown
       is ARMED here (mistake 8: a zeroed timer would fire on frame one). A hit
       mid-attack does not cancel it. */
    if (s->state == NSS_IDLE) {
        s->state     = NSS_ALERT;
        s->atk_timer = NSS_ATTACK_EVERY;
    }
    /* An immune hit (a Flame Round, nessie_scale_damage): found, not hurt - no
       health lost, no bar, no hit sound. */
    if (dmg <= 0) return;
    s->health   -= dmg;
    s->hit_timer = NSS_BAR_TIMER_MAX;
    if (s->health <= 0) {
        s->health = 0;
        s->state  = NSS_DEAD;
        spawn_lava_burst(s->x, s->y + NSS_Y_OFFSET, s->z);
        sound_play(SFX_NSS_DIE);
        nss_lava_splash(s);
    } else {
        sound_play(hit_sfx);
    }
}

void nessie_damage(Nessie *s, int dmg) {
    nss_damage_sfx(s, dmg, SFX_AXEHIT);
}

/* The crucifaxe test, measured to the push surface: lumberers_try_hit with no
   shove — the head is rooted in its lava, and a knockback would only be a way
   to push it out of the pool. */
int nessies_try_hit(void) {
    int i;
    for (i = 0; i < nessie_count; i++) {
        Nessie *s = &nessies[i];
        if (!s->active || s->state == NSS_DEAD || s->area != current_area) continue;
        int32_t dx = s->x - cam_x;
        int32_t dy = (s->y + NSS_Y_OFFSET) - cam_y;
        int32_t dz = s->z - cam_z;
        int32_t d  = nss_isqrt(dx * dx + dz * dz);
        int32_t gh = d > NSS_PUSH_RADIUS ? d - NSS_PUSH_RADIUS : 0;
        if (gh + (dy < 0 ? -dy : dy) >= SWING_RANGE) continue;
        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;
        nessie_damage(s, 1);
        return 1;
    }
    return 0;
}

/* The push-out: lumberers_collide's capped radial push, so it can never throw
   the player through a wall. The vertical gate is the body's span against the
   player's, as theirs. */
void nessies_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < nessie_count; i++) {
        Nessie *s = &nessies[i];
        if (!s->active || s->state == NSS_DEAD || s->area != current_area) continue;

        int32_t top = s->y + NSS_Y_OFFSET - NSS_HALF_H;
        int32_t bot = s->y + NSS_Y_OFFSET + NSS_HALF_H;
        if (py + GROUND_FLOOR_Y < top || py - 30 > bot) continue;

        int32_t dx   = *px - s->x;
        int32_t dz   = *pz - s->z;
        int32_t need = NSS_PUSH_RADIUS + radius;
        int32_t d    = nss_isqrt(dx * dx + dz * dz);
        if (d >= need) continue;
        int32_t step = need - d;
        if (step > NSS_PUSH_STEP) step = NSS_PUSH_STEP;
        if (d == 0) { *px += step; continue; }   /* dead centre */
        *px += (dx * step) / d;
        *pz += (dz * step) / d;
    }
}

/* ---- Movement ----------------------------------------------------------------
   Try one step of (dqx,dqz), in 1/256 units. Refused — and the body left
   exactly where it was — if the new centre is off the pool's lava, or if the
   rim or an outcrop pushes back AGAINST the step. A push that does not oppose
   the step (a body already inside NSS_BODY_RADIUS of a wall, moving away from
   it) is accepted with the correction, or a head spawned near rock could never
   swim clear. Returns 1 if it moved. */
static int nss_try_move(Nessie *s, int32_t dqx, int32_t dqz) {
    int32_t nqx = s->qx + dqx, nqz = s->qz + dqz;
    int32_t nx  = nqx >> 8,    nz  = nqz >> 8;
    if (!nss_in_pool(s, nx, nz)) return 0;

    int32_t cx = nx, cz = nz;
    apply_flat_entity_collision(&cx, &cz, NSS_BODY_RADIUS);
    if (cx != nx || cz != nz) {
        if ((cx - nx) * dqx + (cz - nz) * dqz < 0) return 0;   /* walked into it */
        if (!nss_in_pool(s, cx, cz)) return 0;
        nqx = cx << 8;
        nqz = cz << 8;
    }
    s->qx = nqx; s->qz = nqz;
    s->x  = nqx >> 8;
    s->z  = nqz >> 8;
    return 1;
}

/* IDLE: hold the heading for a leg, then pick another. A refused step is a
   wall: reverse on the spot with a fresh leg — "the opposite trajectory". If
   the step straight after a reversal is refused too (a corner, or a pool edge
   that runs both ways) take a random heading instead, and keep doing so until
   one swims. */
static void nss_swim_idle(Nessie *s) {
    if (--s->leg <= 0) { nss_random_heading(s); s->bumped = 0; }
    int32_t dqx = (s->hx * NSS_SPEED_Q8) >> 12;
    int32_t dqz = (s->hz * NSS_SPEED_Q8) >> 12;
    if (nss_try_move(s, dqx, dqz)) { s->bumped = 0; return; }
    if (s->bumped) {
        nss_random_heading(s);
    } else {
        s->hx  = -s->hx;
        s->hz  = -s->hz;
        s->leg = NSS_LEG_FRAMES;
    }
    s->bumped = 1;
}

/* ALERT: straight at (gx,gz). When the straight step runs off the lava, slide
   along the edge on whichever axis still has lava, which is what brings the
   head right up to the bank nearest the player and keeps it there. */
static void nss_swim_toward(Nessie *s, int32_t gx, int32_t gz) {
    int32_t dx = gx - s->x, dz = gz - s->z;
    int32_t d  = nss_isqrt(dx * dx + dz * dz);
    if (d <= 0) return;
    int32_t dqx = (dx * NSS_SPEED_Q8) / d;
    int32_t dqz = (dz * NSS_SPEED_Q8) / d;
    if (nss_try_move(s, dqx, dqz)) return;
    if (dqx && nss_try_move(s, dqx, 0)) return;
    if (dqz) nss_try_move(s, 0, dqz);
}

static int32_t nss_mouth_y(const Nessie *s) {
    return s->y + NSS_Y_OFFSET - NSS_HALF_H + NSS_MOUTH_DROP;
}

void update_nessies(void) {
    int i;
    for (i = 0; i < nessie_count; i++) {
        Nessie *s = &nessies[i];
        /* current_area, NEVER game_state (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!s->active || s->state == NSS_DEAD || s->area != current_area) continue;

        if (s->hit_timer > 0) s->hit_timer--;
        s->anim_tick++;

        apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                          &s->on_upper_floor, &s->on_ramp);

        /* player_x/y/z, not cam_*: enemies keep chasing the PLAYER through a
           camera-locked shot (camera.h). Manhattan, because the fog it is
           compared with is the Manhattan cull every sprite draws by. */
        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - s->x, dz = pz - s->z;
        int32_t wdist  = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int     in_fog = wdist < g_fog_far;

        switch (s->state) {

        case NSS_IDLE:
            if (in_fog) {
                s->state     = NSS_ALERT;
                s->atk_timer = NSS_ATTACK_EVERY;   /* armed on waking (mistake 8) */
                break;
            }
            nss_swim_idle(s);
            break;

        case NSS_ALERT: {
            if (!in_fog) {
                s->state = NSS_IDLE;
                nss_random_heading(s);
                break;
            }
            if (s->atk_timer > 0) s->atk_timer--;
            /* The countdown expires into an attack only with a clear line from
               the mouth to where the ball would be aimed; until then it waits,
               still swimming toward the player. */
            if (s->atk_timer == 0 &&
                !collision_segment_blocked(s->x, nss_mouth_y(s), s->z,
                                           px, py + NSS_FB_AIM_DROP, pz)) {
                s->state    = NSS_PULL;
                s->atk_tick = 0;
                sound_play(SFX_PULL);
                break;
            }
            if (dx * dx + dz * dz > (int32_t)NSS_STOP_DIST * NSS_STOP_DIST)
                nss_swim_toward(s, px, pz);
            break;
        }

        case NSS_PULL:
            if (++s->atk_tick >= NSS_PULL_FRAMES) {
                s->state    = NSS_SPIT;
                s->atk_tick = 0;
                /* The ball and its sound together, on the first frame of the
                   spit: aimed ONCE, at where the player is now. */
                nss_fireball_spawn(i, s->x, nss_mouth_y(s), s->z,
                                   px, py + NSS_FB_AIM_DROP, pz, s->area);
            }
            break;

        case NSS_SPIT:
            if (++s->atk_tick >= NSS_SPIT_FRAMES) {
                s->state    = NSS_HOLD;
                s->atk_tick = 0;
            }
            break;

        case NSS_HOLD:
            if (++s->atk_tick >= NSS_HOLD_FRAMES) {
                /* Back to the swim, and three more seconds. A player who has
                   left the fog meanwhile drops it to IDLE on the next frame. */
                s->state     = NSS_ALERT;
                s->atk_tick  = 0;
                s->atk_timer = NSS_ATTACK_EVERY;
            }
            break;

        default:
            break;
        }
    }

    nss_fireballs_update();
}

/* ---- The texture-window bracket: lumberer.c's, verbatim in effect. addPrim
   PREPENDS within a bucket, so the restore goes in first and the disable last,
   and the GPU draws disable, sprite, restore. The caller reserves the space. */
static void nss_bracket_begin(RenderContext *ctx, int32_t otz) {
    if (!nss_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
    setTexWindow(restore, &nss_tw_restore);
    addPrim(&ot[otz], restore);
    ctx->next_packet += sizeof(DR_TWIN);
}

static void nss_bracket_end(RenderContext *ctx, int32_t otz) {
    if (!nss_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    RECT full = { 0, 0, 0, 0 };   /* mask 0 = no wrapping, full page */
    DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
    setTexWindow(disable, &full);
    addPrim(&ot[otz], disable);
    ctx->next_packet += sizeof(DR_TWIN);
}

/* Which of the six frames: the attack's by phase, half a clip each, else the
   swim pair one a second. */
static int nss_frame(const Nessie *s) {
    switch (s->state) {
    case NSS_PULL: return s->atk_tick < NSS_PULL_FRAMES / 2 ? 0 : 1;
    case NSS_SPIT: return s->atk_tick < NSS_SPIT_FRAMES / 2 ? 2 : 3;
    case NSS_HOLD: return 3;
    default:       return 4 + ((s->anim_tick / NSS_ANIM_RATE) & 1);
    }
}

/* One camera-facing quad. Frame f is in sheet f/3, column f%3; the columns are
   85, 85 and 86 texels wide (256 does not divide by three), so the bounds are
   computed, and inset one texel on every edge so a magnified quad cannot
   sample the neighbouring frame. V is the row within the 256-tall page, and the
   sheets sit at y=128, so 129..254. No gte_nclip: a camera-facing quad has no
   back face (mistake 4). */
static void draw_nss_sprite(RenderContext *ctx, Nessie *s, int frame) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);
    int32_t cx = s->x, cz = s->z, cy = s->y + NSS_Y_OFFSET;

    int16_t dwx = (int16_t)((NSS_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((NSS_HALF_W * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - NSS_HALF_H);
    int16_t vy_bot = (int16_t)(cy + NSS_HALF_H);

    /* POLY_FT4 order: TL, TR, BL, BR. */
    SVECTOR v[4];
    v[0].vx = (int16_t)(cx - dwx); v[0].vy = vy_top; v[0].vz = (int16_t)(cz - dwz); v[0].pad = 0;
    v[1].vx = (int16_t)(cx + dwx); v[1].vy = vy_top; v[1].vz = (int16_t)(cz + dwz); v[1].pad = 0;
    v[2].vx = (int16_t)(cx - dwx); v[2].vy = vy_bot; v[2].vz = (int16_t)(cz - dwz); v[2].pad = 0;
    v[3].vx = (int16_t)(cx + dwx); v[3].vy = vy_bot; v[3].vz = (int16_t)(cz + dwz); v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4], otz;
    int k;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return;
    for (k = 0; k < 4; k++)
        if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
            sv[k].vy <= -1023 || sv[k].vy >= 1023) return;

    gte_avsz4();
    gte_stotz(&otz);
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
    size_t   need    = sizeof(POLY_FT4) + (nss_tw_active ? 2 * sizeof(DR_TWIN) : 0);
    if (ctx->next_packet + need > buf_end) return;

    int col = frame % 3;
    int u0  = (col * 256) / 3;          /* 0, 85, 170  */
    int u1  = ((col + 1) * 256) / 3;    /* 85, 170, 256 */

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    if (s->hit_timer > 0) setRGB0(poly, fog8, fog8 >> 2, fog8 >> 2);
    else                  setRGB0(poly, fog8, fog8, fog8);
    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
    poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
    poly->u0 = (uint8_t)(u0 + 1); poly->v0 = 129;
    poly->u1 = (uint8_t)(u1 - 2); poly->v1 = 129;
    poly->u2 = (uint8_t)(u0 + 1); poly->v2 = 254;
    poly->u3 = (uint8_t)(u1 - 2); poly->v3 = 254;
    poly->tpage = nss_tpage[frame / 3];
    poly->clut  = nss_clut [frame / 3];
    ctx->next_packet += sizeof(POLY_FT4);

    nss_bracket_begin(ctx, otz);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    nss_bracket_end(ctx, otz);

    if (s->hit_timer <= 0) return;

    /* The health bar: lumberer.c's two TILEs. */
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

    int16_t fill_w = (int16_t)((s->health * 40) / NSS_MAX_HEALTH);
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

/* A fireball: draw_rbs_fireball's solid cube — six faces, one flat colour, one
   OT depth each, no winding to get wrong. Orange-red going out, white-hot on
   the way back. */
static void draw_nss_fireball(RenderContext *ctx, const NssFireball *f) {
    static const int8_t corner[8][3] = {
        { -1, -1, -1 }, {  1, -1, -1 }, {  1, -1,  1 }, { -1, -1,  1 },
        { -1,  1, -1 }, {  1,  1, -1 }, {  1,  1,  1 }, { -1,  1,  1 },
    };
    static const uint8_t face[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
        { 3, 2, 6, 7 }, { 0, 3, 7, 4 }, { 1, 2, 6, 5 },
    };

    int32_t fdx  = f->x - cam_x;
    int32_t fdz  = f->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    if (dist >= g_fog_far) return;
    int32_t fs = render_fog_scale(dist);
    if (fs > 255) fs = 255;

    uint8_t cr, cg, cb;
    if (f->deflected) { cr = 255; cg = 240; cb = 190; }
    else              { cr = 255; cg =  70; cb =  20; }
    cr = (uint8_t)((cr * fs) >> 8);
    cg = (uint8_t)((cg * fs) >> 8);
    cb = (uint8_t)((cb * fs) >> 8);

    SVECTOR v[8];
    int c;
    for (c = 0; c < 8; c++) {
        v[c].vx  = (int16_t)(f->x + corner[c][0] * NSS_FB_HALF);
        v[c].vy  = (int16_t)(f->y + corner[c][1] * NSS_FB_HALF);
        v[c].vz  = (int16_t)(f->z + corner[c][2] * NSS_FB_HALF);
        v[c].pad = 0;
    }

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int fi;
    for (fi = 0; fi < 6; fi++) {
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

        DVECTOR sv[4];
        int32_t sz[4], otz;

        gte_ldv3(&v[face[fi][0]], &v[face[fi][1]], &v[face[fi][2]]);
        gte_rtpt();
        gte_stsxy3c(sv);
        gte_ldv0(&v[face[fi][3]]);
        gte_rtps();
        gte_stsxy(&sv[3]);
        gte_stsz4c(sz);
        if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) continue;

        int k;
        for (k = 0; k < 4; k++)
            if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
                sv[k].vy <= -1023 || sv[k].vy >= 1023) break;
        if (k < 4) continue;

        gte_avsz4();
        gte_stotz(&otz);
        if (otz <= 0) continue;
        if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, cr, cg, cb);
        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
        poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

void draw_nessies(RenderContext *ctx) {
    int i;
    if (tex_loaded) {
        for (i = 0; i < nessie_count; i++) {
            Nessie *s = &nessies[i];
            if (!s->active || s->state == NSS_DEAD || s->area != current_area) continue;

            int32_t dx = s->x - cam_x;
            int32_t dz = s->z - cam_z;
            int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (wdist >= g_fog_far) continue;
            /* Behind an outcrop: skip the fill (collision.h). Not inside the
               push-out's reach, where it is in the player's face whatever
               stands between. */
            if (wdist > NSS_HOLD_DIST * 2 &&
                collision_hidden_from_camera(s->x, s->y + NSS_Y_OFFSET, s->z))
                continue;

            draw_nss_sprite(ctx, s, nss_frame(s));
        }
    }

    for (i = 0; i < MAX_NSS_FIREBALLS; i++) {
        NssFireball *f = &nss_fireballs[i];
        if (f->life <= 0 || f->area != current_area) continue;
        draw_nss_fireball(ctx, f);
    }
}
