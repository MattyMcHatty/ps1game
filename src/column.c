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
#include "crucifaxe.h"   /* SWING_RANGE, melee_damage                        */
#include "nessie.h"      /* NSS_FB_SPEED: the orb flies at half of it        */
#include "underworld.h"  /* UW_FOG_*: the body and shards fog to the red     */
#include "underworld_sky.h" /* UW_SKY_OT: nothing may sort behind the sky    */
#include "column.h"

/* The Column - see column.h for the design, the numbers and why. */

_Static_assert(COL_ORB_SPEED * 2 == NSS_FB_SPEED,
               "the orb is briefed at half a Nessie fireball's speed");

Column columns[MAX_COLUMNS];
int    column_count = 0;

static uint16_t col_tpage  = 0;
static uint16_t col_clut   = 0;
static int      col_tex_id = -1;
static int      tex_loaded = 0;

/* Texture window the current area expects, restored after each quad (the
   sheet is at Voff 128). The Golem's bracket. */
static RECT col_tw_restore;
static int  col_tw_active = 0;

void columns_set_texwindow(const RECT *tw) {
    if (tw) { col_tw_restore = *tw; col_tw_active = 1; }
    else    { col_tw_active = 0; }
}

void columns_load_textures(void) {
    /* BANK: Chapter 4 only. py tools/check_tex_banks.py checks it. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    col_tex_id = texmgr_register("\\TEXUNDW\\COLUMN.TIM;1");
    col_tpage  = texmgr_tpage(col_tex_id);
    col_clut   = texmgr_clut (col_tex_id);
    tex_loaded = (col_tex_id >= 0);
}

/* Nothing to mark taken: the intro still re-reads its own page and the
   Catacombs re-upload lumberer_a on every entry (column.h, TEXTURE SLOT). */
void columns_upload_textures(void) {
    texmgr_upload(col_tex_id);
}

/* The rooms whose entry uploads the sheet. main.c keys the upload on this, and
   tools/gen_tex_rooms.py mirrors it as COLUMN_ROOMS - keep the two in step. */
int column_room_has_sheet(GameState area) {
    return area == STATE_LAVA_MAZE;
}

/* ---- The pieces --------------------------------------------------------------
   One entry per drawn square: its centre (along the plane from where the
   column stands, and height above the floor) and the texels it shows. U/V are
   page coordinates, inclusive; the sheet is at y128, so V = 128 + texel row.

   THE ARCH, for a step in the + direction (a - step negates `along` and
   mirrors every piece's U). The bottom half is a GRID of six equal cells
   (the user re-laid the art for exactly this, Nov 2026), 3 across x 2 down,
   each ~42.7 x 32 texels - cut at u 0 / 43 / 85 / 128, as the Golem's sheet
   is cut at thirds:
       rows 64-95    left drum | ORB   | right drum
       rows 96-127   left base | blank | right base
   Measured: every piece sits inside its cell except the left base's foot
   flare, whose last two texel columns (u43-44, 12 texels in all) spill into
   the blank cell and are cut off - about 5 units at the flare's tip. */
typedef struct {
    int16_t along, h;
    uint8_t u0, v0, u1, v1;
} ColPiece;

static const ColPiece COL_ARCH_SQ[COL_ARCH_PIECES] = {
    {   0,  50,   0, 128 +  96,  42, 128 + 127 },   /* foot: the left base   */
    {  40, 145,   0, 128 +  64,  42, 128 +  95 },   /* the left drum         */
    { COL_ORB_ARCH_ALONG, COL_ORB_ARCH_H,
                 43, 128 +  64,  84, 128 +  95 },   /* the ORB               */
    { 260, 145,  85, 128 +  64, 127, 128 +  95 },   /* the right drum        */
    { COL_STEP, 50, 85, 128 +  96, 127, 128 + 127 },/* head: the right base  */
};

/* THE IDLE COLUMN: the top half's 64 rows cut into five equal bands, the top
   band highest. The orb is painted in rows 27-39 - the middle band. */
static const ColPiece COL_IDLE_SQ[COL_STACK] = {
    { 0, 360, 0, 128 +  0, 127, 128 + 12 },   /* 80-tall pieces (column.h) */
    { 0, 280, 0, 128 + 13, 127, 128 + 25 },
    { 0, COL_ORB_IDLE_H,
              0, 128 + 26, 127, 128 + 37 },   /* the ORB */
    { 0, 120, 0, 128 + 38, 127, 128 + 50 },
    { 0,  40, 0, 128 + 51, 127, 128 + 63 },
};

/* Integer square root, golem.c's - including the `t > 0` seed (mistake 11 in
   tools/ADDING_AN_ENEMY.txt). */
static int32_t col_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

static int32_t col_abs(int32_t v) { return v < 0 ? -v : v; }

/* The plane's direction: (1,0) along X, (0,1) along Z. */
static int32_t col_ax(const Column *s) { return s->axis == COL_AXIS_X ? 1 : 0; }
static int32_t col_az(const Column *s) { return s->axis == COL_AXIS_Z ? 1 : 0; }

static int32_t col_floor_y(const Column *s) { return s->y + GROUND_FLOOR_Y; }

/* The current pose's extent: along the plane [lo, hi] from where it stands,
   and its height above the floor. */
static void col_extent(const Column *s, int32_t *lo, int32_t *hi, int32_t *top) {
    if (s->state == COL_ARCH) {
        if (s->dir > 0) { *lo = -COL_HALF; *hi = COL_STEP + COL_HALF; }
        else            { *lo = -(COL_STEP + COL_HALF); *hi = COL_HALF; }
        *top = COL_ARCH_H;
    } else {
        *lo = -COL_IDLE_HALF_W; *hi = COL_IDLE_HALF_W;
        *top = COL_IDLE_H;
    }
}

/* Where the orb square's centre is in the current pose. */
static void col_orb_point(const Column *s, int32_t *x, int32_t *y, int32_t *z) {
    int32_t along = 0, h = COL_ORB_IDLE_H;
    if (s->state == COL_ARCH) { along = s->dir * COL_ORB_ARCH_ALONG; h = COL_ORB_ARCH_H; }
    *x = s->x + along * col_ax(s);
    *z = s->z + along * col_az(s);
    *y = col_floor_y(s) - h;
}

/* Distance in plan from (px,pz) to the segment [lo, hi] of s's plane. */
static int32_t col_seg_dist(const Column *s, int32_t lo, int32_t hi,
                            int32_t px, int32_t pz) {
    int32_t dx = px - s->x, dz = pz - s->z;
    int32_t along = s->axis == COL_AXIS_X ? dx : dz;
    int32_t perp  = s->axis == COL_AXIS_X ? dz : dx;
    int32_t c = along < lo ? lo : (along > hi ? hi : along);
    int32_t da = along - c;
    return col_isqrt(da * da + perp * perp);
}

/* ---- The shards: golem.c's burst (Sharur's Tablet's cubes), in greys and
   purples, spread along the column's own PLANE rather than the camera's
   right vector - it is not a billboard. One pool for every Column, area-
   tagged, cleared by columns_rest. */
#define COL_SHARDS        12
#define COL_SHARD_SHRINK  20

static const uint8_t COL_SHARD_RGB[8][3] = {
    { 120, 118, 114 }, { 160, 156, 150 }, {  88,  86,  84 }, { 190, 184, 176 },
    {  92,  30, 112 }, { 128,  52, 156 }, {  66,  20,  86 }, { 156,  84, 186 },
};
static const uint16_t COL_FACE_SHADE[6] = { 150, 200, 256, 110, 175, 225 };

typedef struct {
    int32_t x, y, z, vx, vy, vz, floor_y;
    int16_t rx, ry, sx, sy;
    int16_t half, life, max_life;
    uint8_t col;
    GameState area;
} ColShard;

static ColShard col_shards[COL_SHARDS];

static uint32_t col_rng = 0x2F91Bu;
static int32_t col_rand(int32_t n) {      /* 0..n-1 */
    col_rng = col_rng * 1103515245u + 12345u;
    return n > 0 ? (int32_t)((col_rng >> 16) % (uint32_t)n) : 0;
}

static void col_shatter(const Column *s) {
    int32_t lo, hi, top, i;
    int32_t ax = col_ax(s), az = col_az(s);
    int32_t floor_y = col_floor_y(s);
    col_extent(s, &lo, &hi, &top);
    for (i = 0; i < COL_SHARDS; i++) {
        ColShard *h = &col_shards[i];
        /* Evenly along the pose, jittered; heights over its whole height. */
        int32_t a = lo + ((hi - lo) * (2 * i + 1)) / (2 * COL_SHARDS)
                    + col_rand(41) - 20;
        h->x = s->x + a * ax;
        h->z = s->z + a * az;
        h->y = floor_y - 20 - col_rand(top - 20);
        {
            /* A little along the plane, a little out of it either side -
               small, so the burst keeps to the body. */
            int32_t along = col_rand(7) - 3;
            int32_t out   = col_rand(9) - 4;
            h->vx = along * ax + out * az;
            h->vz = along * az + out * ax;
        }
        h->vy  = -(6 + col_rand(10));
        h->rx  = (int16_t)col_rand(4096);
        h->ry  = (int16_t)col_rand(4096);
        h->sx  = (int16_t)(col_rand(81) - 40);
        h->sy  = (int16_t)(col_rand(81) - 40);
        h->half     = (int16_t)(20 + col_rand(12));
        h->life     = (int16_t)(70 + col_rand(31));
        h->max_life = h->life;
        h->col      = (uint8_t)col_rand(8);
        h->floor_y  = floor_y;
        h->area     = s->area;
    }
}

static void col_update_shards(void) {
    int i;
    for (i = 0; i < COL_SHARDS; i++) {
        ColShard *h = &col_shards[i];
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

static void col_clear_shards(void) {
    int i;
    for (i = 0; i < COL_SHARDS; i++) col_shards[i].life = 0;
}

/* ---- The orbs: nessie.c's fireballs at half the speed, no parry. Transient,
   area-tagged, not in the save.

   >>> THE FLIGHT IS IN 1/256ths OF A UNIT (COL_ORB_FRAC). <<< At 16 a frame
   the Nessie's whole-unit velocity loses up to a unit per axis to truncation:
   aimed from 4,700 away at (-15.6, 0.25, -3.6) it flew (-15, 0, -3) and passed
   ~125 off a player who had not moved - outside the 110 contact sphere, so a
   still target was MISSED (measured in a headless run, Nov 2026). The
   fraction keeps the aim true to well under a unit over any room. --------- */
#define COL_ORB_FRAC 8
typedef struct {
    int32_t   x, y, z;     /* whole units, for every test and the draw     */
    int32_t   qx, qy, qz;  /* the same, in 1/256ths                        */
    int32_t   vx, vy, vz;  /* 1/256ths a frame                             */
    int32_t   life;        /* frames left before it fizzles; 0 = free slot */
    GameState area;
} ColOrb;

static ColOrb col_orbs[MAX_COL_ORBS];

static void col_clear_orbs(void) {
    int i;
    for (i = 0; i < MAX_COL_ORBS; i++) col_orbs[i].life = 0;
}

static void col_hurt_player(int32_t amount) {
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

/* Aimed ONCE, from the orb square at the player's dropped aim point, and only
   along a clear line - it never fires into the wall it stands behind. */
static void col_fire(const Column *s) {
    int i;
    int32_t ox, oy, oz;
    for (i = 0; i < MAX_COL_ORBS; i++)
        if (col_orbs[i].life <= 0) break;
    if (i >= MAX_COL_ORBS) return;

    col_orb_point(s, &ox, &oy, &oz);
    int32_t tx = player_x(), ty = player_y() + COL_ORB_AIM_DROP, tz = player_z();
    if (collision_segment_blocked(ox, oy, oz, tx, ty, tz)) return;

    int32_t dx = tx - ox, dy = ty - oy, dz = tz - oz;
    int32_t len = col_isqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0) return;

    ColOrb *f = &col_orbs[i];
    f->x = ox; f->y = oy; f->z = oz;
    f->qx = ox << COL_ORB_FRAC; f->qy = oy << COL_ORB_FRAC; f->qz = oz << COL_ORB_FRAC;
    /* |d| < 8,000 in any room here, so d * 16 * 256 stays under 2^25. */
    f->vx = (dx * (COL_ORB_SPEED << COL_ORB_FRAC)) / len;
    f->vy = (dy * (COL_ORB_SPEED << COL_ORB_FRAC)) / len;
    f->vz = (dz * (COL_ORB_SPEED << COL_ORB_FRAC)) / len;
    /* Outlives the nominal arrival, so a DODGED orb sails on past and only
       dissipates at the next wall (or, in the open, a while later). */
    f->life = (len / COL_ORB_SPEED) * 2 + 60;
    f->area = s->area;
}

static void col_update_orbs(void) {
    int i;
    for (i = 0; i < MAX_COL_ORBS; i++) {
        ColOrb *f = &col_orbs[i];
        if (f->life <= 0) continue;
        if (f->area != current_area) { f->life = 0; continue; }
        if (--f->life <= 0) continue;

        int32_t ox = f->x, oy = f->y, oz = f->z;
        f->qx += f->vx;
        f->qy += f->vy;
        f->qz += f->vz;
        f->x = f->qx >> COL_ORB_FRAC;
        f->y = f->qy >> COL_ORB_FRAC;
        f->z = f->qz >> COL_ORB_FRAC;

        /* The SEGMENT, not the new point (web.c's rule). */
        if (collision_segment_blocked(ox, oy, oz, f->x, f->y, f->z)) {
            f->life = 0;
            continue;
        }
        if (!game_over) {
            int32_t hx = player_x() - f->x;
            int32_t hy = (player_y() + COL_ORB_AIM_DROP) - f->y;
            int32_t hz = player_z() - f->z;
            if (hx * hx + hy * hy + hz * hz <=
                (int32_t)COL_ORB_HIT_RADIUS * COL_ORB_HIT_RADIUS) {
                f->life = 0;
                col_hurt_player(COL_ORB_DAMAGE);
            }
        }
    }
}

/* ---- Spawning ---------------------------------------------------------------- */

static void col_spawn(Column *s, int32_t x, int32_t y, int32_t z) {
    uint8_t axis = s->axis, alive = s->alive;
    GameState area = s->area;
    *s = (Column){0};
    s->axis  = axis;
    s->alive = alive;
    s->area  = area;
    s->x = x; s->y = y; s->z = z;
    s->spawn_x = x; s->spawn_z = z; s->spawn_y = y;
    s->health  = COL_MAX_HEALTH;
    s->state   = COL_IDLE;
    s->dir     = 1;
    s->active  = 1;
}

static int col_alive_count(void) {
    int i, n = 0;
    for (i = 0; i < column_count; i++) if (columns[i].alive) n++;
    return n;
}

int column_add(int32_t x, int32_t z, ColumnAxis axis, int alive, GameState area) {
    if (column_count >= MAX_COLUMNS) return -1;
    if (alive && col_alive_count() >= MAX_ALIVE_COLUMNS) return -1;
    int i = column_count++;
    Column *s = &columns[i];
    s->axis  = (uint8_t)axis;
    s->alive = (uint8_t)(alive ? 1 : 0);
    s->area  = area;
    /* The standing anchor of a y=0 floor (golem_add's reasoning): authored,
       and apply_ddog_height corrects it on the first frame. */
    col_spawn(s, x, -GROUND_FLOOR_Y, z);
    /* Out of step with each other, so two in a room do not move as one. */
    s->timer = (int16_t)((i * 37) % COL_IDLE_FRAMES);
    return i;
}

void columns_init(void) {
    column_count = 0;
    col_clear_shards();
    col_clear_orbs();
}

void columns_reset(void) {
    column_count = 0;
    col_clear_shards();
    col_clear_orbs();
}

void columns_rest(void) {
    int i;
    for (i = 0; i < column_count; i++) {
        Column *s = &columns[i];
        if (!s->active || s->state == COL_DEAD) continue;
        col_spawn(s, s->spawn_x, s->spawn_y, s->spawn_z);
        s->timer = (int16_t)((i * 37) % COL_IDLE_FRAMES);
    }
    col_clear_shards();
    col_clear_orbs();
}

/* ---- Damage ------------------------------------------------------------------ */
static const Weakness column_weakness[] = {
    { DMG_KINETIC, 100 },   /* 100% = no change: 1 from anything */
};

int32_t column_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, column_weakness,
                        WEAKNESS_COUNT(column_weakness));
}

void column_damage(Column *s, int dmg) {
    if (!s->active || !s->alive || s->state == COL_DEAD || dmg <= 0) return;
    s->health   -= (int16_t)dmg;
    s->hit_timer = COL_BAR_TIMER_MAX;
    if (s->health <= 0) {
        s->health = 0;
        col_shatter(s);              /* from the pose it died in */
        s->state  = COL_DEAD;
        sound_play(SFX_RUMBLE);      /* the Golem's death */
        return;
    }
    sound_play(SFX_AXEHIT);
    if (!s->enraged) {
        s->enraged    = 1;
        s->fire_timer = COL_RAGE_FIRST;
    }
}

int column_orb_box(const Column *s, int32_t *ox, int32_t *oy, int32_t *oz,
                   int32_t *hw, int32_t *hh) {
    if (!s->active || !s->alive || s->state == COL_DEAD ||
        s->area != current_area) return 0;
    col_orb_point(s, ox, oy, oz);
    *hw = s->state == COL_ARCH ? COL_HALF : COL_IDLE_HALF_W;
    *hh = s->state == COL_ARCH ? COL_HALF : COL_IDLE_HALF_H;
    return 1;
}

/* The crucifaxe: measured to the orb square's surface plus the vertical gap,
   with the facing dot - golems_try_hit's shape on a 1 m target. */
int columns_try_hit(void) {
    int i;
    for (i = 0; i < column_count; i++) {
        Column *s = &columns[i];
        int32_t ox, oy, oz, hw, hh;
        if (!column_orb_box(s, &ox, &oy, &oz, &hw, &hh)) continue;
        int32_t dx = ox - cam_x;
        int32_t dy = oy - cam_y;
        int32_t dz = oz - cam_z;
        int32_t d  = col_isqrt(dx * dx + dz * dz);
        int32_t gh = d > hw ? d - hw : 0;
        int32_t gv = col_abs(dy) > hh ? col_abs(dy) - hh : 0;
        if (gh + gv >= SWING_RANGE) continue;
        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;
        column_damage(s, melee_damage);
        return 1;
    }
    return 0;
}

/* THE BODY: a wall the length of the pose, two-sided, pushed out along the
   shortest line to it - capped a frame so a landing arch nudges the player
   rather than throwing them. Static and alive alike. */
void columns_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < column_count; i++) {
        Column *s = &columns[i];
        if (!s->active || s->state == COL_DEAD || s->area != current_area) continue;

        int32_t lo, hi, top;
        col_extent(s, &lo, &hi, &top);
        int32_t bot = col_floor_y(s);
        if (py + GROUND_FLOOR_Y < bot - top || py - 30 > bot) continue;

        int32_t dx = *px - s->x, dz = *pz - s->z;
        int32_t along = s->axis == COL_AXIS_X ? dx : dz;
        int32_t perp  = s->axis == COL_AXIS_X ? dz : dx;
        int32_t c  = along < lo ? lo : (along > hi ? hi : along);
        int32_t da = along - c;
        int32_t d  = col_isqrt(da * da + perp * perp);
        if (d >= radius) continue;
        int32_t step = radius - d;
        if (step > COL_PUSH_STEP) step = COL_PUSH_STEP;
        int32_t pa, pp;
        if (d == 0) { pa = 0; pp = perp >= 0 ? step : -step; }
        else        { pa = (da * step) / d; pp = (perp * step) / d; }
        if (s->axis == COL_AXIS_X) { *px += pa; *pz += pp; }
        else                       { *pz += pa; *px += pp; }
    }
}

