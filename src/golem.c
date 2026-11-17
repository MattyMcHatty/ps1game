#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"      /* player_x/y/z, cam_*                              */
#include "player.h"      /* player_hurt, player_health, game_over, flash_timer */
#include "collision.h"   /* apply_ddog_height, collision_segment_blocked, ... */
#include "texmgr.h"
#include "sound.h"
#include "rabisu.h"      /* rbs_glow_quad: Asag's beam is drawn with it too  */
#include "underworld.h"  /* UW_FOG_*: the shards fog to the chapter's red    */
#include "underworld_sky.h" /* UW_SKY_OT: nothing may sort behind the sky    */
#include "golem.h"

/* The Golem - see golem.h for the design, the numbers and why. */

Golem golems[MAX_GOLEMS];
int   golem_count = 0;

/* ---- Sprites ----------------------------------------------------------------
   One sheet per ROW of the source art: GOLEMA is frames 1-3, GOLEMB 4-6, both
   quantised against one palette (tools/split_golem_sheet.py). In this file a
   frame is 0..5 - the design's number less one - so frame / 3 is the sheet and
   frame % 3 the column. */
#define GLM_SHEETS 2
static uint16_t glm_tpage [GLM_SHEETS] = { 0, 0 };
static uint16_t glm_clut  [GLM_SHEETS] = { 0, 0 };
static int      glm_tex_id[GLM_SHEETS] = { -1, -1 };
static int      tex_loaded = 0;

#define GLM_F_IDLE_A   0   /* frame 1 */
#define GLM_F_GLOW     1   /* frame 2 */
#define GLM_F_FIRE     2   /* frame 3 */
#define GLM_F_RAISE    3   /* frame 4 */
#define GLM_F_SLAM     4   /* frame 5 */
#define GLM_F_IDLE_B   5   /* frame 6 */

/* The three columns are cut at 0 / 85 / 171 / 256 - NOT an even third (256
   does not divide by three). tools/split_golem_sheet.py's doc says the same. */
static const uint8_t GLM_COL_U0[3] = { 0, 85, 171 };
static const uint8_t GLM_COL_U1[3] = { 85, 171, 255 };   /* 256 does not fit a u8 */

/* Texture window the current area expects, restored after each sprite (the
   sheets are at Voff 128). The Ahriman's bracket. */
static RECT glm_tw_restore;
static int  glm_tw_active = 0;

void golems_set_texwindow(const RECT *tw) {
    if (tw) { glm_tw_restore = *tw; glm_tw_active = 1; }
    else    { glm_tw_active = 0; }
}

