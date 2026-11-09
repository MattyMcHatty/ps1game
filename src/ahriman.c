#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"      /* player_x/y/z, cam_*, cam_pitch */
#include "player.h"      /* player_hurt, player_health, game_over, flash_timer */
#include "collision.h"   /* apply_ddog_height, collision_segment_blocked, ... */
#include "particles.h"
#include "texmgr.h"
#include "sound.h"
#include "ahriman.h"

Ahriman ahrimans[MAX_AHRIMANS];
int     ahriman_count = 0;

/* ---- Sprites ----------------------------------------------------------------
   ONE sheet per ROW of the source art, on the crawler's blocks: see ahriman.h.
   Both are quantised against one master palette (tools/split_ahriman_sheet.py),
   so the wing-beat's step from frame 1 to frame 2 - which crosses sheets -
   keeps its colour. */
#define AHR_SHEETS 2
static uint16_t ahr_tpage [AHR_SHEETS] = { 0, 0 };
static uint16_t ahr_clut  [AHR_SHEETS] = { 0, 0 };
static int      ahr_tex_id[AHR_SHEETS] = { -1, -1 };
static int      tex_loaded = 0;

/* Texture window the current area expects, restored after each sprite. Both
   sheets sit at VRAM y=128, i.e. Voff 128, so a room's 128-tall window would
   wrap their V and sample the wrong half of the page. Nessie's bracket. */
static RECT ahr_tw_restore;
static int  ahr_tw_active = 0;

/* The white flash, in frames left. Set when an eye goes off, ticked by
   update_ahrimans, drawn by ahrimans_draw_flash. */
static int ahr_flash = 0;

void ahrimans_set_texwindow(const RECT *tw) {
    if (tw) { ahr_tw_restore = *tw; ahr_tw_active = 1; }
    else    { ahr_tw_active = 0; }
}