/* ---- Hidden? -----------------------------------------------------------------
   The brief's test, exactly: OFF CAMERA or PAST THE FOG. ahriman.c's camera-
   space projection (forward (isin, icos), right (icos, -isin), then
   cam_pitch, then gte_SetGeomScreen(256) against 320x240), applied to the four
   corners of the extent rather than one centre, because a 5 m column is
   visible long before its middle is. It is OFF CAMERA only when every corner
   lies outside the SAME side of the view (the usual conservative frustum
   test - a body straddling the screen has corners on both sides and counts
   as seen), with ~10% margin past the screen edge. PAST THE FOG when even the
   nearest corner is at or beyond g_fog_far, measured the way the draw culls
   (Manhattan in plan). */
#define COL_VIEW_HW   176   /* (SCREEN_XRES / 2) + 10% */
#define COL_VIEW_HH   136   /* (SCREEN_YRES / 2) + 10% */

static int col_span_hidden(const Column *s, int32_t lo, int32_t hi, int32_t top) {
    int32_t sr = isin(cam_rot), cr = icos(cam_rot);
    int32_t sp = isin(cam_pitch), cp = icos(cam_pitch);
    int32_t ax = col_ax(s), az = col_az(s), floor_y = col_floor_y(s);
    int32_t near_fog = 0x7fffffff;
    int     out_all = 0x1f, k;
    for (k = 0; k < 4; k++) {
        int32_t along = (k & 1) ? hi : lo;
        int32_t wy    = (k & 2) ? floor_y - top : floor_y;
        int32_t dx = s->x + along * ax - cam_x;
        int32_t dz = s->z + along * az - cam_z;
        int32_t dy = wy - cam_y;
        int32_t f  = (dx * sr + dz * cr) >> 12;
        int32_t r  = (dx * cr - dz * sr) >> 12;
        int32_t fz = (f * cp + dy * sp) >> 12;
        int32_t v  = (dy * cp - f * sp) >> 12;
        int     out = 0;
        if (fz < 16)                        out |= 1;   /* behind the eye */
        if (r * 256 + fz * COL_VIEW_HW < 0) out |= 2;   /* off the left   */
        if (r * 256 - fz * COL_VIEW_HW > 0) out |= 4;   /* off the right  */
        if (v * 256 + fz * COL_VIEW_HH < 0) out |= 8;   /* off the top    */
        if (v * 256 - fz * COL_VIEW_HH > 0) out |= 16;  /* off the bottom */
        out_all &= out;
        {
            int32_t md = col_abs(dx) + col_abs(dz);
            if (md < near_fog) near_fog = md;
        }
    }
    if (out_all) return 1;
    return near_fog >= g_fog_far;
}