void golems_load_textures(void) {
    /* BANK: Chapter 4 only. py tools/check_tex_banks.py checks it. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    glm_tex_id[0] = texmgr_register("\\TEXUNDW\\GOLEMA.TIM;1");
    glm_tex_id[1] = texmgr_register("\\TEXUNDW\\GOLEMB.TIM;1");
    {
        int i;
        for (i = 0; i < GLM_SHEETS; i++) {
            glm_tpage[i] = texmgr_tpage(glm_tex_id[i]);
            glm_clut [i] = texmgr_clut (glm_tex_id[i]);
        }
    }
    tex_loaded = (glm_tex_id[0] >= 0 && glm_tex_id[1] >= 0);
}

/* Nothing to mark taken: the crawler blocks' second halves are put back by
   main.c's destination-keyed lines on entry to any room that draws their
   owners, and the Underworld is left only through the title (golem.h). */
void golems_upload_textures(void) {
    int i;
    for (i = 0; i < GLM_SHEETS; i++) texmgr_upload(glm_tex_id[i]);
}

/* The rooms whose entry uploads the sheet. main.c keys the upload on this, and
   tools/gen_tex_rooms.py mirrors it as GOLEM_ROOMS - keep the two in step. */
int golem_room_has_sheet(GameState area) {
    return area == STATE_WINGSCAPE;
}

/* Integer square root, ahriman.c's - including the `t > 0` seed (mistake 11 in
   tools/ADDING_AN_ENEMY.txt). Inputs here stay under ~1e8. */
static int32_t glm_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

static int32_t glm_abs(int32_t v) { return v < 0 ? -v : v; }

static int32_t glm_body_y(const Golem *s) { return s->y + GLM_Y_OFFSET; }

void golem_body(const Golem *s, int32_t *cyc, int32_t *hh, int32_t *hw) {
    *cyc = glm_body_y(s);
    *hh  = GLM_HIT_HALF_H;
    *hw  = GLM_HIT_HALF_W;
}

/* ---- The shards -------------------------------------------------------------
   Sharur's Tablet's cubes (src/sharur_tablet.c, sh_shatter), on the brief's
   three changes: browns and reds instead of the slab's greys, a little smaller
   (half 22..33 against the tablet's 40..60), and kept to the Golem's own width
   - spread across the billboard, thrown out gently, so the burst stays inside
   the body's outline rather than flying across the room. Same physics (+3
   gravity a frame, sideways speed 28/32) and the same floor bounce.

   ONE pool for every Golem: a burst outlives its Golem but not its room, so it
   is area-tagged and golems_rest clears it. */
#define GLM_SHARDS        12
#define GLM_SHARD_SHRINK  20

static const uint8_t GLM_SHARD_RGB[8][3] = {
    {  92,  56,  36 }, { 120,  74,  46 }, { 146,  96,  60 }, {  70,  42,  28 },
    { 150,  40,  20 }, { 182,  62,  26 }, { 118,  30,  16 }, { 200,  84,  32 },
};
static const uint16_t GLM_FACE_SHADE[6] = { 150, 200, 256, 110, 175, 225 };

typedef struct {
    int32_t x, y, z, vx, vy, vz, floor_y;
    int16_t rx, ry, sx, sy;
    int16_t half, life, max_life;
    uint8_t col;
    GameState area;
} GlmShard;

static GlmShard glm_shards[GLM_SHARDS];

static uint32_t glm_rng = 0x6C3A1u;
static int32_t glm_rand(int32_t n) {      /* 0..n-1 */
    glm_rng = glm_rng * 1103515245u + 12345u;
    return n > 0 ? (int32_t)((glm_rng >> 16) % (uint32_t)n) : 0;
}

static void glm_shatter(const Golem *s) {
    /* Across the billboard as the player sees it at the moment of death: the
       sprite's own right vector. */
    int32_t rx = icos(cam_rot), rz = -isin(cam_rot);
    int32_t cy = glm_body_y(s);
    int32_t span = (GLM_HALF_W * 4) / 5;
    int i;
    for (i = 0; i < GLM_SHARDS; i++) {
        GlmShard *h = &glm_shards[i];
        int32_t a = -span + ((2 * span) * (2 * i + 1)) / (2 * GLM_SHARDS)
                    + glm_rand(41) - 20;
        h->x = s->x + ((a * rx) >> 12);
        h->z = s->z + ((a * rz) >> 12);
        h->y = cy - (GLM_HALF_H * 3) / 4 + glm_rand((GLM_HALF_H * 5) / 4);
        /* A little outward along the billboard, a little toward or away from
           the camera - small, so the burst keeps the body's width. */
        {
            int32_t along = (a * 6) / span + glm_rand(5) - 2;
            int32_t out   = glm_rand(9) - 4;
            h->vx = (along * rx + out * rz) >> 12;
            h->vz = (along * rz - out * rx) >> 12;
        }
        h->vy  = -(6 + glm_rand(10));
        h->rx  = (int16_t)glm_rand(4096);
        h->ry  = (int16_t)glm_rand(4096);
        h->sx  = (int16_t)(glm_rand(81) - 40);
        h->sy  = (int16_t)(glm_rand(81) - 40);
        h->half     = (int16_t)(22 + glm_rand(12));
        h->life     = (int16_t)(70 + glm_rand(31));
        h->max_life = h->life;
        h->col      = (uint8_t)glm_rand(8);
        h->floor_y  = s->y + GROUND_FLOOR_Y;
        h->area     = s->area;
    }
}

static void glm_update_shards(void) {
    int i;
    for (i = 0; i < GLM_SHARDS; i++) {
        GlmShard *h = &glm_shards[i];
        if (h->life <= 0 || h->area != current_area) continue;
        h->x += h->vx;
        h->y += h->vy;
        h->z += h->vz;
        h->vy += 3;
        h->vx = (h->vx * 28) >> 5;
        h->vz = (h->vz * 28) >> 5;
        h->rx = (int16_t)((h->rx + h->sx) & 4095);
        h->ry = (int16_t)((h->ry + h->sy) & 4095);
        if (h->y > h->floor_y - h->half) {
            h->y = h->floor_y - h->half;
            if (h->vy > 0) h->vy = -(h->vy * 3) / 10;
            if (h->vy > -3) h->vy = 0;
            h->sx = (int16_t)((h->sx * 3) / 4);
            h->sy = (int16_t)((h->sy * 3) / 4);
        }
        h->life--;
    }
}

static void glm_clear_shards(void) {
    int i;
    for (i = 0; i < GLM_SHARDS; i++) glm_shards[i].life = 0;
}

/* ---- Spawning ---------------------------------------------------------------- */

/* Rebuild exactly as golem_add leaves one. Only the spawn and the area survive
   the wipe. */
static void glm_spawn(Golem *s, int32_t x, int32_t y, int32_t z, GameState area) {
    int guard = s->guard;          /* a summoned Golem stays one (golem.h) */
    *s = (Golem){0};
    s->guard = guard;
    s->x = x; s->y = y; s->z = z;
    s->spawn_x = x; s->spawn_z = z; s->spawn_y = y;
    s->health  = GLM_MAX_HEALTH * GLM_HP_SCALE;
    s->state   = GLM_IDLE;
    s->active  = 1;
    s->area    = area;
}

int golem_add(int32_t x, int32_t z, GameState area) {
    if (golem_count >= MAX_GOLEMS) return -1;
    int i = golem_count++;
    /* The standing anchor of a y=0 floor (ahriman_add's reasoning): authored,
       and apply_ddog_height corrects it on the first frame if the floor is not
       at 0. */
    golems[i].guard = 0;           /* a reused slot may have been a guard */
    glm_spawn(&golems[i], x, -GROUND_FLOOR_Y, z, area);
    /* Out of step with each other, so two in a room do not breathe as one. */
    golems[i].anim_tick = i * 17;
    return i;
}

void golem_set_spawn(int idx, int32_t x, int32_t z) {
    if (idx < 0 || idx >= golem_count) return;
    Golem *s = &golems[idx];
    if (!s->active || s->state == GLM_DEAD) return;
    glm_spawn(s, x, s->spawn_y, z, s->area);
    s->anim_tick = idx * 17;
}

/* ---- Summoned Golems (golem.h) ---------------------------------------------
   Planted at the FLOOR's anchor, not y=0's: apply_ddog_height takes the first
   zone not above the body, so one planted at the ground's anchor under a
   terrace would stay on the ground. */
static void glm_decide(Golem *s);

int golem_add_guard(int32_t x, int32_t floor_y, int32_t z, GameState area) {
    if (golem_count >= MAX_GOLEMS) return -1;
    int i = golem_count++;
    golems[i].guard = 1;
    glm_spawn(&golems[i], x, floor_y - GROUND_FLOOR_Y, z, area);
    golems[i].anim_tick = i * 17;
    golems[i].state     = GLM_DEAD;      /* ABSENT until summoned */
    return i;
}

Golem *golem_guard(GameState area) {
    int i;
    for (i = 0; i < golem_count; i++)
        if (golems[i].guard && golems[i].area == area) return &golems[i];
    return NULL;
}

void golem_revive(Golem *s) {
    if (!s) return;
    glm_spawn(s, s->spawn_x, s->spawn_y, s->spawn_z, s->area);
    s->anim_tick = (int)(s - golems) * 17;
}

void golem_summon(Golem *s) {
    if (!s) return;
    golem_revive(s);
    /* Awake at once, as the first hit would leave it - "immediately active
       and attacking". */
    glm_decide(s);
}

void golems_init(void) {
    golem_count = 0;
    glm_clear_shards();
}

void golems_reset(void) {
    golem_count = 0;
    glm_clear_shards();
}

void golems_rest(void) {
    int i, firing = 0;
    for (i = 0; i < golem_count; i++) {
        Golem *s = &golems[i];
        if (!s->active || s->state == GLM_DEAD) continue;
        if (s->state == GLM_LAS_FIRING) firing = 1;
        glm_spawn(s, s->spawn_x, s->spawn_y, s->spawn_z, s->area);
        s->anim_tick = i * 17;
    }
    /* A beam cut off by the door takes its sound with it. */
    if (firing) sound_stop(SFX_LASER);
    glm_clear_shards();
}

/* ---- Damage ------------------------------------------------------------------
   HALF-POINTS (golem.h). Flame Rounds do half, everything else - the
   Helluminator included - full. */
int32_t golem_scale_damage(int32_t base, DamageType type) {
    if (base <= 0) return 0;
    if (type == DMG_FLAME) return base * GLM_HP_SCALE / 2;
    return base * GLM_HP_SCALE;
}

static int glm_same_storey(const Golem *s, int32_t py) {
    return glm_abs(py - s->y) < GLM_STOREY_REACH;
}

static int glm_in_punch_range(const Golem *s) {
    int32_t dx = player_x() - s->x, dz = player_z() - s->z;
    return glm_isqrt(dx * dx + dz * dz) <= GLM_PUNCH_RADIUS &&
           glm_same_storey(s, player_y());
}

/* A decision point: after waking, after a punch, after a laser cycle. Nothing
   else changes what it is doing - a laser cycle always finishes (golem.h). */
static void glm_decide(Golem *s) {
    s->timer = 0;
    s->state = glm_in_punch_range(s) ? GLM_WINDUP : GLM_LAS_WAITING;
}

void golem_damage(Golem *s, int dmg) {
    if (!s->active || s->state == GLM_DEAD || dmg <= 0) return;
    /* The first hit wakes it - from any weapon, at any range. */
    if (s->state == GLM_IDLE) glm_decide(s);
    s->health   -= dmg;
    s->hit_timer = GLM_BAR_TIMER_MAX;
    if (s->health <= 0) {
        s->health = 0;
        s->state  = GLM_DEAD;
        s->wave_t = 0;          /* a wave in the air dies with the arm */
        glm_shatter(s);
        sound_play(SFX_RUMBLE);
    } else {
        sound_play(SFX_AXEHIT);
    }
}

/* The crucifaxe test, measured to the push surface (GLM_AXE_REACH) plus the
   vertical gap to the body's centre, with the facing dot. No knockback. */
int golems_try_hit(void) {
    int i;
    for (i = 0; i < golem_count; i++) {
        Golem *s = &golems[i];
        if (!s->active || s->state == GLM_DEAD || s->area != current_area) continue;
        int32_t dx = s->x - cam_x;
        int32_t dy = glm_body_y(s) - cam_y;
        int32_t dz = s->z - cam_z;
        int32_t d  = glm_isqrt(dx * dx + dz * dz);
        int32_t gh = d > GLM_PUSH_RADIUS ? d - GLM_PUSH_RADIUS : 0;
        if (gh + glm_abs(dy) >= SWING_RANGE) continue;
        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;
        golem_damage(s, GLM_HP_SCALE);   /* a swing is a whole hit point */
        return 1;
    }
    return 0;
}

/* The plug: lumberers_collide, on the Golem's radius. Capped per frame so it
   can never carry the player through a wall. */
void golems_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < golem_count; i++) {
        Golem *s = &golems[i];
        if (!s->active || s->state == GLM_DEAD || s->area != current_area) continue;

        int32_t top = glm_body_y(s) - GLM_HALF_H;
        int32_t bot = glm_body_y(s) + GLM_HALF_H;
        if (py + GROUND_FLOOR_Y < top || py - 30 > bot) continue;

        int32_t dx   = *px - s->x;
        int32_t dz   = *pz - s->z;
        int32_t need = GLM_PUSH_RADIUS + radius;
        int32_t d    = glm_isqrt(dx * dx + dz * dz);
        if (d >= need) continue;
        int32_t step = need - d;
        if (step > GLM_PUSH_STEP) step = GLM_PUSH_STEP;
        if (d == 0) { *px += step; continue; }
        *px += (dx * step) / d;
        *pz += (dz * step) / d;
    }
}

