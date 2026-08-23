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
#include "particles.h"
#include "hadad.h"
#include "crucifaxe.h"   /* SWING_RANGE — hadads_try_hit owns the reach */
#include "sound.h"
#include "cdaudio.h"     /* the stalker track */
#include "door_anim.h"   /* door_anim_active — see the guard in update_hadads */

Hadad hadads[MAX_HADADS];
int   hadad_count = 0;

/* ---- Sprites ----------------------------------------------------------------
   Three textures, each owning its own VRAM slot: no texmgr, no re-upload on a
   room transition, just one CD read and one LoadImage apiece at startup. Same
   arrangement as the Living Statue's two and the Mushroom Head's four, and
   affordable for the same reason — 96 rows fit the band under the HUD, which is
   all VRAM has left.

   The UV rect is derived from the TIM's own prect rather than hard-coded, so
   moving a slot in png_to_tim needs no matching edit here. */
typedef struct { uint16_t tpage, clut; uint8_t u0, v0, u1, v1; } Sprite;

enum { HAD_TEX_IDLE, HAD_TEX_STEP1, HAD_TEX_STEP2, HAD_TEX_COUNT };

static Sprite spr[HAD_TEX_COUNT];
static int    tex_loaded = 0;

static const char *had_tex_file[HAD_TEX_COUNT] = {
    "\\TEX\\HADIDLE.TIM;1",   /* the plinth pose               */
    "\\TEX\\HADSTP1.TIM;1",   /* stride A — also the ROOTED pose */
    "\\TEX\\HADSTP2.TIM;1",   /* stride B                      */
};

/* Texture window the current area expects, restored after each sprite. All
   three bodies sit at Voff 128, so a room's 128-tall window would wrap their V
   and sample whatever is above them in VRAM. Same bracket the statue uses. */
static RECT had_tw_restore;
static int  had_tw_active = 0;

void hadads_set_texwindow(const RECT *tw) {
    if (tw) { had_tw_restore = *tw; had_tw_active = 1; }
    else    { had_tw_active = 0; }
}

/* Startup loader for a texture that owns its VRAM slot. Copied from
   living_statue.c's, itself from the mushroom's. STARTUP ONLY — a CdRead is
   only safe before the per-frame render loop begins. */
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
    /* One-texel inset on every edge, as every other sprite here takes: a
       magnified quad sampling its own edge pixels drags a stripe of whatever is
       next to it in VRAM into the silhouette. */
    s->u0 = (uint8_t)(u_off + 1);
    s->v0 = (uint8_t)((tim.prect->y % 256) + 1);
    s->u1 = (uint8_t)(u_off + tex_w - 2);
    s->v1 = (uint8_t)((tim.prect->y % 256) + tim.prect->h - 2);
    free(buf);
}

void hadads_load_textures(void) {
    int i;
    for (i = 0; i < HAD_TEX_COUNT; i++)
        load_owned_sprite(had_tex_file[i], &spr[i]);
    tex_loaded = (spr[HAD_TEX_IDLE].tpage != 0);
}

/* ---- The music latch --------------------------------------------------------
   ONE CD track for the whole enemy, not one per instance. `music_delay` counts
   the rumble out before the track starts — the user asked for the music to come
   in "as soon as the rumble sound has finished playing", and HAD_RUMBLE_FRAMES
   is that clip's own length.

   >>> BOTH OF THESE MUST BE FORCED OFF ON A ROOM CHANGE. <<< update_hadads is
   area-gated, so it simply stops running the moment the player walks out, and a
   latch left set would have the track playing over the next room with nothing
   able to turn it off. hadads_silence() is what does that, from
   world_silence_monsters(). */
static int music_on    = 0;
static int music_delay = 0;

void hadads_silence(void) {
    if (music_on) cdaudio_stop();
    music_on    = 0;
    music_delay = 0;
    sound_stop(SFX_RUMBLE);
}

/* ---- Small numeric helpers --------------------------------------------------
   Integer square root (Newton, converging downward). Copied from
   living_statue.c INCLUDING the `t > 0` seed condition — `t > 1` stops one
   shift early and seeds below the root for every input that is not a perfect
   power of four, at which point the loop returns its own seed (isqrt(120) came
   back 8). Every distance stated as a RADIUS goes through it, so a stated
   radius means the same in every direction. The Rear Gate is 4400 x 7306, so
   the worst-case dx*dx + dz*dz is about 7.4e7 — well inside int32. */
static int32_t had_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* The standing anchor for a point, straight out of the room's floor zones — the
   same table apply_height walks, read directly rather than through
   apply_ddog_height because a teleport has to land at the right height on the
   frame it happens, and because gravity must never be applied to a Hadad
   standing on the plinth. Returns 0 and leaves *out alone if the point is off
   every zone.

   >>> THIS IS THE ONE THING THAT MAKES THE RAMP ARRIVAL WORK. <<< The Rear Gate
   is the first garden room with a FLOOR_RAMP, and HAD_RAMPTOP_Z sits 750 up an
   1100-unit climb — an anchor taken as a flat -149 would bury him three and a
   half metres in the gravel. Same routine as living_statue.c's
   lst_floor_anchor. */
static int had_floor_anchor(int32_t x, int32_t z, int32_t *out) {
    int i;
    for (i = 0; i < floor_zone_count; i++) {
        FloorZone *fz = &floor_zones[i];
        if (x < fz->min_x || x > fz->max_x) continue;
        if (z < fz->min_z || z > fz->max_z) continue;
        if (fz->type == FLOOR_FLAT || fz->type == FLOOR_UPPER) {
            *out = fz->y - GROUND_FLOOR_Y;
            return 1;
        }
        if (fz->type == FLOOR_RAMP) {
            int32_t len = fz->ramp_axis_end - fz->ramp_axis_start;
            int32_t pos = fz->ramp_along_x ? x : z;
            int32_t t, dy;
            if (len == 0) { *out = fz->ramp_y_start - GROUND_FLOOR_Y; return 1; }
            t = ((pos - fz->ramp_axis_start) << 12) / len;
            if (t <    0) t =    0;
            if (t > 4096) t = 4096;
            dy   = fz->ramp_y_end - fz->ramp_y_start;
            *out = fz->ramp_y_start + ((dy * t) >> 12) - GROUND_FLOOR_Y;
            return 1;
        }
    }
    return 0;
}

/* The state a role RESTS in: a plinth Hadad is masonry on show, a West Corridor
   one is simply not in the room. Used by both hadad_add and had_reseat, so the
   two can never disagree about what "put him back" means. */