void ahrimans_load_textures(void) {
    /* BANK: Chapter 4 only. Derived, not guessed: py tools/check_tex_banks.py
       walks the uploader call graph and fails the build if this is short. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    ahr_tex_id[0] = texmgr_register("\\TEXUNDW\\AHRIMANA.TIM;1");
    ahr_tex_id[1] = texmgr_register("\\TEXUNDW\\AHRIMANB.TIM;1");
    {
        int i;
        for (i = 0; i < AHR_SHEETS; i++) {
            ahr_tpage[i] = texmgr_tpage(ahr_tex_id[i]);
            ahr_clut [i] = texmgr_clut (ahr_tex_id[i]);
        }
    }
    tex_loaded = (ahr_tex_id[0] >= 0 && ahr_tex_id[1] >= 0);
}

/* Nothing to mark taken: the crawler's blocks are put back by main.c's own
   destination-keyed lines on entry to any room that draws their owners
   (ahriman.h), and the Underworld is left only through the title. */
void ahrimans_upload_textures(void) {
    int i;
    for (i = 0; i < AHR_SHEETS; i++) texmgr_upload(ahr_tex_id[i]);
}

/* The rooms whose entry uploads the sheet. main.c keys the upload on this, and
   tools/gen_tex_rooms.py mirrors it as AHRIMAN_ROOMS - keep the two in step, or
   the texture late-loads (logged, still correct). */
int ahriman_room_has_sheet(GameState area) {
    return area == STATE_LAVA_U;
}

/* Integer square root, nessie.c's - including the `t > 0` seed (mistake 11 in
   tools/ADDING_AN_ENEMY.txt). The worst case here is a corner-to-corner
   distance across Lava U, about 1.1e8 squared: inside int32. */
static int32_t ahr_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

static int32_t ahr_len(int32_t dx, int32_t dz) {
    return ahr_isqrt(dx * dx + dz * dz);
}

int32_t ahriman_body_y(const Ahriman *s) {
    int32_t off = -AHR_EYE_RISE;
    if (s->height == AHR_HEIGHT_A) off += AHR_HEIGHT_STEP;
    if (s->height == AHR_HEIGHT_C) off -= AHR_HEIGHT_STEP;
    return s->y + off;
}

void ahriman_body(const Ahriman *s, int32_t *cyc, int32_t *hh, int32_t *hw) {
    *cyc = ahriman_body_y(s);
    *hh  = AHR_HIT_HALF_H;
    *hw  = AHR_HIT_HALF_W;
}

/* Rebuild exactly as ahriman_add leaves one. Only the spawn, the height and the
   area survive the wipe. */
static void ahr_spawn(Ahriman *s, int32_t x, int32_t y, int32_t z,
                      AhrimanHeight h, GameState area) {
    *s = (Ahriman){0};
    s->x = x; s->y = y; s->z = z;
    s->qx = x << 8; s->qz = z << 8;
    s->spawn_x = x; s->spawn_z = z; s->spawn_y = y;
    s->goal_x  = x; s->goal_z  = z;
    s->height  = h;
    s->health  = AHR_MAX_HEALTH * AHR_HP_SCALE;
    s->state   = AHR_HUNT;
    s->active  = 1;
    s->area    = area;
}

int ahriman_add(int32_t x, int32_t z, AhrimanHeight height, GameState area) {
    if (ahriman_count >= MAX_AHRIMANS) return -1;
    int i = ahriman_count++;
    /* The standing anchor of a y=0 floor (nessie_add's reasoning): authored, so
       a save rebuild needs no floor probe; apply_ddog_height corrects it on the
       first frame in a room whose floor is not at 0. */
    ahr_spawn(&ahrimans[i], x, -GROUND_FLOOR_Y, z, height, area);
    /* Stagger the route solves so two never land on the same frame. */
    ahrimans[i].route_tick = i * 3;
    return i;
}

void ahrimans_init(void) {
    /* Placements are seeded on a room's first entry by world.c. */
    ahriman_count = 0;
    ahr_flash     = 0;
}

void ahrimans_reset(void) {
    ahriman_count = 0;
    ahr_flash     = 0;
}

void ahrimans_rest(void) {
    int i;
    for (i = 0; i < ahriman_count; i++) {
        Ahriman *s = &ahrimans[i];
        if (!s->active || s->state == AHR_DEAD) continue;
        ahr_spawn(s, s->spawn_x, s->spawn_y, s->spawn_z, s->height, s->area);
        s->route_tick = i * 3;
    }
    ahr_flash = 0;
}

int ahrimans_hold_player(GameState area) {
    int i;
    for (i = 0; i < ahriman_count; i++) {
        const Ahriman *s = &ahrimans[i];
        if (!s->active || s->area != area) continue;
        if (s->state == AHR_PRIMED || s->state == AHR_CHARGE) return 1;
    }
    return 0;
}

/* ---- Damage ------------------------------------------------------------------
   HALF-POINTS (ahriman.h). Flame Rounds do half, everything else full. Not a
   Weakness table: damage_scale() would floor a halved 1 back up to 1, and the
   table cannot express "half of one" - which is exactly what the scale exists
   for. */
int32_t ahriman_scale_damage(int32_t base, DamageType type) {
    if (base <= 0) return 0;
    if (type == DMG_FLAME) return base * AHR_HP_SCALE / 2;
    return base * AHR_HP_SCALE;
}

void ahriman_damage(Ahriman *s, int dmg) {
    if (!s->active || s->state == AHR_DEAD || dmg <= 0) return;
    s->health   -= dmg;
    s->hit_timer = AHR_BAR_TIMER_MAX;
    if (s->health <= 0) {
        s->health = 0;
        s->state  = AHR_DEAD;
        spawn_blood_burst(s->x, ahriman_body_y(s), s->z);
        sound_play(SFX_TNTCL_DIE);   /* BORROWED - the tentacle's cry (ahriman.h) */
    } else {
        sound_play(SFX_AXEHIT);
    }
}

/* The crucifaxe test, measured RADIALLY to the body's surface plus the
   vertical gap (heights A and C are 60 off the eye) - ahriman.h works the
   charge's stop distance through it. A hit shoves the body straight away from
   the player; it comes back in under its own steam. */
int ahrimans_try_hit(void) {
    int i;
    for (i = 0; i < ahriman_count; i++) {
        Ahriman *s = &ahrimans[i];
        if (!s->active || s->state == AHR_DEAD || s->area != current_area) continue;
        int32_t dx = s->x - cam_x;
        int32_t dz = s->z - cam_z;
        int32_t dy = ahriman_body_y(s) - cam_y;
        int32_t d  = ahr_len(dx, dz);
        int32_t gh = d > AHR_AXE_SURFACE ? d - AHR_AXE_SURFACE : 0;
        if (gh + (dy < 0 ? -dy : dy) >= SWING_RANGE) continue;
        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;
        if (d > 0) {
            s->kb_vx = (dx * AHR_KNOCKBACK) / d;
            s->kb_vz = (dz * AHR_KNOCKBACK) / d;
        }
        ahriman_damage(s, AHR_HP_SCALE);   /* a swing is a whole hit point */
        return 1;
    }
    return 0;
}

/* The push-out: nessies_collide's capped radial push, so it can never throw
   the player through a wall. The vertical gate is the body's span against the
   player's. */
void ahrimans_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < ahriman_count; i++) {
        Ahriman *s = &ahrimans[i];
        if (!s->active || s->state == AHR_DEAD || s->area != current_area) continue;

        int32_t by  = ahriman_body_y(s);
        int32_t top = by - AHR_HIT_HALF_H;
        int32_t bot = by + AHR_HIT_HALF_H;
        if (py + GROUND_FLOOR_Y < top || py - 30 > bot) continue;

        int32_t dx   = *px - s->x;
        int32_t dz   = *pz - s->z;
        int32_t need = AHR_PUSH_RADIUS + radius;
        int32_t d    = ahr_len(dx, dz);
        if (d >= need) continue;
        int32_t step = need - d;
        if (step > AHR_PUSH_STEP) step = AHR_PUSH_STEP;
        if (d == 0) { *px += step; continue; }   /* dead centre */
        *px += (dx * step) / d;
        *pz += (dz * step) / d;
    }
}

