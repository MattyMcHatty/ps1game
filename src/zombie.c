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
#include "particles.h"
#include "zombie.h"
#include "fatdoor.h"
#include "sound.h"

Zombie zombies[MAX_ZOMBIES];
int    zombie_count = 0;

static uint16_t sleep_tpage  = 0, sleep_clut  = 0;
static uint16_t alert_tpage  = 0, alert_clut  = 0;
static uint16_t shadow_tpage = 0, shadow_clut = 0;

/* Texture window the current area expects, restored after each sprite (see
   draw_zmb_sprite). Set by zombies_set_texwindow(); inactive by default. */
static RECT zmb_tw_restore;
static int  zmb_tw_active = 0;

void zombies_set_texwindow(const RECT *tw) {
    if (tw) { zmb_tw_restore = *tw; zmb_tw_active = 1; }
    else    { zmb_tw_active = 0; }
}

static void load_tim(const char *filename, uint16_t *tpage_out, uint16_t *clut_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return;
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

/* Load the sprite textures. Call ONCE at startup — LoadImage is only safe
   before the per-frame render loop begins (see tools/TEXTURING_NOTES.txt). */
void zombies_load_textures(void) {
    load_tim("\\ZSLEEP.TIM;1", &sleep_tpage,  &sleep_clut);
    load_tim("\\ZALERT.TIM;1", &alert_tpage,  &alert_clut);
    load_tim("\\SHADOW.TIM;1", &shadow_tpage, &shadow_clut);
}

int zombie_add(int32_t x, int32_t y, int32_t z) {
    if (zombie_count >= MAX_ZOMBIES) return -1;
    int i = zombie_count++;
    Zombie *z0 = &zombies[i];
    *z0 = (Zombie){0};
    z0->x = x; z0->y = y; z0->z = z;
    z0->health = ZMB_MAX_HEALTH;
    z0->state  = ZMB_DORMANT;
    z0->active = 1;
    return i;
}

void zombies_init(void) {
    /* Zombies are seeded per-room by the world system (see world.c), so the
       live array starts empty. */
    zombie_count = 0;
}

void zombies_reset(void) {
    /* Fresh game: clear the live array. Each room's resident zombies are
       re-seeded by world_enter() on first entry. */
    zombie_count = 0;
}

/* -----------------------------------------------------------------------
 * Navigation graph
 *
 * Pure "steer at the player" gets stuck on doorway edges when the player isn't
 * lined up with the gap. Instead, zombies route room-to-room: each room is a
 * zone (rectangle), each doorway is a node bridging two zones. A zombie heads
 * straight at the player when they share a zone, otherwise it walks to the
 * doorway node that leads toward the player's zone, re-evaluating as it crosses
 * into the next room. This funnels them cleanly through openings (and drives
 * them squarely into closed doors so they batter through).
 *
 * NOTE: this table describes the kitchen_dining layout — the only place
 * zombies currently live. If they are ever placed in another area it needs to
 * describe that area (or be supplied by the area, like zombies_set_texwindow).
 * -------------------------------------------------------------------------*/
#define NAV_ZONE_COUNT 4
#define NAV_NODE_COUNT 3

typedef struct { int32_t min_x, max_x, min_z, max_z; } NavZone;
typedef struct { int32_t x, z; int za, zb; }           NavNode;

static const NavZone nav_zones[NAV_ZONE_COUNT] = {
    { -3294, -590, -1000,  1000 },  /* 0: big kitchen room       */
    {  -640,  600, -1040,  1000 },  /* 1: dining / entry (hub)   */
    {  -581,   -8, -1699,  -980 },  /* 2: south corridor         */
    {    18,  591, -1699,  -980 },  /* 3: SE little room         */
};

static const NavNode nav_nodes[NAV_NODE_COUNT] = {
    { -596,  -371, 0, 1 },   /* big-room <-> dining fat door */
    { -300, -1013, 1, 2 },   /* dining   <-> corridor fat door */
    {  300, -1013, 1, 3 },   /* dining   <-> SE-room fat door */
};

static int nav_zone_at(int32_t x, int32_t z) {
    int i;
    for (i = 0; i < NAV_ZONE_COUNT; i++) {
        const NavZone *zn = &nav_zones[i];
        if (x >= zn->min_x && x <= zn->max_x &&
            z >= zn->min_z && z <= zn->max_z)
            return i;
    }
    return -1;
}

/* First doorway node to walk to when travelling from zone `from` to zone `to`.
   BFS over the (small) zone graph; returns -1 to mean "go straight". */
static int nav_next_node(int from, int to) {
    if (from < 0 || to < 0 || from == to) return -1;

    int prev_zone[NAV_ZONE_COUNT];
    int prev_node[NAV_ZONE_COUNT];
    int visited[NAV_ZONE_COUNT];
    int queue[NAV_ZONE_COUNT];
    int qh = 0, qt = 0, i, z;

    for (i = 0; i < NAV_ZONE_COUNT; i++) visited[i] = 0;
    visited[from] = 1; prev_zone[from] = -1; prev_node[from] = -1;
    queue[qt++] = from;

    while (qh < qt) {
        z = queue[qh++];
        if (z == to) break;
        for (i = 0; i < NAV_NODE_COUNT; i++) {
            int other = -1;
            if      (nav_nodes[i].za == z) other = nav_nodes[i].zb;
            else if (nav_nodes[i].zb == z) other = nav_nodes[i].za;
            if (other >= 0 && !visited[other]) {
                visited[other]   = 1;
                prev_zone[other] = z;
                prev_node[other] = i;
                queue[qt++]      = other;
            }
        }
    }

    if (!visited[to]) return -1;            /* unreachable: go straight */

    /* Walk the predecessor chain back to `from`; the node on that first hop is
       the one to head for now. */
    z = to;
    while (prev_zone[z] != from) {
        if (prev_zone[z] < 0) return -1;
        z = prev_zone[z];
    }
    return prev_node[z];
}

void update_zombies(void) {
    static int hurt_sfx_cooldown = 0;
    int i;
    if (hurt_sfx_cooldown > 0) hurt_sfx_cooldown--;
    for (i = 0; i < zombie_count; i++) {
        Zombie *d = &zombies[i];
        if (!d->active || d->state == ZMB_DEAD) continue;

        if (d->hit_timer    > 0) d->hit_timer--;
        if (d->damage_timer > 0) d->damage_timer--;
        if (d->door_timer   > 0) d->door_timer--;
        if (d->state == ZMB_ALERT) d->anim_tick++;

        apply_ddog_height(&d->x, &d->y, &d->z, &d->vy,
                          &d->on_upper_floor, &d->on_ramp);

        if (d->kb_vx != 0 || d->kb_vz != 0) {
            d->x += d->kb_vx;
            d->z += d->kb_vz;
            apply_flat_entity_collision(&d->x, &d->z, ZMB_BODY_RADIUS);
            crates_collide(&d->x, d->y, &d->z, 80);
            fatdoors_collide(&d->x, d->y, &d->z, ZMB_DOOR_CLEARANCE);
            if (d->kb_vx > 0) d->kb_vx =  (  d->kb_vx * 7) >> 3;
            else               d->kb_vx = -((-d->kb_vx * 7) >> 3);
            if (d->kb_vz > 0) d->kb_vz =  (  d->kb_vz * 7) >> 3;
            else               d->kb_vz = -((-d->kb_vz * 7) >> 3);
            continue;
        }

        int32_t dx     = cam_x - d->x;
        int32_t dy     = cam_y - d->y;
        int32_t dz     = cam_z - d->z;
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);

        if (d->state == ZMB_DORMANT) {
            if (dist2d < ZMB_WAKE_RADIUS) {
                d->state       = ZMB_ALERT;
                d->groan_timer = ZMB_GROAN_INTERVAL;
                sound_play(SFX_ZOMBIE);
            } else {
                continue;   /* silent while asleep */
            }
        }

        /* Keep groaning on an interval while alert and alive. */
        if (d->state == ZMB_ALERT) {
            if (--d->groan_timer <= 0) {
                d->groan_timer = ZMB_GROAN_INTERVAL;
                sound_play(SFX_ZOMBIE);
            }
        }

        if (!game_over && dist3d < ZMB_CATCH_DIST && d->damage_timer == 0) {
            d->damage_timer = ZMB_DAMAGE_COOLDOWN;
            player_health  -= ZMB_DAMAGE_AMOUNT;
            if (hurt_sfx_cooldown == 0) {
                sound_play(SFX_HURT);
                hurt_sfx_cooldown = 30;
            }
            if (player_health <= 0) {
                player_health = 0;
                game_over     = 1;
                flash_timer   = 90;
            }
        }

        /* --- Pick a goal: the player when we share a room, otherwise the
           doorway waypoint that leads toward the player's room. --- */
        int zfrom = nav_zone_at(d->x, d->z);
        int zto   = nav_zone_at(cam_x, cam_z);
        int node  = nav_next_node(zfrom, zto);
        int32_t goal_x = cam_x, goal_z = cam_z;
        if (node >= 0) { goal_x = nav_nodes[node].x; goal_z = nav_nodes[node].z; }
        int32_t gdx = goal_x - d->x;
        int32_t gdz = goal_z - d->z;

        /* Batter the fat door at our current waypoint, but ONLY when it actually
           blocks the way to it: probe a step toward the goal and see if the door
           collision shoves us back. So a zombie breaks through a door it needs
           to pass, and ignores doors it's merely walking by. */
        if (node >= 0 && d->door_timer == 0) {
            int32_t gd = (gdx < 0 ? -gdx : gdx) + (gdz < 0 ? -gdz : gdz);
            if (gd > 0) {
                int32_t step = ZMB_BODY_RADIUS + 40;
                int32_t tx = d->x + (gdx * step) / gd;
                int32_t tz = d->z + (gdz * step) / gd;
                int32_t cx = tx, cz = tz;
                fatdoors_collide(&cx, d->y, &cz, ZMB_BODY_RADIUS);
                if ((cx != tx || cz != tz) &&
                    fatdoors_damage_at(nav_nodes[node].x, nav_nodes[node].z, 0, 1))
                    d->door_timer = ZMB_DOOR_COOLDOWN;
            }
        }

        if (dist2d < ZMB_CATCH_DIST) continue;

        /* --- Separation: soft push away from nearby zombies --- */
        int32_t sep_x = 0, sep_z = 0;
        int j;
        for (j = 0; j < zombie_count; j++) {
            if (j == i) continue;
            Zombie *other = &zombies[j];
            if (!other->active || other->state == ZMB_DEAD) continue;
            int32_t odx   = d->x - other->x;
            int32_t odz   = d->z - other->z;
            int32_t odist = (odx < 0 ? -odx : odx) + (odz < 0 ? -odz : odz);
            if (odist < ZMB_SEP_RADIUS && odist > 0) {
                int32_t push = ZMB_SEP_RADIUS - odist;   /* stronger when closer */
                sep_x += (odx * push) / odist;
                sep_z += (odz * push) / odist;
            }
        }

        /* --- Desired direction: toward the goal, biased by separation --- */
        int32_t desired_x = gdx + sep_x * ZMB_SEP_WEIGHT;
        int32_t desired_z = gdz + sep_z * ZMB_SEP_WEIGHT;
        int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                               (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;

        /* --- Obstacle feeler: probe ahead in the desired direction --- */
        int32_t feeler_x = d->x + (desired_x * ZMB_FEELER_LEN) / desired_dist;
        int32_t feeler_z = d->z + (desired_z * ZMB_FEELER_LEN) / desired_dist;
        int32_t fx = feeler_x, fz = feeler_z;
        crates_collide(&fx, d->y, &fz, 80);
        /* Deliberately NOT probing fat doors here: doors only ever fill the
           doorways a zombie wants to pass, so they must not trigger wall-follow
           (which steered the zombie away before it could reach and batter one).
           The door is still a solid obstacle in the real movement step below. */
        apply_flat_entity_collision(&fx, &fz, ZMB_BODY_RADIUS);
        int blocked = (fx != feeler_x || fz != feeler_z);

        /* Perpendiculars to the goal direction — the two ways to slide
           along a wall. */
        int32_t pl_x = -gdz, pl_z =  gdx;   /* left  */
        int32_t pr_x =  gdz, pr_z = -gdx;   /* right */

        if (blocked && d->steer_timer <= 0) {
            /* Newly blocked: probe BOTH sides and commit to one for a while so
               the zombie follows the wall consistently instead of flip-flopping. */
            int32_t pl_dist = (pl_x < 0 ? -pl_x : pl_x) + (pl_z < 0 ? -pl_z : pl_z);
            int32_t pr_dist = (pr_x < 0 ? -pr_x : pr_x) + (pr_z < 0 ? -pr_z : pr_z);
            if (pl_dist == 0) pl_dist = 1;
            if (pr_dist == 0) pr_dist = 1;

            int32_t lx = d->x + (pl_x * ZMB_FEELER_LEN) / pl_dist;
            int32_t lz = d->z + (pl_z * ZMB_FEELER_LEN) / pl_dist;
            int32_t rx = d->x + (pr_x * ZMB_FEELER_LEN) / pr_dist;
            int32_t rz = d->z + (pr_z * ZMB_FEELER_LEN) / pr_dist;

            int32_t tlx = lx, tlz = lz;
            crates_collide(&tlx, d->y, &tlz, 80);
            apply_flat_entity_collision(&tlx, &tlz, ZMB_BODY_RADIUS);
            int left_blocked = (tlx != lx || tlz != lz);

            int32_t trx = rx, trz = rz;
            crates_collide(&trx, d->y, &trz, 80);
            apply_flat_entity_collision(&trx, &trz, ZMB_BODY_RADIUS);
            int right_blocked = (trx != rx || trz != rz);

            if (left_blocked && !right_blocked) {
                d->steer_dir = +1;
            } else if (right_blocked && !left_blocked) {
                d->steer_dir = -1;
            } else {
                /* Both open (or both blocked): pick the side whose probe ends
                   closer to the goal, so the zombie hugs the wall toward the
                   opening rather than away from it. */
                int32_t ld = (goal_x - lx < 0 ? lx - goal_x : goal_x - lx) +
                             (goal_z - lz < 0 ? lz - goal_z : goal_z - lz);
                int32_t rd = (goal_x - rx < 0 ? rx - goal_x : goal_x - rx) +
                             (goal_z - rz < 0 ? rz - goal_z : goal_z - rz);
                d->steer_dir = (ld <= rd) ? -1 : +1;
            }
            d->steer_timer = ZMB_STEER_COMMIT;
        }

        /* While committed, follow the chosen wall side regardless of small
           changes, so the zombie slides past corners cleanly. */
        if (d->steer_timer > 0) {
            if (d->steer_dir < 0) { desired_x = pl_x; desired_z = pl_z; }
            else                  { desired_x = pr_x; desired_z = pr_z; }
            desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                           (desired_z < 0 ? -desired_z : desired_z);
            if (desired_dist == 0) desired_dist = 1;
            d->steer_timer--;
        }

        /* --- Smooth turning: blend new direction with previous (in facing) --- */
        int32_t move_x = (desired_x * ZMB_SPEED) / desired_dist;
        int32_t move_z = (desired_z * ZMB_SPEED) / desired_dist;
        int32_t prev_mx = (int16_t)(d->facing >> 16);
        int32_t prev_mz = (int16_t)(d->facing & 0xFFFF);
        int32_t blend_x = (prev_mx * (8 - ZMB_TURN_RATE) + move_x * ZMB_TURN_RATE) >> 3;
        int32_t blend_z = (prev_mz * (8 - ZMB_TURN_RATE) + move_z * ZMB_TURN_RATE) >> 3;
        d->facing = ((int32_t)(int16_t)blend_x << 16) | (uint16_t)(int16_t)blend_z;

        d->x += blend_x;
        d->z += blend_z;
        apply_flat_entity_collision(&d->x, &d->z, ZMB_BODY_RADIUS);
        crates_collide(&d->x, d->y, &d->z, 80);
        fatdoors_collide(&d->x, d->y, &d->z, ZMB_DOOR_CLEARANCE);
    }

    /* --- Zombie vs zombie hard collision (after every zombie has moved) --- */
    int a, b;
    for (a = 0; a < zombie_count; a++) {
        Zombie *za = &zombies[a];
        if (!za->active || za->state == ZMB_DEAD) continue;
        for (b = a + 1; b < zombie_count; b++) {
            Zombie *zb = &zombies[b];
            if (!zb->active || zb->state == ZMB_DEAD) continue;
            int32_t cdx  = za->x - zb->x;
            int32_t cdz  = za->z - zb->z;
            int32_t dist = (cdx < 0 ? -cdx : cdx) + (cdz < 0 ? -cdz : cdz);
            int32_t min_dist = ZMB_BODY_RADIUS * 2;
            if (dist < min_dist && dist > 0) {
                int32_t push    = (min_dist - dist) / 2;
                int32_t push_ax = (cdx * push) / dist;
                int32_t push_az = (cdz * push) / dist;
                za->x += push_ax; za->z += push_az;
                zb->x -= push_ax; zb->z -= push_az;
            }
        }
    }
}

/* Add an already-filled POLY_FT4 (whose packet space is already reserved via
   next_packet) to ot[otz]. If the area has a texture window active (the kitchen
   uses a 128x128 one), bracket the poly with a full/unmasked window and then a
   restore, all within this OT bucket, so our sprite/shadow sample their true
   VRAM region without disturbing the area's other textured polys. addPrim()
   prepends, so adding restore, poly, disable yields the draw order
   disable -> poly -> restore. */
static void add_ft4_windowed(RenderContext *ctx, int32_t otz, POLY_FT4 *poly) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;

    if (zmb_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &zmb_tw_restore);
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

static void draw_zmb_shadow(RenderContext *ctx, Zombie *d) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(DR_TPAGE) + sizeof(POLY_FT4) > buf_end) return;

    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((ZMB_SHADOW_W * rx) >> 12);
    int16_t dwz = (int16_t)((ZMB_SHADOW_W * rz) >> 12);

    int32_t fx  = isin(cam_rot);
    int32_t fz  = icos(cam_rot);
    int16_t ddx = (int16_t)((ZMB_SHADOW_D * fx) >> 12);
    int16_t ddz = (int16_t)((ZMB_SHADOW_D * fz) >> 12);

    int32_t shadow_y = d->y + ZMB_Y_OFFSET + ZMB_HALF_H - 2;

    SVECTOR sv[4];
    sv[0].vx = (int16_t)(d->x - dwx - ddx); sv[0].vy = (int16_t)shadow_y; sv[0].vz = (int16_t)(d->z - dwz - ddz); sv[0].pad = 0;
    sv[1].vx = (int16_t)(d->x + dwx - ddx); sv[1].vy = (int16_t)shadow_y; sv[1].vz = (int16_t)(d->z + dwz - ddz); sv[1].pad = 0;
    sv[2].vx = (int16_t)(d->x - dwx + ddx); sv[2].vy = (int16_t)shadow_y; sv[2].vz = (int16_t)(d->z - dwz + ddz); sv[2].pad = 0;
    sv[3].vx = (int16_t)(d->x + dwx + ddx); sv[3].vy = (int16_t)shadow_y; sv[3].vz = (int16_t)(d->z + dwz + ddz); sv[3].pad = 0;

    DVECTOR ssv[4];
    int32_t otz;

    gte_ldv0(&sv[0]); gte_rtps(); gte_stsxy(&ssv[0]);
    gte_ldv0(&sv[1]); gte_rtps(); gte_stsxy(&ssv[1]);
    gte_ldv0(&sv[2]); gte_rtps(); gte_stsxy(&ssv[2]);
    gte_ldv0(&sv[3]); gte_rtps(); gte_stsxy(&ssv[3]);

    gte_avsz4();
    gte_stotz(&otz);

    /* Sort just in front of the floor poly the shadow lies on. The room mesh
       sorts geometry at avgZ+40, so a small +2 keeps the shadow on top of the
       floor (and ~38 units of slack stops a large floor quad — whose averaged
       depth reads nearer than the zombie — from sorting over it), while nearer
       walls still occlude it. The sprite uses the raw avgZ, one step in front. */
    if (otz <= 0) return;
    otz += 2;
    if (otz >= OT_LENGTH - 2) otz = OT_LENGTH - 3;

    int32_t shadow_otz = otz;

    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 1, shadow_tpage);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[shadow_otz + 1], tp);
    ctx->next_packet += sizeof(DR_TPAGE);

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, 128, 128, 128);

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

    /* Reserve the poly before the window bracket may allocate DR_TWINs. */
    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, shadow_otz, poly);
}