static HadadState had_rest_state(HadadRole role) {
    return (role == HAD_ROLE_PLINTH) ? HAD_IDLE : HAD_ABSENT;
}

int hadad_add(int32_t x, int32_t z, int32_t y, GameState area, HadadRole role) {
    if (hadad_count >= MAX_HADADS) return -1;
    int i = hadad_count++;
    Hadad *h = &hadads[i];
    *h = (Hadad){0};
    h->x = x; h->y = y; h->z = z;
    h->spawn_x = x; h->spawn_y = y; h->spawn_z = z;
    h->health = HAD_MAX_HEALTH;
    h->role   = role;
    h->state  = had_rest_state(role);
    h->active = 1;
    h->area   = area;
    return i;
}

void hadads_init(void)  { hadad_count = 0; }
void hadads_reset(void) { hadad_count = 0; music_on = 0; music_delay = 0; }

/* Put him back on his spawn, at full health, with every scrap of AI state
   cleared — `leg` and `ramp_armed` among them, so a re-armed encounter starts
   from the top of its path. `role` and `spent` are the two things carried
   across. The caller may then override the STATE — see hadads_rest. */
static void had_reseat(Hadad *h) {
    int32_t   sx = h->spawn_x, sy = h->spawn_y, sz = h->spawn_z;
    int       sp = h->spent;
    HadadRole rl = h->role;
    GameState a  = h->area;
    *h = (Hadad){0};
    h->x = sx; h->y = sy; h->z = sz;
    h->spawn_x = sx; h->spawn_y = sy; h->spawn_z = sz;
    h->health = HAD_MAX_HEALTH;
    h->role   = rl;
    h->state  = had_rest_state(rl);
    h->spent  = sp;
    h->active = 1;
    h->area   = a;
}

/* Where this enemy STARTS a visit — the one thing about him that is not simply
   "back at his spawn", because it depends on the two flags. Called from
   world_leave() (a room change) and from savegame_capture().
 *
 * >>> THIS IS ALSO WHERE THE FLAG-THREE ENCOUNTER BURNS OUT. <<< The user's rule
 * is "if the player leaves the room while flag three is active and comes back
 * then he will have returned to his idle position on top of the plinth and can
 * never be activated again". That is read here as leaving while the encounter
 * is RUNNING — he has actually come up the ramp after them — and not merely as
 * leaving while the flag is set. Taken the second way, walking into the Rear
 * Gate and straight back out again would consume the encounter before the
 * player had any way to meet it, and there is nothing else that could re-arm
 * it. If the literal reading is wanted, drop the state test below.
 */
void hadads_rest(void) {
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (!h->active || h->state == HAD_DEAD) continue;

        /* Neither of the two scripted-room roles has a persistent posture at
           all: whatever he was doing, leaving the room puts him back on his
           appearance point in HAD_ABSENT, ready to run again on the next visit.
           Deliberately NOT the plinth's one-shot `spent` rule — for the West
           Corridor nothing was asked for that would end the ambush other than
           FLAG_HADAD_THREE (checked at the trigger), and the Library Destroyed
           encounter was specified to re-arm from the top after a death or an
           exit through the double door. had_reseat clears `stage`, `leg` and
           `branch` with everything else, so the replay starts from the first
           walk and not from wherever the last one got to. */
        if (h->role != HAD_ROLE_PLINTH) { had_reseat(h); continue; }

        int flag_one = game_flag(FLAG_HADAD_ONE);
        int flag_three = game_flag(FLAG_HADAD_THREE);

        /* Leaving mid-chase spends it, for good. */
        if (flag_three && !h->spent && h->state == HAD_WALK)
            h->spent = 1;

        had_reseat(h);   /* preserves `spent`; everything else is wiped */

        if (h->spent)      h->state = HAD_IDLE;     /* over: masonry again  */
        else if (flag_three) h->state = HAD_ABSENT;   /* waiting on the ramp  */
        else if (flag_one) {
            /* Posted at the corridor's south mouth, exactly where the first
               encounter left him, on every return visit. Authored rather than
               probed: hadads_rest runs on a save capture too, and the room's
               floor zones may belong to somewhere else entirely by then. The
               corridor floor is the room's flat y=0, so the anchor is the same
               -149 literal every other garden placement uses. */
            h->x     = HAD_END_X;
            h->z     = HAD_END_Z;
            h->y     = 0 - GROUND_FLOOR_Y;
            h->state = HAD_ROOTED;
        }
        /* else: neither flag — he is on the plinth, which had_reseat just did */
    }
}

/* No weaknesses, by design: the crucifaxe does exactly 1 and nothing else can
   touch him at all. */
static const Weakness hadad_weakness[] = {
    { DMG_KINETIC, 100 },   /* 100% = no change */
};

int32_t hadad_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, hadad_weakness, WEAKNESS_COUNT(hadad_weakness));
}

void hadad_body(const Hadad *h, int32_t *cyc, int32_t *hh, int32_t *hw) {
    *cyc = h->y + HAD_Y_OFFSET;
    *hh  = HAD_HALF_H;
    *hw  = HAD_HALF_W;
}

/* Is he a thing that can be hurt right now? IDLE (on the plinth) and ABSENT
   (flag three, before the ramp trigger) are both "no", and "no" means completely
   silent — no chip, no bar flash, no sound. Anything else would read as a
   weapon that works over a health bar that is not going down, which is a worse
   lie than nothing happening at all. */
static int had_vulnerable(const Hadad *h) {
    return h->active && (h->state == HAD_WALK || h->state == HAD_ROOTED);
}

void hadad_damage(Hadad *h, int dmg) {
    if (!had_vulnerable(h)) return;

    h->health   -= dmg;
    h->hit_timer = HAD_BAR_TIMER_MAX;
    if (h->health <= 0) {
        h->health = 0;
        h->state  = HAD_DEAD;
        /* Grey, because what comes out of him is stone — the Living Statue's
           colours exactly, and the user asked for it by name. spawn_burst
           carries a per-burst RGB already. */
        spawn_burst(h->x, h->y + HAD_Y_OFFSET, h->z,
                    HAD_BLOOD_R, HAD_BLOOD_G, HAD_BLOOD_B);
        sound_play(SFX_RUMBLE);
        /* The stalk is over, so the stalk music is too. The wanted/on
           reconciliation at the bottom of update_hadads would catch this on the
           next frame anyway; doing it here means the track dies on the same
           frame the body does. */
        if (music_on) { cdaudio_stop(); music_on = 0; }
        music_delay = 0;
    } else {
        sound_play(SFX_AXEHIT);
    }
}