/* ---- Stepping ----------------------------------------------------------------
   May the arch go `d` (+/-1)? Not into a wall - the line along its feet and
   the line through its orb, out to the far edge of the head - not onto the
   player, and not into another Column. */
static int col_step_ok(const Column *s, int d) {
    int32_t ax = col_ax(s), az = col_az(s), floor_y = col_floor_y(s);
    /* Out to the far edge of the COLUMN it stands up as (the wider idle). */
    int32_t far = d * COL_STEP_REACH;
    int32_t lo  = d > 0 ? -COL_IDLE_HALF_W : -COL_STEP_REACH;
    int32_t hi  = d > 0 ? COL_STEP_REACH : COL_IDLE_HALF_W;
    int i;

    if (collision_segment_blocked(s->x, floor_y - COL_HALF, s->z,
                                  s->x + far * ax, floor_y - COL_HALF,
                                  s->z + far * az))
        return 0;
    if (collision_segment_blocked(s->x, floor_y - COL_ORB_ARCH_H, s->z,
                                  s->x + far * ax, floor_y - COL_ORB_ARCH_H,
                                  s->z + far * az))
        return 0;

    if (col_abs(player_y() - s->y) < COL_IDLE_H &&
        col_seg_dist(s, lo, hi, player_x(), player_z()) < COL_PLAYER_CLEAR)
        return 0;

    for (i = 0; i < column_count; i++) {
        const Column *o = &columns[i];
        if (o == s || !o->active || o->state == COL_DEAD || o->area != s->area)
            continue;
        if (col_seg_dist(s, lo, hi, o->x, o->z) < COL_SQ + COL_HALF) return 0;
        if (o->state == COL_ARCH) {
            int32_t hx = o->x + o->dir * COL_STEP * col_ax(o);
            int32_t hz = o->z + o->dir * COL_STEP * col_az(o);
            if (col_seg_dist(s, lo, hi, hx, hz) < COL_SQ + COL_HALF) return 0;
        }
    }
    return 1;
}