/* ---- Hurting the player ------------------------------------------------------ */
static void glm_hurt_player(int32_t amount) {
    if (game_over) return;
    player_hurt(amount);
    sound_play(SFX_HURT);
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
        sound_play(SFX_DIE);
    }
}

/* ---- The shockwave: lmb_shock_tick, from the body's edge (golem.h). ------- */
static int32_t glm_wave_reach(const Golem *s) {
    if (s->wave_t >= GLM_SHOCK_EXPAND) return GLM_SHOCK_REACH;
    return GLM_PUSH_RADIUS + (GLM_SHOCK_TRAVEL * s->wave_t) / GLM_SHOCK_EXPAND;
}

static void glm_shock_tick(Golem *s) {
    if (s->wave_t <= 0) return;
    s->wave_t++;
    if (s->wave_t > GLM_SHOCK_EXPAND + GLM_SHOCK_LINGER) { s->wave_t = 0; return; }
    if (s->wave_hit) return;

    int32_t dx = player_x() - s->x;
    int32_t dz = player_z() - s->z;
    if (glm_isqrt(dx * dx + dz * dz) > glm_wave_reach(s)) return;
    if (!glm_same_storey(s, player_y())) return;
    s->wave_hit = 1;
    glm_hurt_player(GLM_SHOCK_DAMAGE);
}

/* ---- The laser ---------------------------------------------------------------
   A core point on the billboard: `side` units along its right vector (the
   camera's, as draw_glm_sprite uses) and `dy` from the sprite's centre. The
   two laser frames put their cores in different places (golem.h). */