static void draw_zmb_sprite(RenderContext *ctx, Zombie *d,
                            uint16_t tpage, uint16_t clut, uint8_t v_start, int flip) {
    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((ZMB_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((ZMB_HALF_W * rz) >> 12);

    int16_t vy_top = (int16_t)(d->y + ZMB_Y_OFFSET - ZMB_HALF_H);
    int16_t vy_bot = (int16_t)(d->y + ZMB_Y_OFFSET + ZMB_HALF_H);

    SVECTOR v[4];
    v[0].vx = d->x - dwx; v[0].vy = vy_top; v[0].vz = d->z - dwz; v[0].pad = 0;
    v[1].vx = d->x + dwx; v[1].vy = vy_top; v[1].vz = d->z + dwz; v[1].pad = 0;
    v[2].vx = d->x + dwx; v[2].vy = vy_bot; v[2].vz = d->z + dwz; v[2].pad = 0;
    v[3].vx = d->x - dwx; v[3].vy = vy_bot; v[3].vz = d->z - dwz; v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4];
    int32_t otz, nclip;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);

    gte_nclip();
    gte_stopz(&nclip);
    if (nclip <= 0) return;

    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);

    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return;

    gte_avsz4();
    gte_stotz(&otz);
    /* Sort on the room-geometry scale (raw average Z) so walls between the
       camera and the zombie correctly occlude the sprite, while the ~40-unit
       gap to the floor polys (which the mesh sorts at avgZ+40) keeps the sprite
       from sinking under the floor it stands on. One step in front of the
       shadow (which uses avgZ+2). */
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    int32_t fdx        = d->x - cam_x;
    int32_t fdz        = d->z - cam_z;
    int32_t dist       = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    int32_t fog_start  = 500;
    int32_t fog_end    = 3000;
    int32_t fog        = dist < fog_start ? fog_start : dist > fog_end ? fog_end : dist;
    int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);
    uint8_t fog8       = fog_factor > 255 ? 255 : (uint8_t)fog_factor;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, fog8, fog8, fog8);

    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

    /* Sprite is 64 wide x 128 tall. Inset the UVs by one texel on every edge so
       the magnified quad's edge pixels can't sample the neighbouring textures in
       VRAM. The row directly below the sleep frame is the demon dog's open-mouth
       texture, which otherwise bled in as a red/white strip along the bottom. */
    uint8_t u_left  = flip ? 62 : 1;
    uint8_t u_right = flip ?  1 : 62;
    poly->u0 = u_left;  poly->v0 = v_start + 1;
    poly->u1 = u_right; poly->v1 = v_start + 1;
    poly->u2 = u_left;  poly->v2 = v_start + 126;
    poly->u3 = u_right; poly->v3 = v_start + 126;

    poly->tpage = tpage;
    poly->clut  = clut;

    /* Reserve the sprite's packet space before the window bracket may allocate
       DR_TWINs, then add it (V 128..255 needs the kitchen's texture window
       disabled or it samples the wrong VRAM). */
    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, otz, poly);

    if (d->hit_timer <= 0) return;

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

    int16_t fill_w = (int16_t)((d->health * 40) / ZMB_MAX_HEALTH);
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

void draw_zombies(RenderContext *ctx) {
    int i;
    for (i = 0; i < zombie_count; i++) {
        Zombie *d = &zombies[i];
        if (!d->active || d->state == ZMB_DEAD) continue;

        int32_t dx = d->x - cam_x;
        int32_t dz = d->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 4000) continue;

        draw_zmb_shadow(ctx, d);

        if (d->state == ZMB_DORMANT) {
            draw_zmb_sprite(ctx, d, sleep_tpage, sleep_clut, 128, 0);
        } else {
            /* Flip the sprite when the zombie is to the player's right so it
               always faces toward the player. Camera right vector = (icos, -isin). */
            int32_t dot = ((dx >> 4) * (icos(cam_rot) >> 4))
                        - ((dz >> 4) * (isin(cam_rot) >> 4));
            int flip = dot <= 0;
            /* Alternate between the sprite and its mirror every half second
               (~30 frames) to fake a shambling, foot-over-foot walk. */
            if ((d->anim_tick / 30) & 1) flip = !flip;
            draw_zmb_sprite(ctx, d, alert_tpage, alert_clut, 128, flip);
        }
    }
}