/* Which way, or 0 for "not now". Calm: either way at random, whichever is
   open AND hidden - hidden for the column it is now and the arch it would
   become, so the arch can never appear in view. Enraged: toward the player
   along the plane, seen or not. */
static int col_pick_step(const Column *s) {
    if (s->enraged) {
        int32_t along = (player_x() - s->x) * col_ax(s) +
                        (player_z() - s->z) * col_az(s);
        int d = along >= 0 ? 1 : -1;
        return col_step_ok(s, d) ? d : 0;
    } else {
        int d0 = (rand() & 1) ? 1 : -1, k;
        for (k = 0; k < 2; k++) {
            int d = k ? -d0 : d0;
            int32_t lo = d > 0 ? -COL_IDLE_HALF_W : -COL_STEP_REACH;
            int32_t hi = d > 0 ? COL_STEP_REACH : COL_IDLE_HALF_W;
            if (col_step_ok(s, d) && col_span_hidden(s, lo, hi, COL_IDLE_H))
                return d;
        }
        return 0;
    }
}

/* ---- Update ------------------------------------------------------------------ */
void update_columns(void) {
    int i;
    col_update_orbs();
    col_update_shards();

    for (i = 0; i < column_count; i++) {
        Column *s = &columns[i];
        /* current_area, NEVER game_state (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!s->active || s->state == COL_DEAD || s->area != current_area) continue;

        if (s->hit_timer > 0) s->hit_timer--;
        apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                          &s->on_upper_floor, &s->on_ramp);
        if (!s->alive) continue;           /* a STATIC Column is only a wall */

        if (s->timer < 30000) s->timer++;  /* a long wait in view must not wrap */

        if (s->enraged && --s->fire_timer <= 0) {
            col_fire(s);
            s->fire_timer = COL_RAGE_FIRE;
        }

        switch (s->state) {
        case COL_IDLE:
            if (s->timer >= (s->enraged ? COL_RAGE_IDLE : COL_IDLE_FRAMES)) {
                int d = col_pick_step(s);
                if (d) {
                    s->dir   = (int8_t)d;
                    s->state = COL_ARCH;
                    s->timer = 0;
                    /* Calm, the orb goes as the arch appears; enraged, on its
                       own two-second clock instead. */
                    if (!s->enraged) col_fire(s);
                }
            }
            break;

        case COL_ARCH:
            if (s->timer >= (s->enraged ? COL_RAGE_ARCH : COL_ARCH_FRAMES)) {
                /* Stand up again where the head landed. */
                s->x    += s->dir * COL_STEP * col_ax(s);
                s->z    += s->dir * COL_STEP * col_az(s);
                s->state = COL_IDLE;
                s->timer = 0;
            }
            break;

        default:
            break;
        }
    }
}