static void glm_core(const Golem *s, int32_t side, int32_t dy,
                     int32_t *x, int32_t *y, int32_t *z) {
    *x = s->x + ((side * icos(cam_rot)) >> 12);
    *y = glm_body_y(s) + dy;
    *z = s->z + ((side * -isin(cam_rot)) >> 12);
}

/* Aim: the spider's - straight at the player's anchor (web_spawn's target),
   from frame 3's core, which is where the beam leaves. */
static void glm_aim(Golem *s) {
    glm_core(s, GLM_FIRE_CORE_X, GLM_FIRE_CORE_DY, &s->bx0, &s->by0, &s->bz0);
    s->bx1 = player_x();
    s->by1 = player_y();
    s->bz1 = player_z();
}

/* LOCK: run the aimed line on to GLM_LAS_MAX_LEN, then cut it at the floor
   under the Golem and at the first wall or solid prop. Done ONCE, on the frame
   the beam starts - the beam never moves after that (golem.h). The wall cut is
   a bisection on collision_segment_blocked: nine segment tests, once a cycle. */
static void glm_lock_beam(Golem *s) {
    int32_t ox = s->bx0, oy = s->by0, oz = s->bz0;
    int32_t dx = s->bx1 - ox, dy = s->by1 - oy, dz = s->bz1 - oz;
    int32_t len = glm_isqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0) { dx = isin(cam_rot) >> 4; dy = 0; dz = icos(cam_rot) >> 4; len = 256; }

    int32_t ex = ox + (dx * GLM_LAS_MAX_LEN) / len;
    int32_t ey = oy + (dy * GLM_LAS_MAX_LEN) / len;
    int32_t ez = oz + (dz * GLM_LAS_MAX_LEN) / len;

    /* The floor: aimed down at a player's anchor, the line carries on into the
       ground behind them. -Y is up, so "below the floor" is y > floor_y. */
    int32_t floor_y = s->y + GROUND_FLOOR_Y;
    if (ey > floor_y && ey != oy) {
        int32_t num = floor_y - oy, den = ey - oy;
        ex = ox + ((ex - ox) * num) / den;
        ez = oz + ((ez - oz) * num) / den;
        ey = floor_y;
    }

    if (collision_segment_blocked(ox, oy, oz, ex, ey, ez)) {
        int32_t lo = 0, hi = 256, it;
        for (it = 0; it < 9; it++) {
            int32_t mid = (lo + hi) >> 1;
            if (collision_segment_blocked(ox, oy, oz,
                                          ox + ((ex - ox) * mid >> 8),
                                          oy + ((ey - oy) * mid >> 8),
                                          oz + ((ez - oz) * mid >> 8)))
                hi = mid;
            else
                lo = mid;
        }
        ex = ox + ((ex - ox) * hi >> 8);
        ey = oy + ((ey - oy) * hi >> 8);
        ez = oz + ((ez - oz) * hi >> 8);
    }
    s->bx1 = ex; s->by1 = ey; s->bz1 = ez;
}

/* Is the player's body on the beam? Their column - feet at player_y +
   GROUND_FLOOR_Y, head at the eye less a little - against the beam's line in
   plan, then the beam's height where it passes them. */
static int glm_beam_hits_player(const Golem *s) {
    int32_t px = player_x(), py = player_y(), pz = player_z();
    int32_t vx = s->bx1 - s->bx0, vz = s->bz1 - s->bz0;
    int32_t L  = glm_isqrt(vx * vx + vz * vz);
    if (L <= 0) return 0;
    int32_t wx = px - s->bx0, wz = pz - s->bz0;
    int32_t along = (wx * vx + wz * vz) / L;
    if (along < 0 || along > L) return 0;
    int32_t perp = glm_abs(wx * vz - wz * vx) / L;
    if (perp > GLM_BEAM_HIT_R) return 0;
    int32_t yb = s->by0 + ((s->by1 - s->by0) * along) / L;
    return yb >= py - 70 && yb <= py + GROUND_FLOOR_Y;
}

/* ---- Update ------------------------------------------------------------------ */
void update_golems(void) {
    int i;
    glm_update_shards();

    for (i = 0; i < golem_count; i++) {
        Golem *s = &golems[i];
        /* current_area, NEVER game_state (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!s->active || s->state == GLM_DEAD || s->area != current_area) continue;

        if (s->hit_timer > 0) s->hit_timer--;
        if (s->las_cool  > 0) s->las_cool--;
        s->anim_tick++;

        apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                          &s->on_upper_floor, &s->on_ramp);

        glm_shock_tick(s);
        s->timer++;

        switch (s->state) {
        case GLM_IDLE:
            break;

        case GLM_LAS_WAITING:
            glm_aim(s);
            if (s->timer >= GLM_LAS_WAIT) { s->state = GLM_LAS_CHARGING; s->timer = 0; }
            break;

        case GLM_LAS_CHARGING:
            /* Tracking until GLM_LAS_LOCK_LEAD before the beam, then the line
               is fixed - that half second is the player's chance to step off
               it (golem.h). */
            if (s->timer < GLM_LAS_CHARGE - GLM_LAS_LOCK_LEAD) {
                glm_aim(s);
            } else if (s->timer == GLM_LAS_CHARGE - GLM_LAS_LOCK_LEAD) {
                glm_aim(s);
                glm_lock_beam(s);
            }
            if (s->timer >= GLM_LAS_CHARGE) {
                s->state = GLM_LAS_FIRING;
                s->timer = 0;
                /* The cue is the beam leaving it, as Asag's is. */
                sound_play(SFX_LASER);
            }
            break;

        case GLM_LAS_FIRING:
            if (s->las_cool == 0 && !game_over && glm_beam_hits_player(s)) {
                s->las_cool = GLM_LAS_HIT_COOLDOWN;
                glm_hurt_player(GLM_LAS_DAMAGE);
            }
            /* Asag's clip (4.4 s) outlives the beam and plays out in full -
               never cut here (golem.h). */
            if (s->timer >= GLM_LAS_FIRE) glm_decide(s);
            break;

        case GLM_WINDUP:
            if (s->timer >= GLM_WINDUP_FRAMES) {
                s->state    = GLM_STRIKE;
                s->timer    = 0;
                s->wave_t   = 1;      /* the wave leaves on the first strike frame */
                s->wave_hit = 0;
                sound_play(SFX_RUMBLE);
            }
            break;

        case GLM_STRIKE:
            if (s->timer >= GLM_STRIKE_FRAMES) glm_decide(s);
            break;

        default:
            break;
        }
    }
}