void hadads_grinder_crush(int32_t x_half, int32_t z_min, int32_t z_max) {
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (h->area != current_area || !had_vulnerable(h)) continue;
        if (h->x < -x_half || h->x > x_half) continue;
        if (h->z < z_min   || h->z > z_max)  continue;
        /* "his health will be immediately depleted and he will be destroyed" —
           the whole bar in one go, through the normal damage path so the grey
           burst, the sound and the music stop all happen exactly as they do for
           the hundredth axe swing. */
        hadad_damage(h, h->health);
    }
}

/* ---- Steering ---------------------------------------------------------------
   The Living Statue's ls_steer verbatim (itself the mushroom's), with the goal
   passed in because Hadad has TWO of them: the player when he is following, and
   a fixed point at the bottom of the corridor when he is walking his path.

   `sight_clear` switches the wall-follow off when nothing stands between him
   and the goal. It is passed in rather than decided here for the reason the
   mushroom's is: the feeler's point-and-radius test also trips merely walking
   NEAR a hedge, and wall-following on that deadlocks in concave corners, while
   the real move's own collision push already slides the body along anything it
   grazes. */
static void had_steer(Hadad *h, int32_t goal_dx, int32_t goal_dz,
                      int32_t speed, int sight_clear) {
    int32_t desired_x = goal_dx, desired_z = goal_dz;
    int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                           (desired_z < 0 ? -desired_z : desired_z);
    if (desired_dist == 0) desired_dist = 1;

    int32_t feeler_x = h->x + (desired_x * HAD_FEELER_LEN) / desired_dist;
    int32_t feeler_z = h->z + (desired_z * HAD_FEELER_LEN) / desired_dist;
    int32_t fx = feeler_x, fz = feeler_z;
    apply_flat_entity_collision(&fx, &fz, HAD_BODY_RADIUS);
    int blocked = (fx != feeler_x || fz != feeler_z);
    if (sight_clear) { blocked = 0; h->steer_timer = 0; }

    int32_t pl_x = -goal_dz, pl_z =  goal_dx;   /* slide left  */
    int32_t pr_x =  goal_dz, pr_z = -goal_dx;   /* slide right */
    int32_t goal_px = h->x + goal_dx;
    int32_t goal_pz = h->z + goal_dz;

    if (blocked && h->steer_timer <= 0) {
        int32_t pl_dist = (pl_x < 0 ? -pl_x : pl_x) + (pl_z < 0 ? -pl_z : pl_z);
        int32_t pr_dist = (pr_x < 0 ? -pr_x : pr_x) + (pr_z < 0 ? -pr_z : pr_z);
        if (pl_dist == 0) pl_dist = 1;
        if (pr_dist == 0) pr_dist = 1;

        int32_t lx = h->x + (pl_x * HAD_FEELER_LEN) / pl_dist;
        int32_t lz = h->z + (pl_z * HAD_FEELER_LEN) / pl_dist;
        int32_t rx = h->x + (pr_x * HAD_FEELER_LEN) / pr_dist;
        int32_t rz = h->z + (pr_z * HAD_FEELER_LEN) / pr_dist;

        int32_t tlx = lx, tlz = lz;
        apply_flat_entity_collision(&tlx, &tlz, HAD_BODY_RADIUS);
        int left_blocked = (tlx != lx || tlz != lz);

        int32_t trx = rx, trz = rz;
        apply_flat_entity_collision(&trx, &trz, HAD_BODY_RADIUS);
        int right_blocked = (trx != rx || trz != rz);

        if (left_blocked && !right_blocked) {
            h->steer_dir = +1;
        } else if (right_blocked && !left_blocked) {
            h->steer_dir = -1;
        } else {
            int32_t ld = (goal_px - lx < 0 ? lx - goal_px : goal_px - lx) +
                         (goal_pz - lz < 0 ? lz - goal_pz : goal_pz - lz);
            int32_t rd = (goal_px - rx < 0 ? rx - goal_px : goal_px - rx) +
                         (goal_pz - rz < 0 ? rz - goal_pz : goal_pz - rz);
            h->steer_dir = (ld <= rd) ? -1 : +1;
        }
        h->steer_timer = HAD_STEER_COMMIT;
    }

    if (h->steer_timer > 0) {
        if (h->steer_dir < 0) { desired_x = pl_x; desired_z = pl_z; }
        else                  { desired_x = pr_x; desired_z = pr_z; }
        desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                       (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;
        h->steer_timer--;
    }

    /* Blend the new heading into the old one so turns are not instant. */
    int32_t move_x  = (desired_x * speed) / desired_dist;
    int32_t move_z  = (desired_z * speed) / desired_dist;
    int32_t prev_mx = (int16_t)(h->facing >> 16);
    int32_t prev_mz = (int16_t)(h->facing & 0xFFFF);
    int32_t blend_x = (prev_mx * (8 - HAD_TURN_RATE) + move_x * HAD_TURN_RATE) >> 3;
    int32_t blend_z = (prev_mz * (8 - HAD_TURN_RATE) + move_z * HAD_TURN_RATE) >> 3;
    h->facing = ((int32_t)(int16_t)blend_x << 16) | (uint16_t)(int16_t)blend_z;

    h->x += blend_x;
    h->z += blend_z;
    apply_flat_entity_collision(&h->x, &h->z, HAD_BODY_RADIUS);

    /* The walk cycle. Tied to frames rather than to distance travelled because
       he only ever has one speed; if HAD_SPEED ever gets a second value this
       should follow the ground covered instead. */
    if (++h->step_timer >= HAD_STEP_FRAMES) {
        h->step_timer = 0;
        h->step_frame ^= 1;
    }
}

/* Put him on the ground at (x, z), moving, and cue the arrival: the rumble now
   and the stalker music when it has finished. Seeds `facing` toward the player
   as well, because had_steer BLENDS the new heading into the old one and a
   zeroed facing would have him creep out of the arrival at a third speed for
   the first few frames — exactly the wrong moment to look sluggish. */
static void had_arrive(Hadad *h, int32_t x, int32_t z, int follow) {
    int32_t y;
    h->x = x;
    h->z = z;
    if (had_floor_anchor(x, z, &y)) h->y = y;
    h->vy             = 0;
    h->on_upper_floor = 0;
    h->on_ramp        = 0;
    h->follow         = follow;
    h->leg            = 0;                /* always from the top of the path  */
    h->state          = HAD_WALK;
    h->pause_timer    = HAD_WAKE_PAUSE;   /* a second of standing still first */
    h->step_timer     = 0;
    h->step_frame     = 0;

    {
        int32_t adx = player_x() - h->x, adz = player_z() - h->z;
        int32_t ad  = (adx < 0 ? -adx : adx) + (adz < 0 ? -adz : adz);
        if (ad > 0) {
            int32_t mx = (adx * HAD_SPEED) / ad;
            int32_t mz = (adz * HAD_SPEED) / ad;
            h->facing = ((int32_t)(int16_t)mx << 16) | (uint16_t)(int16_t)mz;
        }
    }

    sound_play(SFX_RUMBLE);
    music_delay = HAD_RUMBLE_FRAMES;   /* the track comes in as the rumble ends */
}

/* ---- Marching a leg ---------------------------------------------------------
   Straight at the goal at exactly `speed`, and NOTHING else: no feeler, no
   wall-follow, no collision push, and above all no heading blend. Used instead
   of had_steer by roles whose path is authored down open floor, where every one
   of those things is a liability rather than a help.

   >>> HE STILL COLLIDES WITH THE PLAYER. <<< That is hadads_collide, called
   from apply_collision_reception against the CAMERA, and it is a separate
   mechanism entirely — nothing here touches it. What is dropped is only his
   own push-out against WALL GEOMETRY, which for an authored path is redundant:
   the path is already known to be walkable.

   >>> AND DROPPING THE BLEND IS THE POINT, NOT A SIDE EFFECT. <<< had_steer
   eases the heading with (prev * 5 + want * 3) >> 3, and that shift TRUNCATES,
   so the recurrence has integer fixed points well short of the value it is
   easing toward. Turning a standing start east (prev 0, want +4) it goes
   1, 2, 2, 2... — (2*5 + 4*3) >> 3 is 22 >> 3, which is 2 — and sticks there at
   HALF SPEED for ever. The component being turned AWAY from is worse: from -4
   with a want of 0 it goes -3, -2, -2... and -2 is also a fixed point, because
   -10 >> 3 floors to -2. So a Hadad who turns a corner crabs off diagonally at
   half pace and NEVER straightens up.

   That is exactly what the West Corridor's second leg did. He turned east at
   the corner and from then on moved (+2, -2) a frame: half speed east, and a
   permanent southward drift into a wall only 25 units clear of his 300-unit
   body. The wall push fought the drift every single frame, which is what made
   him judder — the wall was the SYMPTOM, and it was also the only thing keeping
   him anywhere near his own path. Removing wall collision alone would have
   stopped the juddering and walked him quietly out of the room instead. */
static void had_march(Hadad *h, int32_t goal_dx, int32_t goal_dz,
                      int32_t speed, int step_frames) {
    /* RADIAL, not the Manhattan sum had_steer normalises by: a Manhattan
       divisor makes a diagonal leg travel `speed` in the sum of its components
       rather than along the line, i.e. slower the closer to 45° it runs. Both
       of the West Corridor's legs are axis-aligned so the two agree today, but
       an authored path is exactly the thing somebody will later run at an
       angle, and then this would be a silent speed change. */
    int32_t d = had_isqrt(goal_dx * goal_dx + goal_dz * goal_dz);
    if (d <= 0) return;

    int32_t mx = (goal_dx * speed) / d;
    int32_t mz = (goal_dz * speed) / d;
    h->x += mx;
    h->z += mz;
    /* Kept in step even though nothing blends off it here, so the field means
       the same thing in both movement paths and a later reader of `facing` is
       not quietly given a stale heading from the previous leg. */
    h->facing = ((int32_t)(int16_t)mx << 16) | (uint16_t)(int16_t)mz;

    /* The walk cycle, which had_steer would otherwise have been advancing.
       Leave this out and he slides the length of the corridor in a single
       frozen pose. The cadence is passed IN rather than read from
       HAD_STEP_FRAMES because a marched leg may run at a speed of its own (the
       Library Destroyed's first walk does), and the two must always be picked
       together or the stride stops matching the ground covered — see
       HAD_LD_WALK1_STEP_FRAMES. */
    if (++h->step_timer >= step_frames) {
        h->step_timer = 0;
        h->step_frame ^= 1;
    }
}

/* Does this role's authored path run down open floor? A role that says yes
   marches its legs (had_march) instead of steering them, and ignores wall
   geometry entirely while doing so.

   The West Corridor's two legs run down the centre line of two straight arms
   with nothing in either of them, so there is nothing for steering to earn, and
   the Library Destroyed's five are the same shape — the aisles are straight,
   empty and exactly his width, and the two corners are authored waypoints. The
   Rear Gate's single leg says NO and keeps had_steer: that one threads the
   corridor's north mouth and its side hedges with a 300-unit body, its seeded
   facing already points along the goal (so the blend above is stable for it,
   which is why that walk never showed this bug), and it is not what the user
   reported. */
static int had_path_is_open(HadadRole role) {
    return role != HAD_ROLE_PLINTH;
}

/* ---- The scripted paths -----------------------------------------------------
   Where a Hadad with follow == 0 is heading on his CURRENT leg. Returns 0 when
   the path is walked out, which is the caller's cue to root him.

   One table for both roles, because the only difference between "walk to the
   bottom of the Rear Gate's corridor" and "walk the West Corridor's L" is how
   many points there are. The Rear Gate's single leg is unchanged: leg 0 is
   HAD_END, leg 1 does not exist, so he arrives and roots exactly as before.

   >>> THE WEST CORRIDOR NEEDS TWO LEGS AND CANNOT BE DONE WITH ONE. <<< That
   room is an L. A lone goal at the east door points him east from the moment he
   appears, which walks him straight into the west arm's east wall (collision
   walls 12/13 at x=-2100) and hands a right-angled turn to a wall-follow that
   only commits for HAD_STEER_COMMIT frames at a time. The corner waypoint makes
   the turn part of the SCRIPT, and both legs then run down the middle of a
   straight arm with a clear sightline, so had_steer's wall-follow never engages
   at all. */
static int had_path_goal(const Hadad *h, int32_t *gx, int32_t *gz) {
    if (h->role == HAD_ROLE_WEST_CORR) {
        switch (h->leg) {
        case 0: *gx = HAD_WC_CORNER_X; *gz = HAD_WC_CORNER_Z; return 1;
        case 1: *gx = HAD_WC_END_X;    *gz = HAD_WC_END_Z;    return 1;
        default: return 0;
        }
    }
    /* ---- The Library Destroyed's TWO walks, indexed by (stage, leg) --------
       Stage 0 is the one the player runs from: in at the east end of the spur,
       west to the corner, south to the single door. Stage 1 is the one they run
       from AFTER the crawl, and it is the same path in reverse with a fork on
       the end: north up the west aisle, east along the spur, then down whichever
       arm of the east aisle they were in when he reached the junction.

       `branch` is read here and LATCHED elsewhere (had_leg_entered). Solving it
       from the live player position on every call would swing the goal from one
       end of the aisle to the other the moment they crossed HAD_LD_BRANCH_Z,
       and he would pivot on the junction instead of committing to an arm.
       branch == 0 cannot be reached from leg 2 — the latch runs on the same
       frame the leg is taken up — but it falls to SOUTH rather than returning 0,
       because rooting him mid-room on a bad read would be the worse failure. */
    if (h->role == HAD_ROLE_LIBRARY) {
        if (h->stage == 0) {
            switch (h->leg) {
            case 0: *gx = HAD_LD_CORNER_X; *gz = HAD_LD_CORNER_Z; return 1;
            case 1: *gx = HAD_LD_SDOOR_X;  *gz = HAD_LD_SDOOR_Z;  return 1;
            default: return 0;
            }
        }
        switch (h->leg) {
        case 0: *gx = HAD_LD_CORNER_X; *gz = HAD_LD_CORNER_Z; return 1;
        case 1: *gx = HAD_LD_APPEAR_X; *gz = HAD_LD_APPEAR_Z; return 1;
        case 2:
            if (h->branch == 1) { *gx = HAD_LD_NORTH_X; *gz = HAD_LD_NORTH_Z; }
            else                { *gx = HAD_LD_SOUTH_X; *gz = HAD_LD_SOUTH_Z; }
            return 1;
        default: return 0;
        }
    }
    if (h->leg == 0) { *gx = HAD_END_X; *gz = HAD_END_Z; return 1; }
    return 0;
}

/* Called the instant `leg` is advanced, BEFORE the new goal is asked for. The
   one thing on any of these paths that is decided at run time rather than
   authored: which arm of the Library Destroyed's east aisle Hadad takes when he
   reaches the junction at the east end of the spur.

   Latched once and never revisited — see the note in had_path_goal. Measured
   against the spur's own north edge, so "north of the line" is exactly "up in
   the vestibule end of the aisle, at the double door". player_z() and not
   cam_z: the crawl anchors the player, and although that cutscene is over by
   the time this can fire, an enemy that reads the camera to find the player is
   the single most likely bug in this whole feature. */
/* How fast this role walks its CURRENT leg, and the stride cadence that goes
   with it. One function so the pair can never be picked apart: every caller
   takes both or neither.

   Only one path has a pace of its own — the Library Destroyed's FIRST walk, at
   half of HAD_SPEED, because at full speed he is round the corner before the
   player has noticed the single door has gone dead. His second walk, and both
   of the other two roles', run at the enemy's own speed. */
static void had_path_pace(const Hadad *h, int32_t *speed, int *step_frames) {
    if (h->role == HAD_ROLE_LIBRARY && h->stage == 0) {
        *speed       = HAD_LD_WALK1_SPEED;
        *step_frames = HAD_LD_WALK1_STEP_FRAMES;
        return;
    }
    *speed       = HAD_SPEED;
    *step_frames = HAD_STEP_FRAMES;
}

static void had_leg_entered(Hadad *h) {
    if (h->role != HAD_ROLE_LIBRARY || h->stage != 1 || h->leg != 2) return;
    if (h->branch) return;
    h->branch = (player_z() > HAD_LD_BRANCH_Z) ? 1 : 2;
}

void update_hadads(void) {
    int i;
    int music_wanted = 0;

    /* >>> A ROOM TRANSITION HAS ALREADY BEGUN: DO NOTHING. <<< The door-trigger
       branches in update_current_area are not the last thing in that function —
       the shared entity updates below them, this one among them, still run once
       on the very frame the door is pressed. door_anim_start() has by then
       called world_silence_monsters() -> hadads_silence(), which stopped the
       track and cleared music_on; without this guard the reconciliation at the
       bottom of this function would see music_wanted still set, music_on clear,
       and immediately cdaudio_play() the stalker track BACK ON. That is exactly
       what it did: the music cut on the press, came straight back over the whole
       door transition, and only died when the next room loaded — and the three
       CD commands issued in that one frame (stop, stop, play) plus a CD-DA
       stream competing with the mesh read is what made the transition lurch.
       The comment in world_silence_monsters() claims "the area update stops
       running the instant the transition begins"; it stops running from the NEXT
       frame, and this is the frame it does not cover. */
    if (door_anim_active()) return;

    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        /* current_area, NEVER game_state: gating on game_state would freeze the
           whole enemy for as long as the inventory menu is up, letting the
           player pause the chase with Start (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!h->active || h->state == HAD_DEAD || h->area != current_area)
            continue;

        if (h->hit_timer    > 0) h->hit_timer--;
        if (h->damage_timer > 0) h->damage_timer--;

        int flag_one = game_flag(FLAG_HADAD_ONE);
        int flag_three = game_flag(FLAG_HADAD_THREE);

        /* player_x/y/z, not cam_*: a camera-locked puzzle flies the camera off
           to a fixed shot while the PLAYER stays standing where they were, and
           an enemy that tracked the camera would close on an empty spot
           (camera.h). Enemies keep acting through those puzzles. */
        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - h->x;
        int32_t dy = py - (h->y + HAD_Y_OFFSET);
        int32_t dz = pz - h->z;
        int32_t dist = had_isqrt(dx * dx + dz * dz);

        /* ---- The state the flags say this visit should be in ---------------
           hadads_rest() sets this on the way OUT of a room, so ordinarily it is
           already right on the way in. This block is what covers the two cases
           it cannot: a first visit (world_seed_room only ever places him on the
           plinth, because that placement has to be authored for rooms whose
           geometry is not resident) and a flag switched on from the title
           screen's debug menu while he is standing here. */
        if (h->role == HAD_ROLE_PLINTH && h->state == HAD_IDLE && !h->spent) {
            if (flag_three) {
                h->state = HAD_ABSENT;
            } else if (flag_one) {
                h->x = HAD_END_X;
                h->z = HAD_END_Z;
                if (!had_floor_anchor(h->x, h->z, &h->y)) h->y = 0 - GROUND_FLOOR_Y;
                h->state = HAD_ROOTED;
            }
        }

        switch (h->state) {

        case HAD_IDLE:
            /* A statue, and invulnerable. He comes off the plinth for exactly
               one reason: the player walking up to the grinders. `spent` and
               either flag already having been set are both handled above, so by
               the time control reaches here the only live case is the very
               first encounter. */
            if (!h->spent && !flag_one && !flag_three) {
                int32_t gdx = px - HAD_GRINDER_X;
                int32_t gdz = pz - HAD_GRINDER_Z;
                if (had_isqrt(gdx * gdx + gdz * gdz) <= HAD_TRIG_RADIUS) {
                    game_flag_set(FLAG_HADAD_ONE);
                    /* Straight to the corridor's north mouth, behind them,
                       across the only way back to the lawn. `follow` = 0: from
                       here he walks his path and nothing else. */
                    had_arrive(h, HAD_MOUTH_X, HAD_MOUTH_Z, 0);
                }
            }
            break;

        case HAD_ABSENT:
            /* ---- The West Corridor ambush ------------------------------------
               The gate is FLAG ONE SET AND FLAG THREE CLEAR, read the same way
               round as hadad_lever_locked(): flag three REPLACES flag one
               everywhere in this enemy, so arming the third Rear Gate encounter
               closes this one for good. Tested here, live, rather
               than latched at room entry, so a debug flag thrown from the title
               screen takes effect the same way every other branch here does.

               The trigger is the corner where the corridor turns, true radial
               so it reads the same coming down the west arm as coming along the
               south one. He appears at the far end of the room in front of the
               double door and walks his two legs; follow = 0, because this is a
               fixed path and not a pursuit — where he ends up must not depend
               on where the player ran. had_arrive sounds the rumble and cues
               the stalker track, as every Hadad arrival does. */
            if (h->role == HAD_ROLE_WEST_CORR) {
                if (flag_one && !flag_three) {
                    int32_t cdx = px - HAD_WC_CORNER_X;
                    int32_t cdz = pz - HAD_WC_CORNER_Z;
                    if (had_isqrt(cdx * cdx + cdz * cdz) <= HAD_WC_TRIG_RADIUS)
                        had_arrive(h, HAD_WC_START_X, HAD_WC_START_Z, 0);
                }
                break;
            }

            /* ---- The Library Destroyed encounter ----------------------------
               NO FLAG TEST OF ITS OWN, and that is deliberate: this instance
               only exists in a room that only exists while FLAG_HADAD_TWO is
               set (library_destroyed_active()), so the gate the other two roles
               need is already the price of getting in here.

               The trigger is the north-west corner, true radial so it reads the
               same coming down the west aisle from the stairwell door as coming
               west along the spur. He appears at the far end of the spur and
               walks his stage-0 legs; follow = 0 because everything about this
               encounter is a fixed path — the ESCAPE is what depends on the
               player, and that belongs to the director. */
            if (h->role == HAD_ROLE_LIBRARY) {
                int32_t cdx = px - HAD_LD_TRIG_X;
                int32_t cdz = pz - HAD_LD_TRIG_Z;
                if (had_isqrt(cdx * cdx + cdz * cdz) <= HAD_LD_TRIG_RADIUS)
                    had_arrive(h, HAD_LD_APPEAR_X, HAD_LD_APPEAR_Z, 0);
                break;
            }

            /* Flag three, waiting. Two-part latch: the player has to have been up
               the ramp AND come back down off it. One without the other would
               fire on somebody who had merely walked south, which would put him
               in front of them instead of behind. */
            if (pz <= HAD_RAMP_ARM_Z) h->ramp_armed = 1;
            if (h->ramp_armed && pz >= HAD_RAMP_FIRE_Z)
                had_arrive(h, HAD_RAMPTOP_X, HAD_RAMPTOP_Z, 1);
            break;

        case HAD_WALK:
        case HAD_ROOTED: {
            music_wanted = 1;

            /* ---- The blow. Horizontal and vertical reach are tested
               SEPARATELY, for the reason every other enemy here does the same:
               the player's eye sits above the body's centre, and a combined
               budget eats the whole allowance vertically so an adjacent enemy
               never lands a hit. The horizontal half is the RADIAL `dist` — see
               HAD_CATCH_DIST for why a Manhattan sum cannot work at this size.

               The vertical budget is HAD_CATCH_DIST + HAD_HALF_H, which is
               deliberately MORE generous than the axe's own vertical reach
               (HAD_HALF_H + SWING_RANGE): that is what makes "he can be hit" a
               strict subset of "he can hit back". On the flat, `dy` from the
               player's eye to his sprite centre is only 110, so this never
               binds in the corridor — it exists for the ramp. */
            if (!game_over && dist < HAD_CATCH_DIST &&
                (dy < 0 ? -dy : dy) < HAD_CATCH_DIST + HAD_HALF_H &&
                h->damage_timer == 0) {
                h->damage_timer = HAD_HIT_COOLDOWN;
                player_hurt(HAD_DAMAGE);
                player_knockback(h->x, h->z, HAD_KNOCKBACK);
                sound_play(SFX_HURT);
                if (player_health <= 0) {
                    player_health = 0;
                    game_over     = 1;
                    flash_timer   = 90;
                    sound_play(SFX_DIE);
                }
            }

            if (h->state == HAD_ROOTED) break;   /* arrived: he only stands */

            /* The wake pause. He is already SOLID and already dangerous here —
               only the walking is held back — so a player who runs into him
               during it is treated exactly as they would be a second later.
               Nothing below this line runs, which also means the walk cycle
               does not advance and he holds the idle pose. */
            if (h->pause_timer > 0) { h->pause_timer--; break; }

            /* Gravity and the floor. He is only ever probed while walking — a
               Hadad on the plinth would be dragged straight off it, and this is
               also what walks him DOWN the ramp under flag three rather than
               sliding him along at the height he arrived at. */
            apply_ddog_height(&h->x, &h->y, &h->z, &h->vy,
                              &h->on_upper_floor, &h->on_ramp);

            if (h->follow) {
                /* Flag three: he simply comes after them, up the corridor, out
                   onto the lawn, anywhere they go — until the room ends it.
                   Walk until his own cylinder is what is stopping him
                   (HAD_HOLD_DIST), not until he is in strike range: stopping at
                   the reach would park him where his push-out never engages,
                   and the reach is sized to clear the hold anyway. */
                if (dist >= HAD_HOLD_DIST) {
                    int clear = !collision_segment_blocked(
                        h->x, h->y + HAD_Y_OFFSET, h->z, px, py, pz);
                    had_steer(h, dx, dz, HAD_SPEED, clear);
                }
            } else {
                /* A fixed path, not a pursuit — the Rear Gate's one leg to the
                   bottom of its corridor, or the West Corridor's two round the
                   turn. Nothing diverts him from it, which is what makes
                   "blocking the way back" a guarantee rather than something
                   that depends on where the player ran. The stalk the player
                   experiences is the by-product: he entered behind them and the
                   corridor only goes one way.

                   HOW the leg is walked depends on the role — see
                   had_path_is_open(). An open path MARCHES: straight at the
                   waypoint, full speed, ignoring wall geometry. A path with
                   obstacles in it STEERS, because a 300-unit body still has
                   mouths and hedge corners to get round, and there the
                   SIGHTLINE test is what keeps the wall-follow from engaging
                   down the middle of an arm it fits exactly.

                   Each leg is snapped to exactly on arrival before the next one
                   is taken up, so the turn happens from the authored point and
                   not from wherever HAD_ARRIVE_DIST happened to catch him. */
                int32_t tx, tz, leg_speed;
                int     leg_step;
                if (!had_path_goal(h, &tx, &tz)) { h->state = HAD_ROOTED; break; }
                had_path_pace(h, &leg_speed, &leg_step);

                int32_t gx = tx - h->x;
                int32_t gz = tz - h->z;
                int32_t gd = (gx < 0 ? -gx : gx) + (gz < 0 ? -gz : gz);
                if (gd <= HAD_ARRIVE_DIST) {
                    h->x = tx;
                    h->z = tz;
                    h->leg++;
                    had_leg_entered(h);   /* latch anything the next leg needs */
                    /* Last leg walked out: he stands here for the rest of the
                       visit. Still solid, still dangerous, still holding the
                       music up — HAD_ROOTED is a monster standing in a
                       corridor, not a statue again. */
                    if (!had_path_goal(h, &tx, &tz)) h->state = HAD_ROOTED;
                } else if (had_path_is_open(h->role)) {
                    had_march(h, gx, gz, leg_speed, leg_step);
                } else {
                    int clear = !collision_segment_blocked(
                        h->x, h->y + HAD_Y_OFFSET, h->z,
                        tx, h->y + HAD_Y_OFFSET, tz);
                    had_steer(h, gx, gz, leg_speed, clear);
                }
            }
            break;
        }

        default:
            break;
        }
    }

    /* ---- Reconcile the music with what the room actually contains ----------
       Written as a wanted/on comparison rather than as a start call at each
       arrival, because there are three ways for the track to become wanted (the
       two arrivals AND simply walking into a room where he is already posted at
       the corridor mouth) and four ways for it to stop (death, the room change,
       game over, and a debug flag being cleared). One reconciliation covers all
       of them and cannot be forgotten at a new call site.

       >>> WALKING INTO THE ROOM AND FINDING HIM ALREADY THERE STARTS THE TRACK
       WITH NO RUMBLE. <<< The rumble is the sound of him ARRIVING, and on a
       return visit nothing arrives — he has been standing at the bottom of the
       corridor since the last time. So that path takes the else branch below
       and the music simply comes up. */
    if (game_over) music_wanted = 0;

    if (music_delay > 0) {
        if (--music_delay == 0) {
            cdaudio_play(CDAUDIO_STALKER_TRACK, 1);
            music_on = 1;
        }
    }

    if (!music_wanted) {
        if (music_on) { cdaudio_stop(); music_on = 0; }
        music_delay = 0;
    } else if (!music_on && music_delay == 0) {
        cdaudio_play(CDAUDIO_STALKER_TRACK, 1);
        music_on = 1;
    }
}

