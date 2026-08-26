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
#include "spider.h"
#include "web.h"
#include "fatdoor.h"
#include "vines.h"   /* solid on the same terms; area-gated, so free elsewhere */
#include "texmgr.h"
#include "sound.h"

Spider spiders[MAX_SPIDERS];
int    spider_count = 0;

static uint16_t rest_tpage = 0, rest_clut = 0;
static uint16_t walk_tpage = 0, walk_clut = 0;
static uint16_t shadow_tpage = 0, shadow_clut = 0;

/* Texture window the current area expects, restored after each sprite (the
   same bracket the zombie uses — both sprites live at Voff >= 128 in VRAM, so
   a 128-tall window would wrap their V and sample the wrong region). Set by
   spiders_set_texwindow(); inactive by default. */
static RECT spd_tw_restore;
static int  spd_tw_active = 0;

void spiders_set_texwindow(const RECT *tw) {
    if (tw) { spd_tw_restore = *tw; spd_tw_active = 1; }
    else    { spd_tw_active = 0; }
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

/* texmgr ids for the two BODY sprites. They are registered rather than plainly
   loaded because their VRAM slots are now TIME-SHARED with the Rafflesia's two
   sprites (see rafflesia.c: there is no 128-row hole left in VRAM for a second
   pair, and the two enemies never share a room — spiders are house-interior,
   rafflesias are garden). The shadow is not shared and stays a plain load. */
static int rest_tex_id = -1, walk_tex_id = -1;

/* Load the sprite textures. Call ONCE at startup — a CdRead is only safe before
   the per-frame render loop begins (see tools/TEXTURING_NOTES.txt). The bodies
   are also uploaded here, so the spiders are correct from the first frame
   without waiting on a transition. */
void spiders_load_textures(void) {
    rest_tex_id = texmgr_register("\\SPDRRST.TIM;1");
    walk_tex_id = texmgr_register("\\SPDRWK.TIM;1");
    rest_tpage  = texmgr_tpage(rest_tex_id); rest_clut = texmgr_clut(rest_tex_id);
    walk_tpage  = texmgr_tpage(walk_tex_id); walk_clut = texmgr_clut(walk_tex_id);
    spiders_upload_textures();
    load_tim("\\SHADOW.TIM;1",  &shadow_tpage, &shadow_clut);
}

/* Re-stream the two body sprites into their VRAM slots. Pure LoadImage, so it is
   safe on a room transition and only there; main.c calls it on entry to every
   room that is not the Outside Catacombs, which is where the Rafflesias take
   the same two slots. */
void spiders_upload_textures(void) {
    texmgr_upload(rest_tex_id);
    texmgr_upload(walk_tex_id);
}

int spider_add_at(int32_t x, int32_t z, int32_t ceiling_y, GameState area) {
    if (spider_count >= MAX_SPIDERS) return -1;
    int i = spider_count++;
    Spider *s = &spiders[i];
    /* Hang the sprite's TOP edge on the ceiling, so the whole body hangs down
       into the room. The quad is drawn centred on y + SPD_Y_OFFSET and reaches
       SPD_HALF_H either side of that (the 180-degree roll spins it about that
       centre and so does not change its extent), which is why the half-height
       comes back off here — anchoring the centre on the ceiling instead buries
       the top half of the spider in the roof mesh. */
    int32_t y = ceiling_y - SPD_Y_OFFSET + SPD_HALF_H;
    *s = (Spider){0};
    s->x = x; s->y = y; s->z = z;
    s->spawn_x = x; s->spawn_y = y; s->spawn_z = z;
    s->health = SPD_MAX_HEALTH;
    s->state  = SPD_CEILING;
    s->active = 1;
    s->area   = area;
    return i;
}

int spider_add(int32_t x, int32_t z, GameState area) {
    return spider_add_at(x, z, collision_ceiling_y(x, z), area);
}

void spiders_rest(void) {
    int i;
    for (i = 0; i < spider_count; i++) {
        Spider *s = &spiders[i];
        if (!s->active || s->state == SPD_DEAD) continue;
        /* Rebuild exactly as spider_add left it, back on its ceiling perch. */
        int32_t sx = s->spawn_x, sy = s->spawn_y, sz = s->spawn_z;
        GameState area = s->area;
        *s = (Spider){0};
        s->x = sx; s->y = sy; s->z = sz;
        s->spawn_x = sx; s->spawn_y = sy; s->spawn_z = sz;
        s->health = SPD_MAX_HEALTH;
        s->state  = SPD_CEILING;
        s->active = 1;
        s->area   = area;
    }
}

/* Kill every living spider tagged to `area`, SILENTLY: no blood burst, no death
   cry, no wake. It is for a room being emptied by the story rather than by the
   player — the East Hall once FLAG_HADAD_TWO is set (east_hall.h) — and it runs
   during a transition, with nobody there to see or hear it.

   Marked dead, not deactivated. SPD_DEAD is what every loop in this file already
   skips, what spiders_rest() refuses to stand back up, and what world.c's save
   delta records (spiders_dead), so a spider retired this way is retired on
   exactly the same terms as one the player killed — including across a save. */
void spiders_kill_area(GameState area) {
    int i;
    for (i = 0; i < spider_count; i++) {
        Spider *s = &spiders[i];
        if (!s->active || s->area != area || s->state == SPD_DEAD) continue;
        s->health = 0;
        s->state  = SPD_DEAD;
    }
}

/* Shared scuttle loop latch, driven exactly like the tentacle writhe: ONE voice
   for every spider in the room, keyed on whether any of them actually moved this
   frame. Per-spider voices would be three copies of the same sample fighting
   over the SPU. File-scope rather than local to update_spiders so a room
   transition can clear it (spiders_silence). */
static int walk_on = 0;

/* Cut the scuttle loop dead and clear the latch. Clearing walk_on matters as
   much as the sound_stop: update_spiders only issues sound_play on the 0->1
   edge, so a stopped voice with the latch still set would stay silent forever
   once the player came back. Called on every room transition (world.h). */
void spiders_silence(void) {
    sound_stop(SFX_SPDR_WLK);
    walk_on = 0;
}

void spiders_init(void) {
    /* Placements are seeded on a room's first entry by the world system (see
       world_enter in world.c), where the room's collision mesh is loaded and
       spider_add can read its ceiling. The array starts empty. */
    spider_count = 0;
}

void spiders_reset(void) {
    spider_count = 0;
}

/* Drop off the ceiling. Seeding vy at terminal velocity makes the fall quick
   from the first frame rather than easing in under gravity. */
static void spider_wake(Spider *s) {
    if (s->state != SPD_CEILING) return;
    s->state      = SPD_DROPPING;
    s->drop_tick  = 0;
    s->vy         = SPD_DROP_VEL;
    /* Full cadence before the first web, so a spider that happens to land
       already inside the spit band does not open with an instant hit — the
       zeroed timer a fresh spider carries would otherwise fire on the frame it
       touches down. */
    s->spit_timer = SPD_SPIT_INTERVAL;
}

/* No weaknesses yet — add a { DMG_*, percent } line to give it one (damage.h). */
static const Weakness spider_weakness[] = {
    { DMG_KINETIC, 100 },   /* placeholder: 100% = no change. Replace or append. */
};

int32_t spider_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, spider_weakness,
                        WEAKNESS_COUNT(spider_weakness));
}