/* ---- Routing -----------------------------------------------------------------
   A body that floats straight at the player cannot get round a solid block -
   Lava U's rock island stands between its south-west corner and the north-east
   door (tools/ADDING_AN_ENEMY.txt mistake 17: steering is not routing). So each
   room that needs it gets a handful of WAYPOINTS and which pairs of them see
   each other, and the route is the cheapest of:
     straight at the player, if the sightline is clear; else
     to a waypoint it can see, then on through the waypoint graph to one that
     sees the player - Bellman-Ford over at most AHR_MAX_WP nodes.
   Re-solved every AHR_ROUTE_EVERY frames, not every frame: it is a handful of
   segment tests and the answer only changes as fast as the bodies move.

   A room with no entry here simply goes straight and slides along walls. */
#define AHR_MAX_WP        4
#define AHR_ROUTE_EVERY  10
#define AHR_WP_ARRIVE   150     /* a waypoint this close is "reached"        */
#define AHR_NO_ROUTE    0x7fffffff

typedef struct {
    GameState area;
    int       n;
    int32_t   x[AHR_MAX_WP], z[AHR_MAX_WP];
    uint8_t   adj[AHR_MAX_WP];  /* bit j: waypoint j is in sight of this one */
} AhrRoute;

/* LAVA U. The rock island fills x[-3500,-1300] z[-3101,300], so the U's two
   arms meet only across its southern band, z[-5100,-3101]. One waypoint at the
   foot of each arm, 800 south of the island's tip and well clear of the rim:
     0  (-4500,-3900)  the foot of the WEST arm
     1  ( -300,-3900)  the foot of the EAST arm
   and they see each other along z=-3900. */
static const AhrRoute ahr_routes[] = {
    { STATE_LAVA_U, 2, { -4500, -300 }, { -3900, -3900 }, { 0x2, 0x1 } },
};
#define AHR_ROUTE_COUNT ((int)(sizeof ahr_routes / sizeof ahr_routes[0]))