/* ---- The Library Destroyed director API ------------------------------------
   Three functions, and they are everything src/hadad_library.c is allowed to
   know about this enemy (see the marked block at the foot of hadad.h). */

Hadad *hadad_library_instance(void) {
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (!h->active || h->role != HAD_ROLE_LIBRARY) continue;
        if (h->area != current_area || h->state == HAD_DEAD) continue;
        return h;
    }
    return NULL;
}

int hadad_library_present(void) {
    Hadad *h = hadad_library_instance();
    /* WALK or ROOTED — i.e. he is in the room, either stage. Deliberately the
       APPEARANCE and not the arrival: the single door dies and the crawl gap's
       prompt comes up the moment the player sees him at the end of the spur,
       not when he finally reaches the door a dozen seconds later. */
    return h && (h->state == HAD_WALK || h->state == HAD_ROOTED);
}

void hadad_library_begin_return(Hadad *h) {
    if (!h || h->role != HAD_ROLE_LIBRARY) return;
    h->stage  = 1;
    h->branch = 0;
    /* Straight onto the spot the first walk was aimed at, whether or not he had
       time to get there — the player was under the floor and did not see, and
       "he is at the door you were running for" is the read the beat wants.
       had_arrive resets `leg`, so stage 1 starts from the top of its own path,
       and it re-serves the wake pause, which gives the player a second of him
       standing there before he starts back. */
    had_arrive(h, HAD_LD_SDOOR_X, HAD_LD_SDOOR_Z, 0);
    /* had_arrive also re-cues the stalker track behind the rumble, which is
       right for a first arrival and wrong here: the track has been playing
       since he appeared, and letting the cue land would restart the CD from the
       top two seconds after control comes back. The rumble itself still
       sounds — that is the announcement. */
    if (music_on) music_delay = 0;
}

