#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "collision.h"   /* apply_ddog_height + GROUND_FLOOR_Y. NOT the wall or
                            prop collision routines — a creep is a ghost and
                            touches none of them; see creep.h. */
#include "particles.h"
#include "texmgr.h"
#include "sound.h"       /* SFX_HURT / SFX_DIE — the PLAYER's sounds, not the
                            creep's. The creep itself is silent; see the note in
                            lms_creep_damage(). */
#include "crucifaxe.h"   /* SWING_RANGE, for lms_creeps_try_hit */
#include "lms_creep.h"

/* The Lamashtu Creep - see lms_creep.h for the four ways it differs from
   src/creep.c, which this is otherwise a copy of, and creep.h for everything
   the two share. */

LmsCreep lms_creeps[MAX_LMS_CREEPS];
int   lms_creep_count = 0;

/* THE CRY's clock: frames until it is re-keyed; 0 = not playing. */
static int lcr_cry_timer = 0;

/* The body sprite. A DEFERRED texmgr registration, like every other piece of
   Chapter 3 art: the TIM header is read at boot and the pixels only arrive when
   TEXBANK_CATACOMBS is selected at the catacomb mouth (src/texmgr.h), so
   Chapters 1 and 2 pay nothing for a monster they never meet.

   >>> IT IS THE 71st REGISTRATION OF 72. <<< TEXMGR_MAX in src/texmgr.c is the
   cap and texmgr_register past it returns -1 SILENTLY, which breaks that
   texture in every room that draws it. ONE LEFT. Count with
   py tools/heap_budget.py before adding the seventy-second. */
static int lms_creep_tex = -1;

/* >>> A CREEP CASTS NO SHADOW, AND IT IS THE ONLY ENEMY IN THE GAME THAT DOES
   NOT. <<< Every other body in src/ loads the shared SHADOW.TIM and lays a floor
   decal under itself; this one FLOATS at eye level (see creep.h) and never
   touches the ground, so a hard-edged ellipse on the flagstones read as a body
   standing where no body was.

   THE WHOLE APPARATUS WENT, NOT JUST THE CALL SITE: the decal function, the two
   LCR_SHADOW_* plan extents, the shadow_tpage/shadow_clut pair, the SHADOW.TIM
   read at boot AND this module's private load_tim() helper, which existed only
   to serve that one read. A dead LoadImage is still a CD read every boot and a
   pair of statics nothing writes; leaving them behind would have been the more
   expensive tidy. Note the helper is per-module by convention in this codebase,
   so removing creep.c's copy takes nothing away from any other enemy - they all
   still load the shared shadow through their own.

   IT IS NOT A LEVER TO PUT BACK LIGHTLY. The decal was also the only thing that
   grounded a creep in plan, so if one ever becomes hard to locate on the floor
   the honest fix is the HOVER HEIGHT or the sprite - not a shadow under a thing
   that is not standing on anything. git holds the removed function and helper if
   a grounded relative of this enemy ever wants them. */

/* The area's texture window, stored and not read — see lms_creeps_set_texwindow()
   in creep.h for why it exists anyway. */
static RECT lcr_tw_restore;
static int  lcr_tw_active = 0;

void lms_creeps_set_texwindow(const RECT *tw) {
    if (tw) { lcr_tw_restore = *tw; lcr_tw_active = 1; }
    else    { lcr_tw_active = 0; }
}

void lms_creeps_load_textures(void) {
    /* BANK: Chapter 3 and nothing else. Derived, not guessed — py
       tools/check_tex_banks.py walks the uploader call graph (this module is
       reached from room_of_arms_upload_textures) and fails the build if this
       mask is short. Widen it the day a crib stands outside the Catacombs. */
    texmgr_set_bank(TEXBANK_CATACOMBS);
    lms_creep_tex = texmgr_register("\\TEXCTCMB\\LMSCREEP.TIM;1");
}

void lms_creeps_upload_texture(void) {
    texmgr_upload(lms_creep_tex);
}

void lms_creeps_init(void)  { lms_creep_count = 0; }
void lms_creeps_reset(void) {
    lms_creep_count = 0;
    if (lcr_cry_timer > 0) sound_stop(SFX_CREEP);
    lcr_cry_timer = 0;
}