static int ahr_sees(int32_t ax, int32_t ay, int32_t az,
                    int32_t bx, int32_t by, int32_t bz) {
    return !collision_segment_blocked(ax, ay, az, bx, by, bz);
}

static void ahr_solve_route(Ahriman *s, int32_t px, int32_t py, int32_t pz) {
    int32_t by = ahriman_body_y(s);
    s->goal_x = px;
    s->goal_z = pz;
    if (ahr_sees(s->x, by, s->z, px, py, pz)) return;

    const AhrRoute *r = NULL;
    int k, j, it;
    for (k = 0; k < AHR_ROUTE_COUNT; k++)
        if (ahr_routes[k].area == s->area) { r = &ahr_routes[k]; break; }
    if (!r) return;

    /* Cost from each waypoint to the player through the graph. */
    int32_t to_p[AHR_MAX_WP];
    for (k = 0; k < r->n; k++)
        to_p[k] = ahr_sees(r->x[k], py, r->z[k], px, py, pz)
                ? ahr_len(px - r->x[k], pz - r->z[k]) : AHR_NO_ROUTE;
    for (it = 0; it < r->n; it++)
        for (k = 0; k < r->n; k++)
            for (j = 0; j < r->n; j++) {
                if (!(r->adj[k] & (1u << j)) || to_p[j] == AHR_NO_ROUTE) continue;
                int32_t c = ahr_len(r->x[j] - r->x[k], r->z[j] - r->z[k]) + to_p[j];
                if (c < to_p[k]) to_p[k] = c;
            }

    /* The first hop: a waypoint in sight, not already reached, cheapest total. */
    int32_t best = AHR_NO_ROUTE;
    for (k = 0; k < r->n; k++) {
        if (to_p[k] == AHR_NO_ROUTE) continue;
        int32_t d = ahr_len(r->x[k] - s->x, r->z[k] - s->z);
        if (d < AHR_WP_ARRIVE) continue;
        if (!ahr_sees(s->x, by, s->z, r->x[k], by, r->z[k])) continue;
        if (d + to_p[k] < best) {
            best      = d + to_p[k];
            s->goal_x = r->x[k];
            s->goal_z = r->z[k];
        }
    }
}

/* ---- Movement ----------------------------------------------------------------
   One step of (dqx,dqz) in 1/256 units, then the walls hold the body
   AHR_BODY_RADIUS off them and the corrected point is written back to the fixed
   point position, so a push slides it along the face rather than through it. */
static void ahr_move(Ahriman *s, int32_t dqx, int32_t dqz) {
    s->qx += dqx;
    s->qz += dqz;
    int32_t nx = s->qx >> 8, nz = s->qz >> 8;
    int32_t cx = nx, cz = nz;
    apply_flat_entity_collision(&cx, &cz, AHR_BODY_RADIUS);
    if (cx != nx) s->qx = cx << 8;
    if (cz != nz) s->qz = cz << 8;
    s->x = s->qx >> 8;
    s->z = s->qz >> 8;
}

static void ahr_step_toward(Ahriman *s, int32_t gx, int32_t gz, int32_t speed_q8) {
    int32_t dx = gx - s->x, dz = gz - s->z;
    int32_t d  = ahr_len(dx, dz);
    if (d <= 0) return;
    ahr_move(s, (dx * speed_q8) / d, (dz * speed_q8) / d);
}

/* ---- "Is it in the camera's view?" -------------------------------------------
   The body's CENTRE projects inside the screen, and nothing solid stands
   between it and the camera (collision_hidden_from_camera). The projection is
   worked by hand rather than through the GTE because this runs in the update,
   where the GTE holds nothing useful: camera space from cam_rot (the forward
   vector is (isin, icos), right is (icos, -isin) - the billboards' own), then
   cam_pitch (positive tilts the view DOWN), then gte_SetGeomScreen(256) against
   a 320x240 screen. Look mode folds its offset into cam_rot / cam_pitch for the
   frame (camera.h), so a player glancing away with Circle held looks away here
   too. The camera, not the player: a fixed puzzle shot is what is on screen. */