void spider_damage(Spider *s, int dmg) {
    if (!s->active || s->state == SPD_DEAD) return;
    spider_wake(s);                 /* shot off the ceiling: drop and fight */
    s->health   -= dmg;
    s->hit_timer = SPD_BAR_TIMER_MAX;
    if (s->health <= 0) {
        s->health = 0;
        s->state  = SPD_DEAD;
        spawn_blood_burst(s->x, s->y, s->z);
        sound_play(SFX_TNTCL_DIE);  /* no spider clip yet; shares the tentacle's
                                       death cry */
    } else {
        sound_play(SFX_AXEHIT);
    }
}

/* How far through the upside-down -> upright turn a dropping spider is, as an
   angle in PS1 units (SPD_ANGLE_HUNG = hanging, 0 = upright). */
static int32_t spider_angle(const Spider *s) {
    if (s->state == SPD_CEILING) return SPD_ANGLE_HUNG;
    if (s->state != SPD_DROPPING) return 0;
    if (s->drop_tick >= SPD_TURN_FRAMES) return 0;
    return ((int32_t)SPD_ANGLE_HUNG * (SPD_TURN_FRAMES - s->drop_tick))
           / SPD_TURN_FRAMES;
}

void update_spiders(void) {
    static int hurt_sfx_cooldown = 0;
    int any_walking = 0;
    int i;
    if (hurt_sfx_cooldown > 0) hurt_sfx_cooldown--;

    for (i = 0; i < spider_count; i++) {
        Spider *s = &spiders[i];
        if (!s->active || s->state == SPD_DEAD || s->area != current_area) continue;

        if (s->hit_timer    > 0) s->hit_timer--;
        if (s->damage_timer > 0) s->damage_timer--;
        if (s->state == SPD_ALERT) s->anim_tick++;

        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - s->x;
        int32_t dy = py - s->y;
        int32_t dz = pz - s->z;

        /* --- Asleep on the ceiling --- */
        if (s->state == SPD_CEILING) {
            /* A true radial test: SPD_WAKE_RADIUS really means a circle of that
               radius, not the Manhattan diamond the zombie's much coarser
               900-unit wake radius can get away with. Coordinates stay in the
               low thousands, so the squares fit an int32. */
            if (dx * dx + dz * dz <= (int32_t)SPD_WAKE_RADIUS * SPD_WAKE_RADIUS)
                spider_wake(s);
            else
                continue;           /* still, silent, upside down */
        }

        /* --- Falling: gravity settles it onto the floor, and the sprite turns
           upright on the way down (see spider_angle). apply_ddog_height zeroes
           vy the frame it clamps to the floor, which is the landing. --- */
        if (s->state == SPD_DROPPING) {
            s->drop_tick++;
            apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                              &s->on_upper_floor, &s->on_ramp);
            if (s->vy == 0) s->state = SPD_ALERT;
            continue;               /* no chasing or biting mid-air */
        }

        /* --- On the floor and hunting --- */
        apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                          &s->on_upper_floor, &s->on_ramp);

        if (s->kb_vx != 0 || s->kb_vz != 0) {
            s->x += s->kb_vx;
            s->z += s->kb_vz;
            apply_flat_entity_collision(&s->x, &s->z, SPD_BODY_RADIUS);
            crates_collide(&s->x, s->y, &s->z, 80);
            dining_tables_collide(&s->x, s->y, &s->z, 75);
            fatdoors_collide(&s->x, s->y, &s->z, SPD_DOOR_CLEARANCE);
            vines_collide(&s->x, s->y, &s->z, SPD_DOOR_CLEARANCE);
            if (s->kb_vx > 0) s->kb_vx =  (  s->kb_vx * 7) >> 3;
            else              s->kb_vx = -((-s->kb_vx * 7) >> 3);
            if (s->kb_vz > 0) s->kb_vz =  (  s->kb_vz * 7) >> 3;
            else              s->kb_vz = -((-s->kb_vz * 7) >> 3);
            continue;
        }

        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

        /* Bite on contact. Horizontal and vertical reach are checked separately
           for the same reason the zombie checks them separately: the player's
           eye sits above the body, and a combined budget stops a stopped-and-
           adjacent spider from ever landing a hit. */
        if (!game_over && dist2d < SPD_CATCH_DIST &&
            (dy < 0 ? -dy : dy) < SPD_CATCH_DIST && s->damage_timer == 0) {
            s->damage_timer = SPD_DAMAGE_COOLDOWN;
            player_hurt(SPD_DAMAGE_AMOUNT);
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

        if (dist2d < SPD_CATCH_DIST) continue;

        /* --- Pick a band: melee, kite, or close the gap ---
           Uses the TRUE radial distance, because the bands are specified as
           radii; the Manhattan dist2d above is a reach test and stays as it is.
           Squares stay well inside an int32 at these ranges. */
        int32_t rad2      = dx * dx + dz * dz;
        int     pursuing  = 1;              /* 0 = backing away from the player */
        int32_t band_speed = SPD_SPEED;

        if (rad2 <= (int32_t)SPD_ATTACK_RADIUS * SPD_ATTACK_RADIUS) {
            /* Melee band: close in and bite, at full speed, holding fire. */
            s->spit_timer = SPD_SPIT_INTERVAL;   /* re-arm for the next kite */
        } else {
            /* Anywhere beyond the attack radius the spider spits, whatever its
               feet are doing — backing off inside the stand-off distance, or
               running the gap down from outside it. Firing needs a clear
               sightline, so a spider cannot spit through a wall it happens to
               be standing behind. */
            if (rad2 <= (int32_t)SPD_SPIT_RADIUS * SPD_SPIT_RADIUS) {
                pursuing   = 0;                  /* give ground and kite */
                band_speed = SPD_RETREAT_SPEED;
            }
            if (--s->spit_timer <= 0) {
                s->spit_timer = SPD_SPIT_INTERVAL;
                if (!collision_segment_blocked(s->x, s->y + SPD_Y_OFFSET, s->z,
                                               px, py, pz)) {
                    web_spawn(s->x, s->y + SPD_Y_OFFSET, s->z, px, py, pz,
                              s->area);
                    sound_play(SFX_SPIT);   /* one shot per web actually fired */
                }
            }
        }

        /* The direction we WANT to travel: at the player, or directly away. */
        int32_t goal_dx = pursuing ?  dx : -dx;
        int32_t goal_dz = pursuing ?  dz : -dz;

        /* --- Separation: soft push away from nearby spiders --- */
        int32_t sep_x = 0, sep_z = 0;
        int j;
        for (j = 0; j < spider_count; j++) {
            if (j == i) continue;
            Spider *other = &spiders[j];
            if (!other->active || other->state == SPD_DEAD ||
                other->area != current_area) continue;
            int32_t odx   = s->x - other->x;
            int32_t odz   = s->z - other->z;
            int32_t odist = (odx < 0 ? -odx : odx) + (odz < 0 ? -odz : odz);
            if (odist < SPD_SEP_RADIUS && odist > 0) {
                int32_t push = SPD_SEP_RADIUS - odist;
                sep_x += (odx * push) / odist;
                sep_z += (odz * push) / odist;
            }
        }

        /* --- Desired direction: along the band's goal, biased by separation --- */
        int32_t desired_x = goal_dx + sep_x * SPD_SEP_WEIGHT;
        int32_t desired_z = goal_dz + sep_z * SPD_SEP_WEIGHT;
        int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                               (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;

        /* --- Obstacle feeler: probe ahead in the desired direction --- */
        int32_t feeler_x = s->x + (desired_x * SPD_FEELER_LEN) / desired_dist;
        int32_t feeler_z = s->z + (desired_z * SPD_FEELER_LEN) / desired_dist;
        int32_t fx = feeler_x, fz = feeler_z;
        crates_collide(&fx, s->y, &fz, 80);
        dining_tables_collide(&fx, s->y, &fz, 75);
        apply_flat_entity_collision(&fx, &fz, SPD_BODY_RADIUS);
        int blocked = (fx != feeler_x || fz != feeler_z);
        /* Clear sightline: charge straight. The feeler's point+radius test also
           trips merely walking NEAR a wall, and wall-following on that deadlocks
           in concave corners; the real move's own collision push slides the body
           along anything it grazes.
           ONLY while pursuing: the sightline runs toward the player, so it says
           nothing about what is behind a retreating spider. Trusting it while
           backing off would switch off wall-following exactly when the spider is
           reversing into a wall it cannot see. */
        if (pursuing &&
            !collision_segment_blocked(s->x, s->y, s->z, px, py, pz)) {
            blocked        = 0;
            s->steer_timer = 0;
        }

        /* Perpendiculars to the travel direction — the two ways to slide
           along a wall. */
        int32_t pl_x = -goal_dz, pl_z =  goal_dx;   /* left  */
        int32_t pr_x =  goal_dz, pr_z = -goal_dx;   /* right */

        /* Where "making progress" points: the player when closing in, the spot
           directly opposite them when giving ground. */
        int32_t goal_px = s->x + goal_dx;
        int32_t goal_pz = s->z + goal_dz;

        if (blocked && s->steer_timer <= 0) {
            /* Newly blocked: probe BOTH sides and commit to one for a while so
               the spider follows the wall instead of flip-flopping. */
            int32_t pl_dist = (pl_x < 0 ? -pl_x : pl_x) + (pl_z < 0 ? -pl_z : pl_z);
            int32_t pr_dist = (pr_x < 0 ? -pr_x : pr_x) + (pr_z < 0 ? -pr_z : pr_z);
            if (pl_dist == 0) pl_dist = 1;
            if (pr_dist == 0) pr_dist = 1;

            int32_t lx = s->x + (pl_x * SPD_FEELER_LEN) / pl_dist;
            int32_t lz = s->z + (pl_z * SPD_FEELER_LEN) / pl_dist;
            int32_t rx = s->x + (pr_x * SPD_FEELER_LEN) / pr_dist;
            int32_t rz = s->z + (pr_z * SPD_FEELER_LEN) / pr_dist;

            int32_t tlx = lx, tlz = lz;
            crates_collide(&tlx, s->y, &tlz, 80);
            dining_tables_collide(&tlx, s->y, &tlz, 75);
            apply_flat_entity_collision(&tlx, &tlz, SPD_BODY_RADIUS);
            int left_blocked = (tlx != lx || tlz != lz);

            int32_t trx = rx, trz = rz;
            crates_collide(&trx, s->y, &trz, 80);
            dining_tables_collide(&trx, s->y, &trz, 75);
            apply_flat_entity_collision(&trx, &trz, SPD_BODY_RADIUS);
            int right_blocked = (trx != rx || trz != rz);

            if (left_blocked && !right_blocked) {
                s->steer_dir = +1;
            } else if (right_blocked && !left_blocked) {
                s->steer_dir = -1;
            } else {
                /* Both open (or both blocked): take the side whose probe ends
                   closer to where this band wants to go. */
                int32_t ld = (goal_px - lx < 0 ? lx - goal_px : goal_px - lx) +
                             (goal_pz - lz < 0 ? lz - goal_pz : goal_pz - lz);
                int32_t rd = (goal_px - rx < 0 ? rx - goal_px : goal_px - rx) +
                             (goal_pz - rz < 0 ? rz - goal_pz : goal_pz - rz);
                s->steer_dir = (ld <= rd) ? -1 : +1;
            }
            s->steer_timer = SPD_STEER_COMMIT;
        }

        if (s->steer_timer > 0) {
            if (s->steer_dir < 0) { desired_x = pl_x; desired_z = pl_z; }
            else                  { desired_x = pr_x; desired_z = pr_z; }
            desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                           (desired_z < 0 ? -desired_z : desired_z);
            if (desired_dist == 0) desired_dist = 1;
            s->steer_timer--;
        }

        /* --- Smooth turning: blend the new direction with the previous one --- */
        int32_t move_x  = (desired_x * band_speed) / desired_dist;
        int32_t move_z  = (desired_z * band_speed) / desired_dist;
        int32_t prev_mx = (int16_t)(s->facing >> 16);
        int32_t prev_mz = (int16_t)(s->facing & 0xFFFF);
        int32_t blend_x = (prev_mx * (8 - SPD_TURN_RATE) + move_x * SPD_TURN_RATE) >> 3;
        int32_t blend_z = (prev_mz * (8 - SPD_TURN_RATE) + move_z * SPD_TURN_RATE) >> 3;
        s->facing = ((int32_t)(int16_t)blend_x << 16) | (uint16_t)(int16_t)blend_z;

        /* Anything actually travelling this frame keeps the scuttle going. The
           blend is integer-divided, so a spider pinned against a wall can end up
           with a zero step and correctly falls silent. */
        if (blend_x != 0 || blend_z != 0) any_walking = 1;

        s->x += blend_x;
        s->z += blend_z;
        apply_flat_entity_collision(&s->x, &s->z, SPD_BODY_RADIUS);
        crates_collide(&s->x, s->y, &s->z, 80);
        dining_tables_collide(&s->x, s->y, &s->z, 75);
        fatdoors_collide(&s->x, s->y, &s->z, SPD_DOOR_CLEARANCE);
        vines_collide(&s->x, s->y, &s->z, SPD_DOOR_CLEARANCE);
    }

    /* Start/stop the shared loop. Forced off on game-over, which stops the area
       update running and would otherwise leave the scuttle keyed on forever —
       the same guard the tentacle writhe needs. */
    if (game_over) any_walking = 0;
    if (any_walking && !walk_on)       { sound_play(SFX_SPDR_WLK); walk_on = 1; }
    else if (!any_walking && walk_on)  { sound_stop(SFX_SPDR_WLK); walk_on = 0; }

    /* --- Spider vs spider hard collision (after every spider has moved) --- */
    int a, b;
    for (a = 0; a < spider_count; a++) {
        Spider *sa = &spiders[a];
        if (!sa->active || sa->state != SPD_ALERT || sa->area != current_area) continue;
        for (b = a + 1; b < spider_count; b++) {
            Spider *sb = &spiders[b];
            if (!sb->active || sb->state != SPD_ALERT || sb->area != current_area) continue;
            int32_t cdx  = sa->x - sb->x;
            int32_t cdz  = sa->z - sb->z;
            int32_t dist = (cdx < 0 ? -cdx : cdx) + (cdz < 0 ? -cdz : cdz);
            int32_t min_dist = SPD_BODY_RADIUS * 2;
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

/* Bracket an already-filled POLY_FT4 with a full/unmasked texture window and a
   restore of the area's own window, all within one OT bucket. Identical to the
   zombie's add_ft4_windowed and needed for the same reason (both spider sprites
   sit at Voff >= 128). addPrim() prepends, so adding restore, poly, disable
   yields the draw order disable -> poly -> restore. */
static void add_ft4_windowed(RenderContext *ctx, int32_t otz, POLY_FT4 *poly) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;

    if (spd_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &spd_tw_restore);
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

static void draw_spd_shadow(RenderContext *ctx, Spider *s) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(DR_TPAGE) + sizeof(POLY_FT4) > buf_end) return;

    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((SPD_SHADOW_W * rx) >> 12);
    int16_t dwz = (int16_t)((SPD_SHADOW_W * rz) >> 12);

    int32_t fx  = isin(cam_rot);
    int32_t fz  = icos(cam_rot);
    int16_t ddx = (int16_t)((SPD_SHADOW_D * fx) >> 12);
    int16_t ddz = (int16_t)((SPD_SHADOW_D * fz) >> 12);

    int32_t shadow_y = s->y + SPD_Y_OFFSET + SPD_HALF_H - 2;

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

    /* Sort just in front of the floor poly the shadow lies on (see the zombie's
       shadow for the +2 / avgZ+40 reasoning). */
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

    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, shadow_otz, poly);
}