int lms_creep_spawn(int32_t x, int32_t z, int32_t anchor_y, int32_t emerge_y,
                GameState area) {
    /* A DEAD SLOT FIRST (lms_creep.h): she pours them all fight long. */
    int i;
    for (i = 0; i < lms_creep_count; i++)
        if (!lms_creeps[i].active || lms_creeps[i].state == LCR_DEAD) break;
    if (i == lms_creep_count) {
        if (lms_creep_count >= MAX_LMS_CREEPS) return -1;
        lms_creep_count++;
    }
    LmsCreep *c = &lms_creeps[i];

    *c = (LmsCreep){0};
    c->x = x; c->z = z;
    /* THE ANCHOR, not the corner. apply_ddog_height will hold it here from the
       next frame on; seeding it from the corner instead is the two-heights bug
       creep.h describes at length, and only half the spawn points survive it. */
    c->y = anchor_y;
    /* This body's hover height, latched for life. rand() % (2v+1) - v, so the
       range is symmetric about the nominal eye offset and includes it. */
    c->float_off = (int32_t)((uint32_t)rand() % (2u * LCR_FLOAT_VARY + 1u))
                 - LCR_FLOAT_VARY;
    /* Where the BODY starts: the corner, in world y. Only lms_creep_body_y() reads
       it, and only while the state is LCR_EMERGING. */
    c->emerge_y = emerge_y;
    c->emerge   = LCR_EMERGE_FRAMES;
    c->health   = LCR_MAX_HEALTH;
    c->state    = LCR_EMERGING;
    c->active   = 1;
    c->area     = area;
    return i;
}

int32_t lms_creep_body_y(const LmsCreep *c) {
    int32_t target = c->y + LCR_Y_OFFSET + c->float_off;
    if (c->state == LCR_EMERGING) {
        /* Linear from the corner to the float height. `emerge` counts DOWN, so
           the elapsed frames are the complement — and it is clamped rather than
           trusted, because a body drawn on the frame it was spawned has not been
           ticked yet and a body one frame past the end must not overshoot. */
        int32_t t = LCR_EMERGE_FRAMES - c->emerge;
        if (t < 0) t = 0;
        if (t > LCR_EMERGE_FRAMES) t = LCR_EMERGE_FRAMES;
        return c->emerge_y + ((target - c->emerge_y) * t) / LCR_EMERGE_FRAMES;
    }
    return target;
}

int lms_creeps_alive_in(GameState area) {
    int i, n = 0;
    for (i = 0; i < lms_creep_count; i++) {
        LmsCreep *c = &lms_creeps[i];
        if (c->active && c->state != LCR_DEAD && c->area == area) n++;
    }
    return n;
}

/* No weaknesses, by design — the brief says so in as many words. The empty
   placeholder table is kept so that giving it one later is an appended line
   rather than a new code path (damage.h). */
static const Weakness lms_creep_weakness[] = {
    { DMG_KINETIC, 100 },   /* 100% = no change. Replace or append. */
};

int32_t lms_creep_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, lms_creep_weakness,
                        WEAKNESS_COUNT(lms_creep_weakness));
}

/* Every one alive in `area` dies as if struck - its blood burst and all.
   Lamashtu calls this on each hit she takes (src/lamashtu.h); the cry stops by
   itself on the next update, with nothing left in reach. */
void lms_creeps_kill_all(GameState area) {
    int i;
    for (i = 0; i < lms_creep_count; i++)
        if (lms_creeps[i].area == area)
            lms_creep_damage(&lms_creeps[i], LCR_MAX_HEALTH);
}

void lms_creep_damage(LmsCreep *c, int dmg) {
    if (!c->active || c->state == LCR_DEAD) return;
    c->health -= dmg;
    if (c->health <= 0) {
        /* The burst goes where the BODY was, not where its floor anchor is, and
           it is read BEFORE the state changes: lms_creep_body_y() answers the
           EMERGENCE interpolation while the state is LCR_EMERGING, so one killed
           halfway out of the cot bleeds where it was rather than where it was
           heading. */
        int32_t body_y = lms_creep_body_y(c);
        c->health = 0;
        c->state  = LCR_DEAD;
        spawn_blood_burst(c->x, body_y, c->z);
        /* NO SOUND. Deliberate and temporary: the SPU is within ~16 KB of full
           (tools/ADDING_A_SOUND.txt) and ten deaths in thirty seconds is the
           worst possible thing to spend a voice on before the clip exists. Give
           it an SfxID here the day one does. */
    }
}