/* ---- Drawing ----------------------------------------------------------------- */

/* The texture-window bracket: golem.c's. addPrim PREPENDS within a bucket, so
   the restore goes in first and the disable last. */
static void col_bracket_begin(RenderContext *ctx, int32_t otz) {
    if (!col_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
    setTexWindow(restore, &col_tw_restore);
    addPrim(&ot[otz], restore);
    ctx->next_packet += sizeof(DR_TWIN);
}

static void col_bracket_end(RenderContext *ctx, int32_t otz) {
    if (!col_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    RECT full = { 0, 0, 0, 0 };
    DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
    setTexWindow(disable, &full);
    addPrim(&ot[otz], disable);
    ctx->next_packet += sizeof(DR_TWIN);
}

/* One 1 m square, world-fixed in the column's plane. NO backface cull - it is
   two-sided. Fogged the way the room's own walls are (draw_lm_smd): a neutral
   128 blended toward the chapter's red. Each square sorts at its own depth,
   like a wall poly. Returns its OT bucket, or 0 if it was not drawn. */
static int32_t col_draw_square(RenderContext *ctx, const Column *s,
                               const ColPiece *pc, int32_t along, int flip,
                               int32_t hw, int32_t hh) {
    int32_t ax = col_ax(s), az = col_az(s), floor_y = col_floor_y(s);
    int32_t cxw = s->x + along * ax, czw = s->z + along * az;

    int32_t dist = col_abs(cxw - cam_x) + col_abs(czw - cam_z);
    if (dist >= g_fog_far) return 0;

    int32_t lx = cxw - hw * ax, lz = czw - hw * az;
    int32_t rx = cxw + hw * ax, rz = czw + hw * az;
    int16_t yt = (int16_t)(floor_y - pc->h - hh);
    int16_t yb = (int16_t)(floor_y - pc->h + hh);

    SVECTOR v[4];
    v[0].vx = (int16_t)lx; v[0].vy = yt; v[0].vz = (int16_t)lz; v[0].pad = 0;
    v[1].vx = (int16_t)rx; v[1].vy = yt; v[1].vz = (int16_t)rz; v[1].pad = 0;
    v[2].vx = (int16_t)lx; v[2].vy = yb; v[2].vz = (int16_t)lz; v[2].pad = 0;
    v[3].vx = (int16_t)rx; v[3].vy = yb; v[3].vz = (int16_t)rz; v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4], otz, k;
    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return 0;
    for (k = 0; k < 4; k++)
        if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
            sv[k].vy <= -1023 || sv[k].vy >= 1023) return 0;

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return 0;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= UW_SKY_OT)   otz = UW_SKY_OT - 1;   /* in front of the sky */

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    size_t   need    = sizeof(POLY_FT4) + (col_tw_active ? 2 * sizeof(DR_TWIN) : 0);
    if (ctx->next_packet + need > buf_end) return 0;

    /* The room's fog (draw_lm_smd), on a neutral 128 - or a red flash for a
       few frames after a hit. */
    int32_t near = g_fog_near, far = g_fog_far;
    int32_t fog  = dist < near ? near : dist;
    int32_t ff   = far > near ? ((far - fog) << 8) / (far - near) : 256;
    if (ff < 0) ff = 0;
    if (ff > 256) ff = 256;
    int32_t br = 128, bg = 128, bb = 128;
    if (s->hit_timer > COL_BAR_TIMER_MAX - 8) { br = 220; bg = 40; bb = 40; }
    uint8_t r = (uint8_t)((br * ff + UW_FOG_R * (256 - ff)) >> 8);
    uint8_t g = (uint8_t)((bg * ff + UW_FOG_G * (256 - ff)) >> 8);
    uint8_t b = (uint8_t)((bb * ff + UW_FOG_B * (256 - ff)) >> 8);

    uint8_t uL = flip ? pc->u1 : pc->u0;
    uint8_t uR = flip ? pc->u0 : pc->u1;

    col_bracket_begin(ctx, otz);
    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, r, g, b);
    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
    poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
    poly->u0 = uL; poly->v0 = pc->v0;
    poly->u1 = uR; poly->v1 = pc->v0;
    poly->u2 = uL; poly->v2 = pc->v1;
    poly->u3 = uR; poly->v3 = pc->v1;
    poly->tpage = col_tpage;
    poly->clut  = col_clut;
    ctx->next_packet += sizeof(POLY_FT4);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    col_bracket_end(ctx, otz);
    return otz;
}