static int ahr_in_view(const Ahriman *s) {
    int32_t by = ahriman_body_y(s);
    int32_t dx = s->x - cam_x;
    int32_t dy = by - cam_y;
    int32_t dz = s->z - cam_z;
    int32_t sr = isin(cam_rot), cr = icos(cam_rot);
    int32_t f  = (dx * sr + dz * cr) >> 12;
    int32_t r  = (dx * cr - dz * sr) >> 12;
    int32_t sp = isin(cam_pitch), cp = icos(cam_pitch);
    int32_t fz = (f * cp + dy * sp) >> 12;
    int32_t v  = (dy * cp - f * sp) >> 12;
    if (fz < 64) return 0;                                       /* behind */
    if ((r < 0 ? -r : r) * 256 > fz * (SCREEN_XRES / 2)) return 0;
    if ((v < 0 ? -v : v) * 256 > fz * (SCREEN_YRES / 2)) return 0;
    return !collision_hidden_from_camera(s->x, by, s->z);
}

static void ahr_bite(Ahriman *s) {
    if (game_over) return;
    player_hurt(AHR_BITE_DAMAGE);
    sound_play(SFX_HURT);
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
        sound_play(SFX_DIE);
    }
    s->timer = AHR_BITE_COOLDOWN;
}

void update_ahrimans(void) {
    int i;
    if (ahr_flash > 0) ahr_flash--;

    for (i = 0; i < ahriman_count; i++) {
        Ahriman *s = &ahrimans[i];
        /* current_area, NEVER game_state (tools/ADDING_AN_ENEMY.txt STEP 6): the
           inventory menu must not pause the freeze or the charge. */
        if (!s->active || s->state == AHR_DEAD || s->area != current_area) continue;

        if (s->hit_timer > 0) s->hit_timer--;
        s->anim_tick++;   /* the wing-beat never stops while it lives */

        /* The FLOOR anchor (creep.h's model): the body floats, the anchor is
           clamped to the floor under it, and ahriman_body_y lifts it. */
        apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                          &s->on_upper_floor, &s->on_ramp);
        if ((s->qx >> 8) != s->x) s->qx = s->x << 8;
        if ((s->qz >> 8) != s->z) s->qz = s->z << 8;

        /* player_x/y/z, not cam_*: it chases the PLAYER through a camera-locked
           shot (camera.h). The gaze, below, asks the CAMERA. */
        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - s->x, dz = pz - s->z;
        int32_t d  = ahr_len(dx, dz);

        if (--s->route_tick <= 0) {
            s->route_tick = AHR_ROUTE_EVERY;
            ahr_solve_route(s, px, py, pz);
        }

        /* A crucifaxe shove takes the whole frame: slide back, decay, nothing
           else - no bite and no advance while it is still flying. */
        if (s->kb_vx || s->kb_vz) {
            ahr_move(s, s->kb_vx << 8, s->kb_vz << 8);
            s->kb_vx = s->kb_vx > 0 ? (s->kb_vx > AHR_KB_DECAY ? s->kb_vx - AHR_KB_DECAY : 0)
                                    : (s->kb_vx < -AHR_KB_DECAY ? s->kb_vx + AHR_KB_DECAY : 0);
            s->kb_vz = s->kb_vz > 0 ? (s->kb_vz > AHR_KB_DECAY ? s->kb_vz - AHR_KB_DECAY : 0)
                                    : (s->kb_vz < -AHR_KB_DECAY ? s->kb_vz + AHR_KB_DECAY : 0);
            if (s->state == AHR_CHARGE && s->timer > 0) s->timer--;
            continue;
        }

        switch (s->state) {

        case AHR_HUNT:
            if (d > AHR_HOLD_DIST) {
                /* Out of range: follow. The gaze PAUSES here rather than
                   resetting - only leaving the view resets it (ahriman.h). */
                ahr_step_toward(s, s->goal_x, s->goal_z, AHR_SPEED_Q8);
                break;
            }
            /* In range: hang, and watch to be watched. */
            if (!ahr_in_view(s)) {
                s->gaze = 0;
                break;
            }
            if (++s->gaze >= AHR_GAZE_FRAMES) {
                /* The eye goes off: the white flash, and from this frame the
                   player is frozen (main.c reads ahrimans_hold_player). */
                s->state = AHR_PRIMED;
                s->timer = AHR_PRIME_FRAMES;
                s->gaze  = 0;
                ahr_flash = AHR_FLASH_HOLD + AHR_FLASH_FADE;
                sound_play(SFX_AHR_FLASH);
            }
            break;

        case AHR_PRIMED:
            if (--s->timer <= 0) {
                /* Bite cooldown starts at 0: the first bite lands on arrival. */
                s->state = AHR_CHARGE;
                s->timer = 0;
            }
            break;

        case AHR_CHARGE: {
            if (s->timer > 0) s->timer--;
            if (d > AHR_STOP_DIST)
                ahr_step_toward(s, s->goal_x, s->goal_z, AHR_CHARGE_Q8);
            /* Reach: radial in the plane, and the bodies must share a storey -
               tested SEPARATELY (the runbook's contact-reach rule). */
            int32_t ddx = px - s->x, ddz = pz - s->z;
            int32_t dy  = py - ahriman_body_y(s);
            if (s->timer == 0 &&
                ahr_len(ddx, ddz) <= AHR_BITE_REACH &&
                (dy < 0 ? -dy : dy) < 250)
                ahr_bite(s);
            break;
        }

        default:
            break;
        }
    }

    /* Ahriman vs Ahriman, AFTER every body has moved, so two hanging at the
       same 1500 cannot sit inside each other. */
    int a, b;
    for (a = 0; a < ahriman_count; a++) {
        Ahriman *sa = &ahrimans[a];
        if (!sa->active || sa->state == AHR_DEAD || sa->area != current_area) continue;
        for (b = a + 1; b < ahriman_count; b++) {
            Ahriman *sb = &ahrimans[b];
            if (!sb->active || sb->state == AHR_DEAD || sb->area != current_area) continue;
            int32_t cdx  = sa->x - sb->x;
            int32_t cdz  = sa->z - sb->z;
            int32_t dist = ahr_len(cdx, cdz);
            int32_t need = AHR_BODY_RADIUS * 2;
            if (dist >= need || dist <= 0) continue;
            int32_t push = (need - dist) / 2;
            ahr_move(sa,  ((cdx * push) / dist) << 8,  ((cdz * push) / dist) << 8);
            ahr_move(sb, -((cdx * push) / dist) << 8, -((cdz * push) / dist) << 8);
        }
    }
}