int lms_creeps_try_hit(void) {
    int i;
    for (i = 0; i < lms_creep_count; i++) {
        LmsCreep *c = &lms_creeps[i];
        if (!c->active || c->state == LCR_DEAD || c->area != current_area) continue;

        /* Reach to the body's CENTRE through lms_creep_body_y(), not to the floor
           anchor: the body hovers at eye level and the anchor is 22 below the
           player's eye plus whatever this one's float_off is, so measuring to the
           anchor would aim the swing at the floor under the thing. This is
           tools/ADDING_AN_ENEMY.txt's "test the reach the enemy's own geometry
           asks for", and it is the reason this test lives in the module rather
           than inlined in crucifaxe.c beside the zombie's.

           The float makes this EASIER than it was, incidentally: dy is now
           tens of units rather than the 145 it was when the body sat on the
           floor, so more of the Manhattan budget is left for real distance. */
        int32_t dx     = c->x - cam_x;
        int32_t dy     = lms_creep_body_y(c) - cam_y;
        int32_t dz     = c->z - cam_z;
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
        /* Plus the body's own half-extent, so the reach is to its SURFACE. */
        if (dist3d >= SWING_RANGE + LCR_HALF_W) continue;

        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;

        /* No knockback. One hit kills, so there is nothing left to shove. */
        lms_creep_damage(c, 1);
        return 1;
    }
    return 0;
}