int hadad_lever_locked(void) {
    /* The first encounter takes the lever away and never gives it back; the
       second one hands it straight over again, because the grinders are how the
       player is meant to beat him. Note this is asked of the FLAGS and not of
       any Hadad's state: it must answer the same way on the frame the room
       loads, before update_hadads has run, and it must go on answering "locked"
       long after he has stopped moving. */
    return game_flag(FLAG_HADAD_ONE) && !game_flag(FLAG_HADAD_THREE);
}

/* Crucifaxe strike. Reach is measured to the body's SURFACE, not to its centre,
   because the wall-like standoff puts the centre far out of the axe's range:
   the push holds the player HAD_BODY_RADIUS + 195 = 495 off, against a
   SWING_RANGE of 350. Measured from the surface the same stand gives a 195
   horizontal gap and, with the player's eye inside the body's vertical span, no
   vertical gap at all — 195 against 350, from every bearing.

   >>> THIS FUNCTION AND THE CONTACT TEST IN update_hadads SHARE A NUMBER. <<<
   HAD_CATCH_DIST is defined as HAD_BODY_RADIUS + SWING_RANGE precisely so that
   the outermost stand this will accept is also the outermost stand he can
   strike from — "the player should never be able to get close enough to hit him
   without also taking damage". Read the note at that constant before changing
   either side of it.

   The horizontal gap is derived from HAD_BODY_RADIUS, the very constant
   hadads_collide pushes with, so the two can never disagree about where the
   edge is and leave the player held at a distance the axe cannot cross. */