/* ---- The texture-window bracket: nessie.c's. addPrim PREPENDS within a
   bucket, so the restore goes in first and the disable last, and the GPU draws
   disable, sprite, restore. The caller reserves the space. */
static void ahr_bracket_begin(RenderContext *ctx, int32_t otz) {
    if (!ahr_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
    setTexWindow(restore, &ahr_tw_restore);
    addPrim(&ot[otz], restore);
    ctx->next_packet += sizeof(DR_TWIN);
}

static void ahr_bracket_end(RenderContext *ctx, int32_t otz) {
    if (!ahr_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    RECT full = { 0, 0, 0, 0 };   /* mask 0 = no wrapping, full page */
    DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
    setTexWindow(disable, &full);
    addPrim(&ot[otz], disable);
    ctx->next_packet += sizeof(DR_TWIN);
}

/* One camera-facing quad. Frame f is in sheet f/2, column f%2, each 128 texels
   square; inset one texel on every edge so a magnified quad cannot sample the
   neighbouring frame. V is the row within the 256-tall page and the sheets sit
   at y=128, so 129..254. No gte_nclip: a camera-facing quad has no back face
   (mistake 4). */
static void draw_ahr_sprite(RenderContext *ctx, Ahriman *s, int frame) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);
    int32_t cx = s->x, cz = s->z, cy = ahriman_body_y(s);

    int16_t dwx = (int16_t)((AHR_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((AHR_HALF_W * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - AHR_HALF_H);
    int16_t vy_bot = (int16_t)(cy + AHR_HALF_H);

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
    size_t   need    = sizeof(POLY_FT4) + (ahr_tw_active ? 2 * sizeof(DR_TWIN) : 0);
    if (ctx->next_packet + need > buf_end) return;

    int u0 = (frame & 1) * 128;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    if (s->hit_timer > AHR_BAR_TIMER_MAX - 8)      /* a brief red flash on a hit */
        setRGB0(poly, fog8, fog8 >> 2, fog8 >> 2);
    else
        setRGB0(poly, fog8, fog8, fog8);
    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
    poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
    poly->u0 = (uint8_t)(u0 + 1);   poly->v0 = 129;
    poly->u1 = (uint8_t)(u0 + 126); poly->v1 = 129;
    poly->u2 = (uint8_t)(u0 + 1);   poly->v2 = 254;
    poly->u3 = (uint8_t)(u0 + 126); poly->v3 = 254;
    poly->tpage = ahr_tpage[frame >> 1];
    poly->clut  = ahr_clut [frame >> 1];
    ctx->next_packet += sizeof(POLY_FT4);

    ahr_bracket_begin(ctx, otz);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    ahr_bracket_end(ctx, otz);

    if (s->hit_timer <= 0) return;

    /* The health bar: nessie.c's two TILEs. */
    int16_t bar_cx  = (sv[0].vx + sv[1].vx) / 2;
    int16_t bar_top = (sv[0].vy < sv[1].vy ? sv[0].vy : sv[1].vy) + 8;
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

    int16_t fill_w = (int16_t)((s->health * 40) / (AHR_MAX_HEALTH * AHR_HP_SCALE));
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

void draw_ahrimans(RenderContext *ctx) {
    int i;
    if (!tex_loaded) return;
    for (i = 0; i < ahriman_count; i++) {
        Ahriman *s = &ahrimans[i];
        if (!s->active || s->state == AHR_DEAD || s->area != current_area) continue;

        int32_t dx = s->x - cam_x;
        int32_t dz = s->z - cam_z;
        int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (wdist >= g_fog_far) continue;
        /* Behind the island: skip the fill (collision.h). Never once it has
           the player - an attacker must not be invisible (mistake 10). */
        if (s->state == AHR_HUNT && wdist > AHR_STOP_DIST * 2 &&
            collision_hidden_from_camera(s->x, ahriman_body_y(s), s->z))
            continue;

        draw_ahr_sprite(ctx, s, (s->anim_tick / AHR_ANIM_RATE) & 3);
    }
}

/* The flash: player.c's poison wash, white and at full strength - an ADDITIVE
   full-screen tile (abr=1) of AHR_FLASH_HOLD frames at 255, which saturates the
   whole frame to white, then a fade over AHR_FLASH_FADE. Same OT slot as the
   poison and lava tints (3): over the scene, both additive. */
#define OT_AHR_FLASH 3
void ahrimans_draw_flash(RenderContext *ctx) {
    if (ahr_flash <= 0) return;
    int32_t k = ahr_flash >= AHR_FLASH_FADE ? 255
                                            : (ahr_flash * 255) / AHR_FLASH_FADE;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) + sizeof(DR_TPAGE) > buf_end) return;

    TILE *t = (TILE *)ctx->next_packet;
    setTile(t);
    setSemiTrans(t, 1);
    setRGB0(t, (uint8_t)k, (uint8_t)k, (uint8_t)k);
    setXY0(t, 0, 0);
    setWH(t, SCREEN_XRES, SCREEN_YRES);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_AHR_FLASH], t);
    ctx->next_packet += sizeof(TILE);

    DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(dp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_AHR_FLASH], dp);
    ctx->next_packet += sizeof(DR_TPAGE);
}