void update_lms_creeps(void) {
    static int hurt_sfx_cooldown = 0;
    int i, biting = 0;
    if (hurt_sfx_cooldown > 0) hurt_sfx_cooldown--;

    for (i = 0; i < lms_creep_count; i++) {
        LmsCreep *c = &lms_creeps[i];
        /* current_area, NEVER game_state: they are the same value during
           ordinary play and differ the moment the inventory menu opens, and
           gating on game_state would let the player pause a swarm with Start
           (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!c->active || c->state == LCR_DEAD || c->area != current_area) continue;

        if (c->damage_timer > 0) c->damage_timer--;

        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - c->x;
        int32_t dz = pz - c->z;

        /* THE ANCHOR IS KEPT ON THE FLOOR EVERY FRAME, in both states. The body
           hovers, but c->y is still the floor probe's answer — that is what keeps
           the ramps and the multi-storey zones working with no second height
           model (creep.h). The drop shadow used to be the other thing that rode
           on it and this enemy no longer has one, so the HOVER HEIGHT is now the
           only consumer; the probe is still load-bearing for that alone. vy stays
           at zero because nothing here falls; the call is doing the floor CLAMP,
           not gravity. */
        apply_ddog_height(&c->x, &c->y, &c->z, &c->vy,
                          &c->on_upper_floor, &c->on_ramp);

        /* The vertical gap to the player, measured BODY to eye. Not anchor to
           eye: the two heights come from different functions and the body is the
           one the player is looking at (creep.h, and mistake 14 in the runbook). */
        int32_t dy = py - lms_creep_body_y(c);

        /* --- Coming out of the cot: hold position while the body rises or
           settles into its float height. No gravity, so there is no landing to
           detect — it is a countdown, and lms_creep_body_y() reads it. --- */
        if (c->state == LCR_EMERGING) {
            if (--c->emerge <= 0) {
                c->emerge = 0;
                c->state  = LCR_HUNTING;
            }
            continue;              /* no hunting or biting on the way out */
        }

        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

        /* --- Bite. Horizontal and vertical reach are tested SEPARATELY, not as
           one |dx|+|dz|+|dy| budget. That bug has now been fixed three times in
           this codebase; do not merge these two tests. It matters less than it
           used to here — a floating creep's body is at the player's eye, so dy
           is tens of units rather than the 145 it was when the body sat on the
           floor — but the separate tests are also what stops a creep biting
           across a storey, which is the OTHER thing the vertical term is for.

           >>> AND NOTHING CAPS THE TOTAL ACROSS CREEPS. <<< Each body runs its
           own damage_timer, so a ring of ten deals ten times 5 hp every 12
           frames and empties a full 100 in two seconds. That is the encounter's
           actual difficulty and it is intended: a creep is trivially killable
           and the threat is arithmetic, not any one of them. If this ever needs
           softening, soften it HERE with an explicit whole-swarm budget rather
           than by slowing the spawn cadence, which is the crib's timing and
           belongs to the prop. */
        if (!game_over && dist2d < LCR_CATCH_DIST &&
            (dy < 0 ? -dy : dy) < LCR_CATCH_DIST)
            biting = 1;   /* in reach: the cry below keys on any of them */
        if (!game_over && dist2d < LCR_CATCH_DIST &&
            (dy < 0 ? -dy : dy) < LCR_CATCH_DIST && c->damage_timer == 0) {
            c->damage_timer = LCR_DAMAGE_TICK;
            player_hurt(LCR_DAMAGE_AMOUNT);
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

        /* --- Hold station a little in front of the player. A TRUE radius, so
           the ring is round from every bearing; the Manhattan dist2d above is a
           reach test and stays Manhattan to match the sprite's footprint.
           Squares stay well inside an int32 at these ranges. --- */
        if (dx * dx + dz * dz <= (int32_t)LCR_STOP_DIST * LCR_STOP_DIST) continue;

        /* --- Separation: a soft push away from nearby creeps, and the ONLY
           thing that stops ten bodies homing on one point from becoming one
           sprite with nine hidden inside it (creep.h). --- */
        int32_t sep_x = 0, sep_z = 0;
        int j;
        for (j = 0; j < lms_creep_count; j++) {
            if (j == i) continue;
            LmsCreep *other = &lms_creeps[j];
            if (!other->active || other->state == LCR_DEAD ||
                other->area != current_area) continue;
            int32_t odx   = c->x - other->x;
            int32_t odz   = c->z - other->z;
            int32_t odist = (odx < 0 ? -odx : odx) + (odz < 0 ? -odz : odz);
            if (odist < LCR_SEP_RADIUS && odist > 0) {
                int32_t push = LCR_SEP_RADIUS - odist;   /* stronger when closer */
                sep_x += (odx * push) / odist;
                sep_z += (odz * push) / odist;
            }
        }

        /* --- THE STEP. Straight at the player, biased by separation, and that
           is the entire movement model.

           >>> NO FEELER, NO WALL-FOLLOW, NO SIGHTLINE TEST, AND NO COLLISION
           CALL OF ANY KIND AFTER IT. <<< A creep is a GHOST: it passes through
           the room's walls, its props and its doors. What used to be here was
           the spider's local steering layer — a feeler probe, a committed
           wall-follow side, and a collision_segment_blocked() shortcut — and
           every line of it existed to get a body along a surface this one does
           not notice. It is deleted, not disabled. creep.h says so at the top so
           that a reader arriving from any other enemy in src/ does not read the
           absence as an omission.

           There is no heading blend either, and that was already true before the
           lms_creeps became ghosts: see creep.h on tools/ADDING_AN_ENEMY.txt mistake
           16. So the step is always exactly one gait in the direction chosen
           this frame. */
        int32_t desired_x = dx + sep_x * LCR_SEP_WEIGHT;
        int32_t desired_z = dz + sep_z * LCR_SEP_WEIGHT;
        int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                               (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;

        c->x += (desired_x * LCR_SPEED) / desired_dist;
        c->z += (desired_z * LCR_SPEED) / desired_dist;
    }

    /* --- LmsCreep vs creep hard separation, AFTER every body has moved. The soft
       push above biases the steering; this stops two of them occupying the same
       point once they have all stopped on the ring. --- */
    int a, b;
    for (a = 0; a < lms_creep_count; a++) {
        LmsCreep *ca = &lms_creeps[a];
        if (!ca->active || ca->state != LCR_HUNTING ||
            ca->area != current_area) continue;
        for (b = a + 1; b < lms_creep_count; b++) {
            LmsCreep *cb = &lms_creeps[b];
            if (!cb->active || cb->state != LCR_HUNTING ||
                cb->area != current_area) continue;
            int32_t cdx  = ca->x - cb->x;
            int32_t cdz  = ca->z - cb->z;
            int32_t dist = (cdx < 0 ? -cdx : cdx) + (cdz < 0 ? -cdz : cdz);
            int32_t min_dist = LCR_BODY_RADIUS * 2;
            if (dist < min_dist && dist > 0) {
                int32_t push    = (min_dist - dist) / 2;
                int32_t push_ax = (cdx * push) / dist;
                int32_t push_az = (cdz * push) / dist;
                ca->x += push_ax; ca->z += push_az;
                cb->x -= push_ax; cb->z -= push_az;
            }
        }
    }

    /* THE CRY: on a loop while ANY of them is biting, re-keyed on the frame
       the clip ends; cut the moment none is. */
    if (biting) {
        if (lcr_cry_timer <= 0 || --lcr_cry_timer == 0) {
            sound_play(SFX_CREEP);
            lcr_cry_timer = LCR_CRY_FRAMES;
        }
    } else if (lcr_cry_timer > 0) {
        sound_stop(SFX_CREEP);
        lcr_cry_timer = 0;
    }
}

/* The body quad: a camera-facing billboard, the zombie's construction with the
   roll dropped (nothing rolls a creep) and the health bar dropped (one HP —
   creep.h says why).

   NO gte_nclip() BACKFACE CULL. A camera-facing quad has no back face, and the
   cull is what threw the spider away the moment its roll passed 90 degrees
   (tools/ADDING_AN_ENEMY.txt mistake 4). Nothing here rolls, but the cull would
   still be dead weight guarding against a case that cannot arise.

   NO TEXTURE-WINDOW BRACKET EITHER, and that is a property of the SLOT rather
   than of the sprite: creep.tim sits at VRAM x960 y0, which is Voff 0 and an
   exact tpage boundary, so U runs 0..63 and V runs 0..63 and a 128x128 window
   wraps neither. disc.xml spells this out beside the file. Move the texture and
   the bracket comes back — copy spider.c's add_ft4_windowed() when you do, and
   read lcr_tw_restore, which lms_creeps_set_texwindow() is already filling in. */
static void draw_lcr_sprite(RenderContext *ctx, LmsCreep *c, int flip) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);

    int32_t cx = c->x, cz = c->z, cy = lms_creep_body_y(c);

    int16_t hw_x = (int16_t)((LCR_HALF_W * rx) >> 12);
    int16_t hw_z = (int16_t)((LCR_HALF_W * rz) >> 12);

    SVECTOR v[4];
    v[0].vx = (int16_t)(cx - hw_x); v[0].vy = (int16_t)(cy - LCR_HALF_H); v[0].vz = (int16_t)(cz - hw_z); v[0].pad = 0;
    v[1].vx = (int16_t)(cx + hw_x); v[1].vy = (int16_t)(cy - LCR_HALF_H); v[1].vz = (int16_t)(cz + hw_z); v[1].pad = 0;
    v[2].vx = (int16_t)(cx - hw_x); v[2].vy = (int16_t)(cy + LCR_HALF_H); v[2].vz = (int16_t)(cz - hw_z); v[2].pad = 0;
    v[3].vx = (int16_t)(cx + hw_x); v[3].vy = (int16_t)(cy + LCR_HALF_H); v[3].vz = (int16_t)(cz + hw_z); v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4];
    int32_t otz;

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
       camera and the creep occlude it, with the SCENE_OT_MIN clamp that keeps a
       body seen from close up out of the single-digit buckets. */
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN)    otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1)  otz = OT_LENGTH - 2;

    int32_t fdx  = c->x - cam_x;
    int32_t fdz  = c->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    if (dist >= g_fog_far) return;          /* fully fogged: cull */
    int32_t fs   = render_fog_scale(dist);
    uint8_t fog8 = fs > 255 ? 255 : (uint8_t)fs;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, fog8, fog8, fog8);
    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
    poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;

    /* 64x64 at VRAM (640,320). U is the column within the tpage and x640 is an
       exact tpage boundary, so U runs 0..63. >>> V IS 64..127, NOT 0..63. <<<
       V is the row within the 256-tall tpage, the page base is y256, and the
       texture is at y320 — so Voff is 64 and every V here carries it. Getting
       this wrong samples a band of somebody else's art with no warning of any
       kind; it is the same arithmetic the map's Voff column prints.

       Inset by one texel on every edge: a magnified quad's edge pixels otherwise
       sample whatever is next door in VRAM (the zombie shipped with a red strip
       bled in from the row below for exactly this reason). */
    uint8_t u_left  = flip ? 62 : 1;
    uint8_t u_right = flip ?  1 : 62;
    poly->u0 = u_left;  poly->v0 =  65;
    poly->u1 = u_right; poly->v1 =  65;
    poly->u2 = u_left;  poly->v2 = 126;
    poly->u3 = u_right; poly->v3 = 126;

    poly->tpage = texmgr_tpage(lms_creep_tex);
    poly->clut  = texmgr_clut(lms_creep_tex);

    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    ctx->next_packet += sizeof(POLY_FT4);
}

void draw_lms_creeps(RenderContext *ctx) {
    int i;
    for (i = 0; i < lms_creep_count; i++) {
        LmsCreep *c = &lms_creeps[i];
        if (!c->active || c->state == LCR_DEAD || c->area != current_area) continue;

        int32_t dx = c->x - cam_x;
        int32_t dz = c->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 4000) continue;

        /* Mirror the sprite when the creep is to the player's right, so the art
           always faces toward them. Camera right = (icos, -isin); the shift
           keeps the product inside an int32 at room-sized coordinates. */
        int32_t dot = ((dx >> 4) * (icos(cam_rot) >> 4))
                    - ((dz >> 4) * (isin(cam_rot) >> 4));
        draw_lcr_sprite(ctx, c, dot <= 0);
    }
}