/* ---- Drawing ----------------------------------------------------------------- */

static int glm_frame(const Golem *s) {
    switch (s->state) {
    case GLM_LAS_CHARGING: return GLM_F_GLOW;
    case GLM_LAS_FIRING:   return GLM_F_FIRE;
    case GLM_WINDUP:       return GLM_F_RAISE;
    case GLM_STRIKE:       return GLM_F_SLAM;
    default:
        /* IDLE and the laser's wait: the breath, frame 1 then frame 6. */
        return ((s->anim_tick / GLM_BREATH_RATE) & 1) ? GLM_F_IDLE_B : GLM_F_IDLE_A;
    }
}

/* The texture-window bracket: ahriman.c's. addPrim PREPENDS within a bucket,
   so the restore goes in first and the disable last. */
static void glm_bracket_begin(RenderContext *ctx, int32_t otz) {
    if (!glm_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
    setTexWindow(restore, &glm_tw_restore);
    addPrim(&ot[otz], restore);
    ctx->next_packet += sizeof(DR_TWIN);
}

static void glm_bracket_end(RenderContext *ctx, int32_t otz) {
    if (!glm_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    RECT full = { 0, 0, 0, 0 };
    DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
    setTexWindow(disable, &full);
    addPrim(&ot[otz], disable);
    ctx->next_packet += sizeof(DR_TWIN);
}

/* The body: draw_lmb_sprite's 2x2 grid, one OT bucket, one bracket. Returns the
   bucket (0 if nothing was drawn) for the beam and glow to sort against. */
static int32_t draw_glm_sprite(RenderContext *ctx, Golem *s, int frame) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);
    int     gi, gj;

    int32_t cx = s->x, cz = s->z, cy = glm_body_y(s);
    int16_t dwx = (int16_t)((GLM_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((GLM_HALF_W * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - GLM_HALF_H);
    int16_t vy_bot = (int16_t)(cy + GLM_HALF_H);

    /* The ring TL, TR, BR, BL, which the grid interpolates. */
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
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return 0;

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return 0;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    int32_t dist = glm_abs(s->x - cam_x) + glm_abs(s->z - cam_z);
    if (dist >= g_fog_far) return 0;
    int32_t fs   = render_fog_scale(dist);
    uint8_t fog8 = fs > 255 ? 255 : (uint8_t)fs;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    size_t   need    = (size_t)(GLM_SUBDIV * GLM_SUBDIV) * sizeof(POLY_FT4)
                     + (glm_tw_active ? 2 * sizeof(DR_TWIN) : 0);
    if (ctx->next_packet + need > buf_end) return 0;

    SVECTOR g[(GLM_SUBDIV + 1) * (GLM_SUBDIV + 1)];
    DVECTOR gs[(GLM_SUBDIV + 1) * (GLM_SUBDIV + 1)];
    for (gi = 0; gi <= GLM_SUBDIV; gi++) {
        int32_t tx = v[0].vx + ((int32_t)(v[1].vx - v[0].vx) * gi) / GLM_SUBDIV;
        int32_t ty = v[0].vy + ((int32_t)(v[1].vy - v[0].vy) * gi) / GLM_SUBDIV;
        int32_t tz = v[0].vz + ((int32_t)(v[1].vz - v[0].vz) * gi) / GLM_SUBDIV;
        int32_t bx = v[3].vx + ((int32_t)(v[2].vx - v[3].vx) * gi) / GLM_SUBDIV;
        int32_t by = v[3].vy + ((int32_t)(v[2].vy - v[3].vy) * gi) / GLM_SUBDIV;
        int32_t bz = v[3].vz + ((int32_t)(v[2].vz - v[3].vz) * gi) / GLM_SUBDIV;
        for (gj = 0; gj <= GLM_SUBDIV; gj++) {
            SVECTOR *p = &g[gj * (GLM_SUBDIV + 1) + gi];
            p->vx  = (int16_t)(tx + ((bx - tx) * gj) / GLM_SUBDIV);
            p->vy  = (int16_t)(ty + ((by - ty) * gj) / GLM_SUBDIV);
            p->vz  = (int16_t)(tz + ((bz - tz) * gj) / GLM_SUBDIV);
            p->pad = 0;
        }
    }
    {
        int n = (GLM_SUBDIV + 1) * (GLM_SUBDIV + 1), k;
        for (k = 0; k < n; k++) {
            gte_ldv0(&g[k]);
            gte_rtps();
            gte_stsxy(&gs[k]);
            if (gs[k].vx <= -1023 || gs[k].vx >= 1023 ||
                gs[k].vy <= -1023 || gs[k].vy >= 1023) return 0;
        }
    }

    /* UVs: the frame's column, split down the middle, inset one texel at the
       outer edges so a magnified quad cannot sample the next frame. V is the
       row within the 256-tall page: the sheets sit at y128, so 129..254. */
    int col = frame % 3;
    int u0  = GLM_COL_U0[col], u1 = GLM_COL_U1[col];
    int um  = (u0 + u1) / 2;
    uint8_t ulo[2] = { (uint8_t)(u0 + 1), (uint8_t)um };
    uint8_t uhi[2] = { (uint8_t)(um - 1), (uint8_t)(u1 - 1) };
    uint8_t vlo[2] = { 129, 192 };
    uint8_t vhi[2] = { 191, 254 };

    uint16_t tpage = glm_tpage[frame / 3];
    uint16_t clut  = glm_clut [frame / 3];

    glm_bracket_begin(ctx, otz);
    for (gj = 0; gj < GLM_SUBDIV; gj++) {
        for (gi = 0; gi < GLM_SUBDIV; gi++) {
            DVECTOR *tl = &gs[ gj      * (GLM_SUBDIV + 1) + gi    ];
            DVECTOR *tr = &gs[ gj      * (GLM_SUBDIV + 1) + gi + 1];
            DVECTOR *bl = &gs[(gj + 1) * (GLM_SUBDIV + 1) + gi    ];
            DVECTOR *br = &gs[(gj + 1) * (GLM_SUBDIV + 1) + gi + 1];

            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            if (s->hit_timer > GLM_BAR_TIMER_MAX - 8)   /* a brief red flash */
                setRGB0(poly, fog8, fog8 >> 2, fog8 >> 2);
            else
                setRGB0(poly, fog8, fog8, fog8);
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
    glm_bracket_end(ctx, otz);

    if (s->hit_timer > 0) {
        /* The health bar: the Lumberer's two TILEs, over its head. */
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
        int16_t fill_w = (int16_t)((s->health * 40) / (GLM_MAX_HEALTH * GLM_HP_SCALE));
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
    return otz;
}

/* A glow hung at a world point: asag_fight.c's af_glow_point, copied (it is
   static there) - concentric additive screen-space squares round the point's
   projection, sized by world distance. `min_otz` keeps it in front of the
   body it sits on: a chest glow sorted at its own depth would land in the
   same bucket as the billboard and could go under it. */
static void glm_glow_point(RenderContext *ctx, int32_t px, int32_t py, int32_t pz,
                           int32_t world_half, int rings,
                           uint8_t r, uint8_t g, uint8_t b, int32_t max_otz) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    SVECTOR pt;
    pt.vx = (int16_t)px; pt.vy = (int16_t)py; pt.vz = (int16_t)pz; pt.pad = 0;
    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    int32_t dx = px - cam_x, dy = py - cam_y, dz = pz - cam_z;
    int32_t dist = glm_isqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 64) dist = 64;

    int32_t otz = sz >> 2;
    if (max_otz > 0 && otz >= max_otz) otz = max_otz - 1;
    if (otz <= SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    int ring;
    for (ring = 0; ring < rings; ring++) {
        if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;
        int32_t wh   = world_half >> ring;
        int32_t half = (wh * 256) / dist;
        if (half < 1)   half = 1;
        if (half > 400) half = 400;
        int32_t lev = 70 + ring * 62;
        if (lev > 256) lev = 256;

        POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setSemiTrans(p, 1);
        setRGB0(p, (uint8_t)((r * lev) >> 8),
                   (uint8_t)((g * lev) >> 8),
                   (uint8_t)((b * lev) >> 8));
        p->x0 = (int16_t)(sv.vx - half); p->y0 = (int16_t)(sv.vy - half);
        p->x1 = (int16_t)(sv.vx + half); p->y1 = (int16_t)(sv.vy - half);
        p->x2 = (int16_t)(sv.vx - half); p->y2 = (int16_t)(sv.vy + half);
        p->x3 = (int16_t)(sv.vx + half); p->y3 = (int16_t)(sv.vy + half);
        addPrim(&ot[otz], p);
        ctx->next_packet += sizeof(POLY_F4);

        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

/* The charge glow (frame 2) and the beam (frame 3): Asag's af_draw_charge and
   af_draw_laser, in his colours. His beam's two crossed quads spread along X
   and Z, which suits a beam that falls steeply from a ceiling-high head; this
   one runs nearly level, so it is crossed about ITS OWN axis instead - one
   quad spread across it in plan, one spread vertically - or a beam pointing
   along X would put the first quad edge-on and leave half of it missing. */
static void draw_glm_laser(RenderContext *ctx, const Golem *s, int32_t body_otz) {
    int32_t lev;
    if (s->state == GLM_LAS_CHARGING)
        lev = (s->timer * 256) / GLM_LAS_CHARGE;
    else if (s->state == GLM_LAS_FIRING)
        lev = 256;
    else
        return;

    /* The glow sits on the core of the frame on screen: frame 2's while it
       charges, frame 3's (the beam's own origin) once it fires. */
    {
        int32_t gx = s->bx0, gy = s->by0, gz = s->bz0;
        if (s->state == GLM_LAS_CHARGING)
            glm_core(s, GLM_CHARGE_CORE_X, GLM_CHARGE_CORE_DY, &gx, &gy, &gz);
        glm_glow_point(ctx, gx, gy, gz, (110 * lev) / 256, 3,
                       255, (uint8_t)(150 + (lev * 60) / 256), 70, body_otz);
    }

    if (s->state != GLM_LAS_FIRING) return;

    int32_t dx = s->bx1 - s->bx0, dy = s->by1 - s->by0, dz = s->bz1 - s->bz0;
    int32_t L  = glm_isqrt(dx * dx + dz * dz);
    int32_t nx = 0, nz = 4096;
    if (L > 0) { nx = (-dz * 4096) / L; nz = (dx * 4096) / L; }

    /* THE SEGMENTS (golem.h: why). n pieces of about GLM_BEAM_SEG; each end
       is given its depth along the view, and a piece is cut back to
       GLM_BEAM_NEAR in front of the eye where it passes behind it - so no
       corner ever projects from behind the camera, and no piece is long
       enough on screen for the GPU's 1023-pixel limit. Parameters are in
       4096ths of the beam. */
    int32_t Lt = glm_isqrt(dx * dx + dy * dy + dz * dz);
    int32_t n  = Lt / GLM_BEAM_SEG + 1;
    if (n > 48) n = 48;
    int32_t sr = isin(cam_rot), cr = icos(cam_rot);
    int32_t sp = isin(cam_pitch), cp = icos(cam_pitch);

    SVECTOR v[4];
    int k, seg;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    for (seg = 0; seg < n; seg++) {
        int32_t ta = (seg * 4096) / n, tb = ((seg + 1) * 4096) / n;
        int32_t t[2] = { ta, tb }, d[2];
        for (k = 0; k < 2; k++) {
            int32_t px = (dx * t[k]) >> 12, py = (dy * t[k]) >> 12, pz = (dz * t[k]) >> 12;
            int32_t ex = s->bx0 + px - cam_x, ey = s->by0 + py - cam_y, ez = s->bz0 + pz - cam_z;
            int32_t f  = (ex * sr + ez * cr) >> 12;
            d[k] = (f * cp + ey * sp) >> 12;
        }
        if (d[0] < GLM_BEAM_NEAR && d[1] < GLM_BEAM_NEAR) continue;
        if (d[0] < GLM_BEAM_NEAR)
            ta = ta + ((tb - ta) * (GLM_BEAM_NEAR - d[0])) / (d[1] - d[0]);
        else if (d[1] < GLM_BEAM_NEAR)
            tb = ta + ((tb - ta) * (d[0] - GLM_BEAM_NEAR)) / (d[0] - d[1]);

        int32_t ax = s->bx0 + ((dx * ta) >> 12), ay = s->by0 + ((dy * ta) >> 12),
                az = s->bz0 + ((dz * ta) >> 12);
        int32_t bx = s->bx0 + ((dx * tb) >> 12), by = s->by0 + ((dy * tb) >> 12),
                bz = s->bz0 + ((dz * tb) >> 12);
        int32_t ha = GLM_BEAM_HALF_NEAR + (((GLM_BEAM_HALF_FAR - GLM_BEAM_HALF_NEAR) * ta) >> 12);
        int32_t hb = GLM_BEAM_HALF_NEAR + (((GLM_BEAM_HALF_FAR - GLM_BEAM_HALF_NEAR) * tb) >> 12);

        /* Quad 1: spread across the beam in plan. */
        {
            int32_t hax = (nx * ha) >> 12, haz = (nz * ha) >> 12;
            int32_t hbx = (nx * hb) >> 12, hbz = (nz * hb) >> 12;
            v[0].vx = (int16_t)(ax - hax); v[0].vy = (int16_t)ay; v[0].vz = (int16_t)(az - haz);
            v[1].vx = (int16_t)(ax + hax); v[1].vy = (int16_t)ay; v[1].vz = (int16_t)(az + haz);
            v[2].vx = (int16_t)(bx - hbx); v[2].vy = (int16_t)by; v[2].vz = (int16_t)(bz - hbz);
            v[3].vx = (int16_t)(bx + hbx); v[3].vy = (int16_t)by; v[3].vz = (int16_t)(bz + hbz);
            rbs_glow_quad(ctx, v, 255, 190, 80);
        }
        /* Quad 2: spread vertically. */
        v[0].vx = (int16_t)ax; v[0].vy = (int16_t)(ay - ha); v[0].vz = (int16_t)az;
        v[1].vx = (int16_t)ax; v[1].vy = (int16_t)(ay + ha); v[1].vz = (int16_t)az;
        v[2].vx = (int16_t)bx; v[2].vy = (int16_t)(by - hb); v[2].vz = (int16_t)bz;
        v[3].vx = (int16_t)bx; v[3].vy = (int16_t)(by + hb); v[3].vz = (int16_t)bz;
        rbs_glow_quad(ctx, v, 255, 190, 80);
    }

    /* Where it lands - the part the player reads as "that is where it hits". */
    glm_glow_point(ctx, s->bx1, s->by1, s->bz1, 120, 3, 255, 215, 120, 0);
}

/* lmb_wave_quad: one flat additive quad of the shockwave. */
static void glm_wave_quad(RenderContext *ctx, const SVECTOR v[4],
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

/* The wave: draw_lmb_shockwave's ring and skirt, DARK RED as briefed. Additive
   can only add light, so "dark red" is a low red with almost no green or blue:
   it reads as a deep red glow over the rock rather than the Lumberer's dust. */
static void draw_glm_shockwave(RenderContext *ctx, const Golem *s) {
    int32_t reach = glm_wave_reach(s);
    int32_t bright = 256;
    if (s->wave_t > GLM_SHOCK_EXPAND) {
        int32_t k = s->wave_t - GLM_SHOCK_EXPAND;
        bright = 256 - (k * 256) / GLM_SHOCK_LINGER;
        if (bright <= 0) return;
    }
    int32_t inner = reach - GLM_SHOCK_BAND;
    if (inner < 0) inner = 0;

    uint8_t gr = (uint8_t)((130 * bright) >> 8);
    uint8_t gg = (uint8_t)(( 10 * bright) >> 8);
    uint8_t gb = (uint8_t)((  6 * bright) >> 8);
    uint8_t wr = (uint8_t)((170 * bright) >> 8);
    uint8_t wg = (uint8_t)(( 16 * bright) >> 8);
    uint8_t wb = (uint8_t)((  8 * bright) >> 8);

    /* The floor under its feet, lifted 4 clear of the floor poly. */
    int32_t fy = s->y + GLM_Y_OFFSET + GLM_HALF_H - 4;

    SVECTOR v[4];
    int k, seg;
    for (k = 0; k < 4; k++) v[k].pad = 0;
    for (seg = 0; seg < GLM_SHOCK_SEGS; seg++) {
        int32_t a0 = (seg * 4096) / GLM_SHOCK_SEGS;
        int32_t a1 = ((seg + 1) * 4096) / GLM_SHOCK_SEGS;
        int32_t s0 = isin(a0), c0 = icos(a0);
        int32_t s1 = isin(a1), c1 = icos(a1);
        int32_t ix0 = s->x + (s0 * inner >> 12), iz0 = s->z + (c0 * inner >> 12);
        int32_t ix1 = s->x + (s1 * inner >> 12), iz1 = s->z + (c1 * inner >> 12);
        int32_t ox0 = s->x + (s0 * reach >> 12), oz0 = s->z + (c0 * reach >> 12);
        int32_t ox1 = s->x + (s1 * reach >> 12), oz1 = s->z + (c1 * reach >> 12);

        v[0].vx = (int16_t)ix0; v[0].vy = (int16_t)fy; v[0].vz = (int16_t)iz0;
        v[1].vx = (int16_t)ox0; v[1].vy = (int16_t)fy; v[1].vz = (int16_t)oz0;
        v[2].vx = (int16_t)ix1; v[2].vy = (int16_t)fy; v[2].vz = (int16_t)iz1;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)fy; v[3].vz = (int16_t)oz1;
        glm_wave_quad(ctx, v, gr, gg, gb);

        v[0].vx = (int16_t)ox0; v[0].vy = (int16_t)(fy - GLM_SHOCK_WALL_H); v[0].vz = (int16_t)oz0;
        v[1].vx = (int16_t)ox1; v[1].vy = (int16_t)(fy - GLM_SHOCK_WALL_H); v[1].vz = (int16_t)oz1;
        v[2].vx = (int16_t)ox0; v[2].vy = (int16_t)fy;                      v[2].vz = (int16_t)oz0;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)fy;                      v[3].vz = (int16_t)oz1;
        glm_wave_quad(ctx, v, wr, wg, wb);
    }
}

/* One shard: sh_draw_shard's cube, rotated on the CPU with isin/icos (yaw then
   pitch) instead of RotMatrix, so nothing here ever touches the GTE's matrix -
   the room's view stays loaded for whatever draws next. No backface cull: a
   convex body painted face by face at each face's own depth comes out right. */
static void draw_glm_shard(RenderContext *ctx, const GlmShard *h) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    SVECTOR  wv[8];
    DVECTOR  scr[8];
    int32_t  sz[8], hh, dist, fog, k, f;

    hh = h->half;
    if (h->life < GLM_SHARD_SHRINK) hh = (hh * h->life) / GLM_SHARD_SHRINK;
    if (hh <= 0) return;

    dist = glm_abs(h->x - cam_x) + glm_abs(h->z - cam_z);
    if (dist > g_fog_far) return;
    fog = render_fog_scale(dist);
    if (fog > 256) fog = 256;

    int32_t sy = isin(h->ry), cyw = icos(h->ry);
    int32_t sp = isin(h->rx), cp = icos(h->rx);
    for (k = 0; k < 8; k++) {
        int32_t lx = (k & 1) ? hh : -hh;
        int32_t ly = (k & 2) ? hh : -hh;
        int32_t lz = (k & 4) ? hh : -hh;
        /* pitch about X, then yaw about Y */
        int32_t y1 = (ly * cp - lz * sp) >> 12;
        int32_t z1 = (ly * sp + lz * cp) >> 12;
        int32_t x2 = (lx * cyw + z1 * sy) >> 12;
        int32_t z2 = (z1 * cyw - lx * sy) >> 12;
        wv[k].vx = (int16_t)(h->x + x2);
        wv[k].vy = (int16_t)(h->y + y1);
        wv[k].vz = (int16_t)(h->z + z2);
        wv[k].pad = 0;
    }
    for (k = 0; k < 8; k++) {
        gte_ldv0(&wv[k]);
        gte_rtps();
        gte_stsxy(&scr[k]);
        gte_stsz(&sz[k]);
        if (sz[k] <= 0) return;
        if (scr[k].vx <= -1023 || scr[k].vx >= 1023 ||
            scr[k].vy <= -1023 || scr[k].vy >= 1023) return;
    }

    const uint8_t *rgb = GLM_SHARD_RGB[h->col];
    for (f = 0; f < 6; f++) {
        int axis = f >> 1;
        int bit  = 1 << axis;
        int ba   = 1 << ((axis + 1) % 3);
        int bb   = 1 << ((axis + 2) % 3);
        int c0   = (f & 1) ? bit : 0;
        int c1 = c0 | ba, c2 = c0 | bb, c3 = c0 | ba | bb;
        int32_t otz = (sz[c0] + sz[c1] + sz[c2] + sz[c3]) >> 4;
        int32_t sh  = GLM_FACE_SHADE[f];
        POLY_F4 *poly;
        if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
        if (otz >= UW_SKY_OT)   otz = UW_SKY_OT - 1;
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

        int32_t lr = (rgb[0] * sh) >> 8, lg = (rgb[1] * sh) >> 8, lb = (rgb[2] * sh) >> 8;
        uint8_t r = (uint8_t)((lr * fog + UW_FOG_R * (256 - fog)) >> 8);
        uint8_t g = (uint8_t)((lg * fog + UW_FOG_G * (256 - fog)) >> 8);
        uint8_t b = (uint8_t)((lb * fog + UW_FOG_B * (256 - fog)) >> 8);

        poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, r, g, b);
        poly->x0 = scr[c0].vx; poly->y0 = scr[c0].vy;
        poly->x1 = scr[c1].vx; poly->y1 = scr[c1].vy;
        poly->x2 = scr[c2].vx; poly->y2 = scr[c2].vy;
        poly->x3 = scr[c3].vx; poly->y3 = scr[c3].vy;
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

void draw_golems(RenderContext *ctx) {
    int i;
    for (i = 0; i < GLM_SHARDS; i++)
        if (glm_shards[i].life > 0 && glm_shards[i].area == current_area)
            draw_glm_shard(ctx, &glm_shards[i]);

    if (!tex_loaded) return;
    for (i = 0; i < golem_count; i++) {
        Golem *s = &golems[i];
        if (!s->active || s->state == GLM_DEAD || s->area != current_area) continue;

        int32_t wdist = glm_abs(s->x - cam_x) + glm_abs(s->z - cam_z);
        /* The beam can reach well past the body, so it is drawn even when the
           body itself is fogged out. */
        int32_t otz = 0;
        if (wdist < g_fog_far) otz = draw_glm_sprite(ctx, s, glm_frame(s));
        if (s->wave_t > 0) draw_glm_shockwave(ctx, s);
        draw_glm_laser(ctx, s, otz);
    }
}