/* The health bar: the Golem's two TILEs, over the top of the pose. */
static void col_draw_bar(RenderContext *ctx, const Column *s, int32_t near_otz) {
    int32_t lo, hi, top;
    col_extent(s, &lo, &hi, &top);
    int32_t mid = (lo + hi) / 2;
    SVECTOR pt;
    pt.vx = (int16_t)(s->x + mid * col_ax(s));
    pt.vy = (int16_t)(col_floor_y(s) - top - 30);
    pt.vz = (int16_t)(s->z + mid * col_az(s));
    pt.pad = 0;
    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int16_t bar_x   = (int16_t)(sv.vx - 20);
    int16_t bar_top = (int16_t)(sv.vy - 8);
    int32_t bar_otz = near_otz > SCENE_OT_MIN ? near_otz - 1 : SCENE_OT_MIN;

    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setRGB0(bg, 40, 40, 40);
        setXY0(bg, bar_x, bar_top);
        setWH(bg, 40, 5);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[bar_otz + 1], bg);
        ctx->next_packet += sizeof(TILE);
    }
    int16_t fill_w = (int16_t)((s->health * 40) / COL_MAX_HEALTH);
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

static void draw_col_body(RenderContext *ctx, const Column *s) {
    int32_t near_otz = 0x7fffffff, otz;
    int k;
    if (s->state == COL_ARCH) {
        for (k = 0; k < COL_ARCH_PIECES; k++) {
            otz = col_draw_square(ctx, s, &COL_ARCH_SQ[k],
                                  s->dir * COL_ARCH_SQ[k].along, s->dir < 0,
                                  COL_HALF, COL_HALF);
            if (otz > 0 && otz < near_otz) near_otz = otz;
        }
    } else {
        for (k = 0; k < COL_STACK; k++) {
            otz = col_draw_square(ctx, s, &COL_IDLE_SQ[k], 0, 0,
                                  COL_IDLE_HALF_W, COL_IDLE_HALF_H);
            if (otz > 0 && otz < near_otz) near_otz = otz;
        }
    }
    if (s->hit_timer > 0 && near_otz != 0x7fffffff)
        col_draw_bar(ctx, s, near_otz);
}