int hadads_try_hit(void) {
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        /* Not in a damageable state: skipped, and NOT reported as a hit, so the
           swing is still live for whatever else is in range. */
        if (h->area != current_area || !had_vulnerable(h)) continue;

        int32_t dx = h->x - cam_x;
        int32_t dz = h->z - cam_z;
        int32_t d  = had_isqrt(dx * dx + dz * dz);
        int32_t gh = d > HAD_BODY_RADIUS ? d - HAD_BODY_RADIUS : 0;

        int32_t top = h->y + HAD_Y_OFFSET - HAD_HALF_H;
        int32_t bot = h->y + HAD_Y_OFFSET + HAD_HALF_H;
        int32_t ey  = cam_y;
        int32_t gv  = (ey < top) ? (top - ey) : (ey > bot ? ey - bot : 0);

        if (gh + gv >= SWING_RANGE) continue;

        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;

        hadad_damage(h, 1);
        return 1;
    }
    return 0;
}

void hadads_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (!h->active || h->area != current_area) continue;
        /* ABSENT is not in the room and DEAD is rubble: neither is solid. */
        if (h->state == HAD_DEAD || h->state == HAD_ABSENT) continue;

        /* Vertical gate against the player's body span, so a Hadad the player
           somehow got above does not push them in mid-air. On the plinth his
           body runs from -800 down to feet at -200 and the player's eye on the
           lawn is at -189, so this passes and the statue is solid — which is
           what it should be. */
        int32_t top = h->y + HAD_Y_OFFSET - HAD_HALF_H;
        int32_t bot = h->y + HAD_Y_OFFSET + HAD_HALF_H;
        if (py + GROUND_FLOOR_Y < top || py - 30 > bot) continue;

        /* A radial push, not the crates' AABB: a billboard has no facing, so the
           stop distance must be identical from every bearing, and that needs a
           true sqrt (tools/ADDING_A_3D_ENEMY.txt STEP 6). */
        int32_t dx   = *px - h->x;
        int32_t dz   = *pz - h->z;
        int32_t need = HAD_BODY_RADIUS + radius;
        int32_t d    = had_isqrt(dx * dx + dz * dz);
        if (d >= need) continue;
        if (d == 0) { *px = h->x + need; continue; }   /* dead centre */
        *px = h->x + (dx * need) / d;
        *pz = h->z + (dz * need) / d;
    }
}