/* Draw the body quad. `angle` rolls it in its own plane (SPD_ANGLE_HUNG = fully
   inverted, 0 = upright), which is what sells hanging from the ceiling and the
   turn on the way down. The camera-facing billboard is built from the view's
   right axis and world Y as before; the rotation is applied to the corner
   offsets in that (right, down) plane before they are placed in the world. */
static void draw_spd_sprite(RenderContext *ctx, Spider *s,
                            uint16_t tpage, uint16_t clut, int32_t angle, int flip) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);
    int32_t ca = icos(angle);
    int32_t sa = isin(angle);

    int32_t cx = s->x, cz = s->z, cy = s->y + SPD_Y_OFFSET;

    /* Corner offsets in sprite space (u along the view's right axis, v down),
       rolled by `angle`, then pushed out into the world. */
    static const int32_t corner[4][2] = {
        { -SPD_HALF_W, -SPD_HALF_H },   /* top-left     */
        {  SPD_HALF_W, -SPD_HALF_H },   /* top-right    */
        {  SPD_HALF_W,  SPD_HALF_H },   /* bottom-right */
        { -SPD_HALF_W,  SPD_HALF_H },   /* bottom-left  */
    };

    SVECTOR v[4];
    int c;
    for (c = 0; c < 4; c++) {
        int32_t u = corner[c][0], d = corner[c][1];
        int32_t ru = (u * ca - d * sa) >> 12;
        int32_t rd = (u * sa + d * ca) >> 12;
        v[c].vx  = (int16_t)(cx + ((ru * rx) >> 12));
        v[c].vy  = (int16_t)(cy + rd);
        v[c].vz  = (int16_t)(cz + ((ru * rz) >> 12));
        v[c].pad = 0;
    }

    DVECTOR sv[4];
    int32_t sz[4];
    int32_t otz;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);

    /* No nclip backface cull here (the zombie's would throw the sprite away the
       moment the roll passes 90 degrees and reverses the winding). The quad is
       always camera-facing, so there is no back face to cull. */

    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);

    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return;

    gte_avsz4();
    gte_stotz(&otz);
    /* Sort on the room-geometry scale (raw average Z) so walls between the
       camera and the spider occlude it, while the ~40-unit gap to the floor
       polys (which the mesh sorts at avgZ+40) keeps it off the floor. */
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    int32_t fdx  = s->x - cam_x;
    int32_t fdz  = s->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    /* Fog to match the room mesh; beyond fog_far it is fully fogged, so cull. */
    if (dist >= g_fog_far) return;
    int32_t fs   = render_fog_scale(dist);
    uint8_t fog8 = fs > 255 ? 255 : (uint8_t)fs;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, fog8, fog8, fog8);

    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

    /* Sprite is 128x128 at VRAM y=128, so V runs 128..255. Inset the UVs by one
       texel on every edge so the magnified quad's edge pixels can't sample the
       neighbouring textures in VRAM (the zombie hit exactly this as a red strip
       bled in from the row below). */
    uint8_t u_left  = flip ? 126 : 1;
    uint8_t u_right = flip ?   1 : 126;
    poly->u0 = u_left;  poly->v0 = 129;
    poly->u1 = u_right; poly->v1 = 129;
    poly->u2 = u_left;  poly->v2 = 254;
    poly->u3 = u_right; poly->v3 = 254;

    poly->tpage = tpage;
    poly->clut  = clut;

    /* Reserve the sprite's packet space before the window bracket may allocate
       DR_TWINs, then add it. */
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

    int16_t fill_w = (int16_t)((s->health * 40) / SPD_MAX_HEALTH);
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