/* An orb: golem.c's glow point - concentric additive screen-space squares
   round the projection, sized by world distance - in blue. */
static void col_draw_orb(RenderContext *ctx, const ColOrb *f) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (col_abs(f->x - cam_x) + col_abs(f->z - cam_z) >= g_fog_far) return;

    SVECTOR pt;
    pt.vx = (int16_t)f->x; pt.vy = (int16_t)f->y; pt.vz = (int16_t)f->z; pt.pad = 0;
    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    int32_t dx = f->x - cam_x, dy = f->y - cam_y, dz = f->z - cam_z;
    int32_t dist = col_isqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 64) dist = 64;

    int32_t otz = sz >> 2;
    if (otz <= SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= UW_SKY_OT)    otz = UW_SKY_OT - 1;

    static const uint8_t RGB[3][3] = {
        {  30,  70, 200 },   /* the outer haze   */
        {  70, 140, 255 },   /* the orb          */
        { 190, 225, 255 },   /* the bright core  */
    };
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    int ring;
    for (ring = 0; ring < 3; ring++) {
        if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;
        int32_t wh   = (COL_ORB_HALF * 2) >> ring;
        int32_t half = (wh * 256) / dist;
        if (half < 1)   half = 1;
        if (half > 300) half = 300;

        POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setSemiTrans(p, 1);
        setRGB0(p, RGB[ring][0], RGB[ring][1], RGB[ring][2]);
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

/* One shard: draw_glm_shard verbatim - a cube rotated on the CPU, so the GTE's
   view matrix is never touched. */
static void col_draw_shard(RenderContext *ctx, const ColShard *h) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    SVECTOR  wv[8];
    DVECTOR  scr[8];
    int32_t  sz[8], hh, dist, fog, k, f;

    hh = h->half;
    if (h->life < COL_SHARD_SHRINK) hh = (hh * h->life) / COL_SHARD_SHRINK;
    if (hh <= 0) return;

    dist = col_abs(h->x - cam_x) + col_abs(h->z - cam_z);
    if (dist > g_fog_far) return;
    fog = render_fog_scale(dist);
    if (fog > 256) fog = 256;

    int32_t sy = isin(h->ry), cyw = icos(h->ry);
    int32_t sp = isin(h->rx), cp = icos(h->rx);
    for (k = 0; k < 8; k++) {
        int32_t lx = (k & 1) ? hh : -hh;
        int32_t ly = (k & 2) ? hh : -hh;
        int32_t lz = (k & 4) ? hh : -hh;
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

    const uint8_t *rgb = COL_SHARD_RGB[h->col];
    for (f = 0; f < 6; f++) {
        int axis = f >> 1;
        int bit  = 1 << axis;
        int ba   = 1 << ((axis + 1) % 3);
        int bb   = 1 << ((axis + 2) % 3);
        int c0   = (f & 1) ? bit : 0;
        int c1 = c0 | ba, c2 = c0 | bb, c3 = c0 | ba | bb;
        int32_t otz = (sz[c0] + sz[c1] + sz[c2] + sz[c3]) >> 4;
        int32_t sh  = COL_FACE_SHADE[f];
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

void draw_columns(RenderContext *ctx) {
    int i;
    for (i = 0; i < COL_SHARDS; i++)
        if (col_shards[i].life > 0 && col_shards[i].area == current_area)
            col_draw_shard(ctx, &col_shards[i]);
    for (i = 0; i < MAX_COL_ORBS; i++)
        if (col_orbs[i].life > 0 && col_orbs[i].area == current_area)
            col_draw_orb(ctx, &col_orbs[i]);

    if (!tex_loaded) return;
    for (i = 0; i < column_count; i++) {
        const Column *s = &columns[i];
        if (!s->active || s->state == COL_DEAD || s->area != current_area) continue;
        draw_col_body(ctx, s);
    }
}