/* Bracket an already-filled POLY_FT4 with a full/unmasked texture window and a
   restore of the area's own, all within one OT bucket. Identical to the
   statue's, and needed for the same reason: all three sprites sit at Voff 128.
   addPrim() prepends, so adding restore, poly, disable yields the draw order
   disable -> poly -> restore. */
static void add_ft4_windowed(RenderContext *ctx, int32_t otz, POLY_FT4 *poly) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;

    if (had_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &had_tw_restore);
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

/* One camera-facing body quad. No mirror flip and no roll, so there is
   deliberately no gte_nclip backface cull — a camera-facing quad has no back
   face to cull (tools/ADDING_AN_ENEMY.txt mistake 4). */
static void draw_had_sprite(RenderContext *ctx, Hadad *h, const Sprite *sp) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);

    int32_t cx = h->x, cz = h->z, cy = h->y + HAD_Y_OFFSET;

    int16_t dwx = (int16_t)((HAD_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((HAD_HALF_W * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - HAD_HALF_H);
    int16_t vy_bot = (int16_t)(cy + HAD_HALF_H);

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
    /* Sort on the room-geometry scale (raw average Z) so hedges between the
       camera and the body occlude it, with the SCENE_OT_MIN clamp every sprite
       here takes. */
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    int32_t fdx  = h->x - cam_x;
    int32_t fdz  = h->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    if (dist >= g_fog_far) return;          /* fully fogged: cull */
    int32_t fs   = render_fog_scale(dist);
    uint8_t fog8 = fs > 255 ? 255 : (uint8_t)fs;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    if (h->hit_timer > 0) setRGB0(poly, fog8, fog8 >> 2, fog8 >> 2);
    else                  setRGB0(poly, fog8, fog8, fog8);

    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

    poly->u0 = sp->u0; poly->v0 = sp->v0;
    poly->u1 = sp->u1; poly->v1 = sp->v0;
    poly->u2 = sp->u0; poly->v2 = sp->v1;
    poly->u3 = sp->u1; poly->v3 = sp->v1;

    poly->tpage = sp->tpage;
    poly->clut  = sp->clut;

    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, otz, poly);

    if (h->hit_timer <= 0) return;

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

    int16_t fill_w = (int16_t)((h->health * 40) / HAD_MAX_HEALTH);
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

void draw_hadads(RenderContext *ctx) {
    if (!tex_loaded) return;
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (!h->active || h->area != current_area) continue;
        if (h->state == HAD_DEAD || h->state == HAD_ABSENT) continue;

        int32_t dx = h->x - cam_x;
        int32_t dz = h->z - cam_z;
        int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (wdist >= g_fog_far) continue;

        /* >>> NO SIGHTLINE CULL, for the Living Statue's reason and more so.
           <<< The other sprite enemies skip themselves when
           collision_hidden_from_camera says a wall stands between them and the
           camera. That test is a flat XZ wall-crossing with no Y in it, and
           this body is 600 tall against 500-tall hedges — it would throw away
           a silhouette whose head stands clear of the thing supposedly hiding
           it, which is most of what makes him worth putting on a plinth
           in the middle of an open lawn. The OT sort already handles the
           occlusion correctly and does it per pixel. */

        /* The idle art covers two poses: the statue on the plinth, and the
           second he spends standing where he has just arrived before the first
           stride (HAD_WAKE_PAUSE). Once he is moving it is the two step frames,
           and a Hadad who has ARRIVED holds STEP1 — he is a monster standing in
           a corridor, not a statue again. */
        int tex = (h->state == HAD_IDLE || h->pause_timer > 0) ? HAD_TEX_IDLE
                : (h->state == HAD_WALK && h->step_frame) ? HAD_TEX_STEP2
                : HAD_TEX_STEP1;
        draw_had_sprite(ctx, h, &spr[tex]);
    }
}