void draw_spiders(RenderContext *ctx) {
    int i;
    for (i = 0; i < spider_count; i++) {
        Spider *s = &spiders[i];
        if (!s->active || s->state == SPD_DEAD || s->area != current_area) continue;

        int32_t dx = s->x - cam_x;
        int32_t dz = s->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 4000) continue;

        /* Only a grounded spider casts a floor shadow — one drawn under a
           hanging or falling body would sit at its feet in mid-air. */
        if (s->state == SPD_ALERT) draw_spd_shadow(ctx, s);

        if (s->state == SPD_ALERT) {
            /* Flip the sprite when the spider is to the player's right so it
               always faces toward them. Camera right vector = (icos, -isin). */
            int32_t dot = ((dx >> 4) * (icos(cam_rot) >> 4))
                        - ((dz >> 4) * (isin(cam_rot) >> 4));
            int flip = dot <= 0;
            /* Oscillate between the two textures to fake a scuttle — the
               spider's equivalent of the zombie's alternating walk frames. */
            if ((s->anim_tick / SPD_ANIM_RATE) & 1)
                draw_spd_sprite(ctx, s, walk_tpage, walk_clut, 0, flip);
            else
                draw_spd_sprite(ctx, s, rest_tpage, rest_clut, 0, flip);
        } else {
            /* Hung or falling: the rest texture, rolled by the drop turn. */
            draw_spd_sprite(ctx, s, rest_tpage, rest_clut, spider_angle(s), 0);
        }
    }
}
