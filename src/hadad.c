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

/* The ejection watch's sample of the player, one frame old. Declared up here
   only so hadads_reset() can clear it; the rule these serve, and every reason
   it is written the way it is, are at had_watch_ejection() below. */
static int32_t   had_watch_x = 0, had_watch_y = 0, had_watch_z = 0;
static GameState had_watch_area  = (GameState)-1;
static int       had_watch_epoch = -1;
static int       had_watch_valid = 0;

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
void hadads_reset(void) {
    hadad_count = 0; music_on = 0; music_delay = 0;
    /* The ejection watch holds a player position, so it belongs to a visit and
       not to the module. A new game or a load must not leave last session's
       spot behind to be measured against this room's walls — the area and
       anchor-epoch guards would catch it anyway, but this is the honest place
       to say it. */
    had_watch_valid = 0;
}

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
           Corridor nothing was asked for that would end either the ambush or
           the return other than the flags themselves (checked at the trigger:
           FLAG_HADAD_TWO hands the room from one to the other and
           FLAG_HADAD_THREE closes the first), and the Library Destroyed
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

Hadad *hadads_grinder_caught(int32_t x_half, int32_t z_min, int32_t z_max) {
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (h->area != current_area || !had_vulnerable(h)) continue;
        if (h->x < -x_half || h->x > x_half) continue;
        if (h->z < z_min   || h->z > z_max)  continue;
        /* >>> IT ANSWERS; IT NO LONGER KILLS. <<< This used to be
           hadad_damage(h, h->health) — the whole bar in one go the moment the
           plates had him. The kill still happens through exactly that call, but
           it happens at the END of the Hadad Death Scene (src/hadad_grinder.c),
           six seconds later, with the plates all but shut and the body squashed
           to nothing. Emptying the bar here as well would fire the grey burst
           at the moment the camera was only just taking over. */
        return h;
    }
    return NULL;
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
/* A point HAD_FEELER_LEN ahead along (dx, dz) — the probe the steering asks its
   questions with, and RADIALLY normalised for the reason the constant exists.
   The Manhattan divisor this replaces made the probe short by up to 41% on a
   diagonal, i.e. 283 units of a stated 400, which is INSIDE HAD_BODY_RADIUS:
   "a probe shorter than the cylinder's own radius reports clear ground the body
   cannot fit into, and he would grind on every hedge corner in the room" is the
   note at HAD_FEELER_LEN, and on every diagonal approach that is exactly what
   it was. Length now means length, on every bearing. */
static void had_feeler(const Hadad *h, int32_t dx, int32_t dz,
                       int32_t *ox, int32_t *oz) {
    int32_t d = had_isqrt(dx * dx + dz * dz);
    if (d <= 0) { *ox = h->x; *oz = h->z; return; }
    *ox = h->x + (dx * HAD_FEELER_LEN) / d;
    *oz = h->z + (dz * HAD_FEELER_LEN) / d;
}

static void had_steer(Hadad *h, int32_t goal_dx, int32_t goal_dz,
                      int32_t speed, int sight_clear) {
    int32_t desired_x = goal_dx, desired_z = goal_dz;

    int32_t feeler_x, feeler_z;
    had_feeler(h, desired_x, desired_z, &feeler_x, &feeler_z);
    int32_t fx = feeler_x, fz = feeler_z;
    apply_flat_entity_collision(&fx, &fz, HAD_BODY_RADIUS);
    int blocked = (fx != feeler_x || fz != feeler_z);
    if (sight_clear) { blocked = 0; h->steer_timer = 0; }

    int32_t pl_x = -goal_dz, pl_z =  goal_dx;   /* slide left  */
    int32_t pr_x =  goal_dz, pr_z = -goal_dx;   /* slide right */
    int32_t goal_px = h->x + goal_dx;
    int32_t goal_pz = h->z + goal_dz;

    if (blocked && h->steer_timer <= 0) {
        int32_t lx, lz, rx, rz;
        had_feeler(h, pl_x, pl_z, &lx, &lz);
        had_feeler(h, pr_x, pr_z, &rx, &rz);

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
        h->steer_timer--;
    }

    /* ---- The heading, blended, AND IT IS ALL DONE IN 1/256s ----------------
       >>> THIS USED TO RUN AT A THIRD OF `speed` AND THAT IS WHY THE CHASE WAS
       SLOW. <<< Both halves of the arithmetic below used to work in WHOLE world
       units, where the only values available at HAD_SPEED are -4..4, and both
       lost most of the speed to integer division:

         THE NORMALISE  divided by the MANHATTAN sum, so a diagonal step
                        travelled `speed` in |x| + |z| rather than along the
                        line — 2.83 of a wanted 4 at 45 degrees — and then
                        truncated what was left: a heading of (100, 40) came out
                        (2, 1), a step of 2.24 against a speed of 4.

         THE EASE       (prev * 5 + want * 3) >> 3 TRUNCATES, and on numbers this
                        small the recurrence has fixed points far short of what
                        it is easing toward. With want = 2 the value 1 is stable
                        ((1*5 + 2*3) >> 3 == 1) and so is 0 ((0*5 + 6) >> 3 ==
                        0) — a component being eased toward 2 from a standing
                        start NEVER LEAVES ZERO. That is the whole of it: on any
                        heading whose components are 2 and 2 he crawled, and he
                        could not straighten up either.

       This is the same trap had_march was written to escape (see the long note
       on it), and the seeded `facing` in had_arrive is another patch over the
       same hole. It is now fixed at the source instead: the normalise is RADIAL
       like had_march's, the velocity is carried at HAD_STEER_FRAC to the unit so
       the ease has somewhere to converge, and the sub-unit remainder is banked
       rather than thrown away — so a chase covers exactly `speed` a frame in
       every direction, which is what the marched legs cover and what
       HAD_STEP_FRAMES is matched to. */
    int32_t d = had_isqrt(desired_x * desired_x + desired_z * desired_z);
    if (d <= 0) d = 1;
    int32_t want_vx = (desired_x * speed * HAD_STEER_FRAC) / d;
    int32_t want_vz = (desired_z * speed * HAD_STEER_FRAC) / d;

    h->steer_vx = (h->steer_vx * (8 - HAD_TURN_RATE) + want_vx * HAD_TURN_RATE) >> 3;
    h->steer_vz = (h->steer_vz * (8 - HAD_TURN_RATE) + want_vz * HAD_TURN_RATE) >> 3;

    /* Bank the fraction. The shift FLOORS on negatives and the subtraction uses
       the same floored value, so the remainder stays in [0, HAD_STEER_FRAC) on
       both signs and nothing drifts either way over a long walk. */
    h->steer_fx += h->steer_vx;
    h->steer_fz += h->steer_vz;
    int32_t blend_x = h->steer_fx >> HAD_STEER_SHIFT;
    int32_t blend_z = h->steer_fz >> HAD_STEER_SHIFT;
    h->steer_fx -= blend_x << HAD_STEER_SHIFT;
    h->steer_fz -= blend_z << HAD_STEER_SHIFT;

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
   and the stalker music when it has finished. Seeds the STEERING VELOCITY
   toward the player as well, because had_steer eases the new heading into the
   old one and a body starting from rest would spend the first half-second of
   the arrival easing up to speed — exactly the wrong moment to look sluggish.
   (This used to seed `facing`, which is what the ease read before it had a
   fixed-point velocity of its own; the seed is still worth having, but it is no
   longer covering for an ease that could not converge. See had_steer.) */
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
        int32_t ad  = had_isqrt(adx * adx + adz * adz);
        h->steer_fx = h->steer_fz = 0;
        if (ad > 0) {
            h->steer_vx = (adx * HAD_SPEED * HAD_STEER_FRAC) / ad;
            h->steer_vz = (adz * HAD_SPEED * HAD_STEER_FRAC) / ad;
            h->facing   = ((int32_t)(int16_t)(h->steer_vx >> HAD_STEER_SHIFT) << 16) |
                          (uint16_t)(int16_t)(h->steer_vz >> HAD_STEER_SHIFT);
        } else {
            h->steer_vx = h->steer_vz = 0;
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

   >>> AND DROPPING THE BLEND IS THE POINT, NOT A SIDE EFFECT. <<< An authored
   leg is a straight line between two chosen points and wants to be walked as
   one: an ease, however well behaved, bows the start of every leg off the line
   and arrives late, and on a path threaded between hedges by hand that is a
   change to geometry somebody measured.

   >>> IT WAS ALSO, ONCE, THE ONLY WAY TO GET FULL SPEED OUT OF HIM. <<< Kept
   here because it is the history of both movement paths. had_steer used to ease
   in WHOLE world units — (prev * 5 + want * 3) >> 3, on values that at
   HAD_SPEED can only be -4..4 — and that shift TRUNCATES, so the recurrence had
   integer fixed points well short of the value it was easing toward. Turning a
   standing start east (prev 0, want +4) it went 1, 2, 2, 2... — (2*5 + 4*3) >> 3
   is 22 >> 3, which is 2 — and stuck there at HALF SPEED for ever. The
   component being turned AWAY from was worse: from -4 with a want of 0 it went
   -3, -2, -2..., and -2 is a fixed point too, because -10 >> 3 floors to -2. So
   a Hadad who turned a corner crabbed off diagonally at half pace and NEVER
   straightened up.

   That is exactly what the West Corridor's second leg did. He turned east at
   the corner and from then on moved (+2, -2) a frame: half speed east, and a
   permanent southward drift into a wall only 25 units clear of his 300-unit
   body. The wall push fought the drift every single frame, which is what made
   him judder — the wall was the SYMPTOM, and it was also the only thing keeping
   him anywhere near his own path. Removing wall collision alone would have
   stopped the juddering and walked him quietly out of the room instead.

   The ease itself is FIXED now (HAD_STEER_FRAC), which is what finally gave the
   Rear Gate's pursuit its full HAD_SPEED — see the note in had_steer. This
   function stays for the reason in the paragraph above it, not for that one. */
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

    /* ...and so is the STEERING VELOCITY, which this function does not use and
       had_steer starts from. The one place it matters is the Rear Gate's
       flag-three handover: he marches the whole length of the room and then
       takes up the pursuit mid-stride (had_leg_entered), and an ease that began
       from rest there would have him visibly slow down at the very moment he
       stops being a scripted walk and starts coming after you. Taken from the
       exact quotient rather than from the truncated step, so the seed is the
       speed the leg was MEANT to run at. */
    h->steer_vx = (goal_dx * speed * HAD_STEER_FRAC) / d;
    h->steer_vz = (goal_dz * speed * HAD_STEER_FRAC) / d;
    h->steer_fx = h->steer_fz = 0;

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
   Rear Gate's FLAG-ONE leg says NO and keeps had_steer: that one threads the
   corridor's north mouth and its side hedges with a 300-unit body, its seeded
   facing already points along the goal (so the blend above is stable for it,
   which is why that walk never showed this bug), and it is not what the user
   reported.

   >>> THE FLAG-THREE CLIMB SAYS YES, AND IT IS THE ONE CASE THAT NEEDS TO. <<<
   Same role, same room, and it is the leg that was getting him stuck: 750 units
   of RAMP between its rails, then the 1800-wide stretch where the two dead-end
   courts open off it, then the corridor, then the mouth. It runs from
   HAD_RAMPTOP to HAD_CLIMB with both ends on x = 0, so every point of it is on
   the room's centre line and there is nothing to steer around — while what
   steering DOES there is trip its feeler on a rail, commit to a wall-follow, and
   walk him into a court. Marching also means the closing plates cannot shove him
   back off the strip that kills him. This is why the test takes the whole Hadad
   and not just the role. */
static int had_path_is_open(const Hadad *h) {
    if (h->role != HAD_ROLE_PLINTH) return 1;
    return h->stage == HAD_PLINTH_STAGE_CLIMB;
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
    /* ---- The West Corridor's RETURN: the same L, the other way round --------
       Under FLAG_HADAD_TWO he comes in at the single door instead of the double
       one and stops at the double door instead of the single one, so this is
       the table above with its two entries swapped and nothing else. The corner
       is still leg 0 and still an authored waypoint, for the reason it is one
       going the other way: a lone goal at the far door aims him through a wall
       for the whole first leg. */
    if (h->role == HAD_ROLE_WEST_CORR_RET) {
        switch (h->leg) {
        case 0: *gx = HAD_WC_CORNER_X;  *gz = HAD_WC_CORNER_Z;  return 1;
        /* NOT HAD_WC_START, which is the ambush's appearance point 450 further
           north: he stops SHORT, in front of the fat door's gap. See
           HAD_WC_RET_END_Z for why that still shuts the double door. */
        case 1: *gx = HAD_WC_RET_END_X; *gz = HAD_WC_RET_END_Z; return 1;
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
    /* ---- The Reception's FOUR STAGES, indexed by (stage, leg) --------------
       Stage 0 is the arrival: he lands out of the ceiling in front of the
       north-west door and walks one diagonal leg to the head of the stair.
       Reaching it latches the SCENARIO (had_leg_entered) into `stage`.

       Stage 1 is scenario 1 — the player went down the stair, so he follows it:
       south down the upper flight onto the landing, then west down the lower
       flight and out onto the ground floor.

       Stage 2 is scenario 2 — the player did NOT take the stair. It is a LOOP
       in its own right and not a run-up to stage 3: he walks the walkway east
       to the East Hall door (leg 0), stands for a second (HAD_RC_EDGE_PAUSE,
       set by had_leg_entered as leg 1 is taken up), walks south off the
       unguarded stretch of the second level's edge and drops to the ground
       (leg 1), on to the front of the Kitchen Dining door (leg 2), south into
       the south-east corner (leg 3), a square 90 degrees west to the point
       directly south of the stair (leg 4), north-west to its foot (leg 5), UP
       both flights (legs 6-7) and back out along the walkway, for ever. Leg 8
       does not exist — had_leg_entered wraps it to 0.

       >>> IT IS STAGE 3 RUN BACKWARDS, AND THAT IS THE WHOLE POINT OF IT. <<<
       The two loops go round the same room in opposite directions and differ in
       exactly one move: where the circuit LEAPS from the Kitchen Dining door up
       onto the second level, this one WALKS OFF the edge above it. A player who
       leaves the balcony by the stair gets one; a player who drops off its edge
       gets the other.

       Stage 3 is THE CIRCUIT, and it is the same seven legs for ever: down and
       round the stair to the Kitchen Dining door (0-2), the LEAP from it up
       onto the second level (3), west along the balcony to the head of the
       stair (4), and back down the stair to HAD_RC_FOOT (5-6). Leg 7 does not
       exist — had_leg_entered wraps it to 0 — so this table never returns 0 and
       he is never rooted. NEITHER LOOP READS THE PLAYER: the scenario latch at
       the head of the stair is the only run-time decision in the role. */
    if (h->role == HAD_ROLE_RECEPTION) {
        if (h->stage == 0) {
            if (h->leg == 0) { *gx = HAD_RC_STAIRTOP_X; *gz = HAD_RC_STAIRTOP_Z; return 1; }
            return 0;
        }
        if (h->stage == 1) {
            switch (h->leg) {
            case 0: *gx = HAD_RC_LANDING_X; *gz = HAD_RC_LANDING_Z; return 1;
            case 1: *gx = HAD_RC_FOOT_X;    *gz = HAD_RC_FOOT_Z;    return 1;
            default: return 0;
            }
        }
        if (h->stage == 2) {
            switch (h->leg) {
            case 0: *gx = HAD_RC_FOLLOW_X;   *gz = HAD_RC_FOLLOW_Z;   return 1;
            case 1: *gx = HAD_RC_EDGE_X;     *gz = HAD_RC_EDGE_Z;     return 1;
            case 2: *gx = HAD_RC_RDOOR_X;    *gz = HAD_RC_RDOOR_Z;    return 1;
            case 3: *gx = HAD_RC_CORNER_X;   *gz = HAD_RC_CORNER_Z;   return 1;
            case 4: *gx = HAD_RC_SOUTH_X;    *gz = HAD_RC_SOUTH_Z;    return 1;
            case 5: *gx = HAD_RC_FOOT_X;     *gz = HAD_RC_FOOT_Z;     return 1;
            case 6: *gx = HAD_RC_LANDING_X;  *gz = HAD_RC_LANDING_Z;  return 1;
            /* Leg 7 ends at the head of the stair and had_leg_entered wraps 8
               back to 0, so `default` is unreachable — it is here for stage 3's
               reason: a table that falls off its own end roots him silently,
               and that failure would look like the loop simply stopping. */
            default: *gx = HAD_RC_STAIRTOP_X; *gz = HAD_RC_STAIRTOP_Z; return 1;
            }
        }
        /* stage 3: the circuit. */
        switch (h->leg) {
        case 0: *gx = HAD_RC_SOUTH_X;    *gz = HAD_RC_SOUTH_Z;    return 1;
        case 1: *gx = HAD_RC_CORNER_X;   *gz = HAD_RC_CORNER_Z;   return 1;
        case 2: *gx = HAD_RC_RDOOR_X;    *gz = HAD_RC_RDOOR_Z;    return 1;
        case 3: *gx = HAD_RC_JUMP_X;     *gz = HAD_RC_JUMP_Z;     return 1;
        case 4: *gx = HAD_RC_STAIRTOP_X; *gz = HAD_RC_STAIRTOP_Z; return 1;
        case 5: *gx = HAD_RC_LANDING_X;  *gz = HAD_RC_LANDING_Z;  return 1;
        /* Leg 6 ends on HAD_RC_FOOT and had_leg_entered wraps 7 back to 0, so
           `default` is unreachable — it is here because a table that can fall
           off its own end silently roots him, and that failure would look like
           the loop simply stopping. */
        default: *gx = HAD_RC_FOOT_X;    *gz = HAD_RC_FOOT_Z;     return 1;
        }
    }
    /* ---- The Rear Gate's two walks, told apart by `stage` -------------------
       The plinth role has two encounters in one room and they walk the same
       corridor in opposite directions, so `stage` is the cursor that says which
       — the same job it does for the Library and the Reception. It is the FLAG
       that decides which encounter is running, but the flag is not read here:
       `stage` is latched once at the arrival (the HAD_ABSENT branch), so a debug
       flag thrown mid-walk cannot swing his goal from one end of the room to
       the other under him.

       STAGE 0, flag one: the original single leg. In at the corridor's north
       mouth, south to HAD_END at the bottom of it, root there for ever.
       Unchanged, down to the leg number.

       STAGE 1, flag three: in at the ramp top, ONE leg north to HAD_CLIMB, out
       past the corridor's north mouth and a little way onto the lawn — the whole
       length of the room, 4450 units of it. He does NOT root at the end:
       had_leg_entered turns `follow` on as the leg is walked out and the pursuit
       takes over from there. Both ends are on x = 0, which is what lets it be
       one leg and a march (had_path_is_open).

       >>> THE MARCH IS THE KILL WINDOW, BECAUSE IT GOES STRAIGHT THROUGH THE
       PLATES. <<< The player throws the lever while he is between them on his
       way past, and THAT FRAME is the whole of it: grinder_puzzle.c takes the
       verdict once, at the throw, against a band a little wider than the plates
       themselves (GP_KILL_Z_MIN/MAX). The pair being wide open at that moment is
       exactly right — the scene then watches the whole 5.3 s of travel shut on
       him. The strip is 400 deep and HAD_SPEED is 4, so he is inside the band
       for about 175 frames.

       >>> AND THROWING IT EARLY IS A MISS, NOT A DELAYED HIT. <<< The test used
       to run on every frame of the travel, which meant `shut` plates went on
       catching him long after they had stopped moving: a player who threw the
       lever the moment he appeared at the ramp top got the kill anyway, a
       minute later, by him walking into a wall. He now LEAPS a machine he finds
       already closed (had_vault_step, and the leap block in hadad.h) and the
       lever cannot be thrown a second time — grinder_puzzle.c breaks it on the
       throw under this flag.

       Worth knowing, because it is why the window is where it is: once he is
       FOLLOWING he stops HAD_HOLD_DIST short on the side he approached from, and
       from the lawn that is always the north side of anybody standing at the
       lever, while the plates are south of it. The pursuit is the chase; the
       march is the chance. */
    if (h->stage == HAD_PLINTH_STAGE_CLIMB) {
        if (h->leg == 0) { *gx = HAD_CLIMB_X; *gz = HAD_CLIMB_Z; return 1; }
        return 0;
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
    /* The Reception circuit's LEAP. Four times a walk, because the arc has to
       carry him over the second level's edge before gravity has taken the
       upward kick back off him — see HAD_RC_JUMP_VY for the frame arithmetic
       that ties this number to that one. */
    if (h->role == HAD_ROLE_RECEPTION && h->stage == 3 && h->leg == 3) {
        *speed       = HAD_RC_JUMP_SPEED;
        *step_frames = HAD_RC_JUMP_STEP_FRAMES;
        return;
    }
    *speed       = HAD_SPEED;
    *step_frames = HAD_STEP_FRAMES;
}

static void had_leg_entered(Hadad *h) {
    /* ---- The Rear Gate's flag-three climb hands over to the pursuit --------
       He has reached HAD_MOUTH, the top of the corridor. From here he chases,
       and the caller's "no next leg, so root him" rule must not fire — see the
       `follow` test beside it. This is the only leg in the game that ends in a
       handover rather than in another waypoint or in HAD_ROOTED. */
    if (h->role == HAD_ROLE_PLINTH) {
        if (h->stage == HAD_PLINTH_STAGE_CLIMB && h->leg == 1) h->follow = 1;
        return;
    }
    if (h->role == HAD_ROLE_LIBRARY) {
        if (h->stage != 1 || h->leg != 2) return;
        if (h->branch) return;
        h->branch = (player_z() > HAD_LD_BRANCH_Z) ? 1 : 2;
        return;
    }

    /* ---- Everything the Reception's cursor does between legs ---------------
       Four things, in the order they can happen: the SCENARIO latch at the head
       of the stair, the one-second PAUSE before each step off the balcony, the
       handover from SCENARIO 1 onto the circuit, and the two wraps that close
       the two loops (plus, in the circuit, the leap's upward kick). The first
       two of those move `stage`, which is this role's cursor, and reset `leg`
       to 0 so the new stage starts from the top of its own table.

       The latch runs HERE rather than in had_path_goal for the reason the
       Library Destroyed's branch does: solved every frame, the goal would swing
       from one end of the room to the other as the player crossed the line, and
       he would pivot on the waypoint instead of committing. It is also the ONLY
       thing in this role that reads the player at all — neither loop does.

       player_y(), like every other read of the player in this file, and NEVER
       cam_* — the quake that opens this encounter anchors the player and flies
       the camera off the spot (camera.h). */
    if (h->role != HAD_ROLE_RECEPTION) return;

    /* Stage 0 walked out at the head of the stair: which scenario is it?

       >>> HEIGHT ALONE CANNOT ANSWER THIS, AND THAT IS WHY SCENARIO 2 NEVER
       USED TO RUN. <<< The two scenarios are "they went down the STAIR" and
       "they dropped off the BALCONY EDGE", and both of those end with the
       player standing on the ground floor at exactly the same eye height. This
       used to be `player_y() > HAD_RC_DESCEND_Y ? 1 : 2` — a pure height test —
       so a player who dropped off the edge read as a player who had taken the
       stair, and the only way to reach scenario 2 at all was to still be up on
       the balcony on the one frame this ran. In practice they never were: the
       leg from the ceiling to the stair head is 1735 units at HAD_SPEED, close
       to seven seconds, and a player being walked at by that has left.

       So the descent is measured by HOW rather than by WHERE: `branch` is the
       sticky record of the player having been on one of the stair's two ramps
       (set every frame of stage 0 in update_hadads), and it is the only thing
       that tells the two exits apart. Still up top when this runs? Scenario 2 as
       well — its first leg walks the walkway after them, which is exactly what
       that case wants, and it is where the drop leads anyway. */
    if (h->stage == 0 && h->leg == 1) {
        int descended = player_y() > HAD_RC_DESCEND_Y;
        h->stage = (descended && h->branch) ? 1 : 2;
        h->leg   = 0;
        return;
    }

    /* SCENARIO 2, leg 1 taken up: he has walked the walkway out to the East
       Hall door and now stands for a second before turning off the edge. Every
       lap, not just the first — the loop comes back round to leg 0. */
    if (h->stage == 2 && h->leg == 1) {
        h->pause_timer = HAD_RC_EDGE_PAUSE;
        return;
    }

    /* SCENARIO 2's own wrap: leg 7 walks him back up to the head of the stair
       and the next one is the walkway again. He is never rooted in this loop
       either, and it never joins the circuit — see had_path_goal. */
    if (h->stage == 2) {
        if (h->leg > 7) h->leg = 0;
        return;
    }

    /* SCENARIO 1 walked out at the foot of the stair: onto the circuit, which
       nothing ever takes him off again. */
    if (h->stage == 1 && h->leg == 2) {
        h->stage = 3;
        h->leg   = 0;
        return;
    }

    if (h->stage != 3) return;

    /* THE LEAP, taken up at the Kitchen Dining door. All it needs is the
       upward kick: apply_ddog_height integrates it from the next frame, and
       had_path_pace gives this one leg the speed that carries the arc. Set
       HERE and not in had_path_goal because it must happen exactly once, on
       the frame the leg is entered — that function is asked for the goal every
       frame the leg runs. */
    if (h->leg == 3) { h->vy = HAD_RC_JUMP_VY; return; }

    /* ...and round again. The circuit has no end and he is never rooted in
       this room. */
    if (h->leg > 6) h->leg = 0;
}

/* THE LEAP, one frame of it. Called from the marching branch below and from
   nowhere else, with the leg's pace in hand so it can take the pace over for as
   long as he is in the air.

   Three frames matter and they are the three branches: the one he crosses
   HAD_VAULT_Z on, which kicks him off the ground; every frame after, which is
   just a faster march with gravity under it; and the one apply_ddog_height puts
   his feet back down on, which hands the walk back at HAD_SPEED. Nothing here
   touches h->z — had_march still does all the moving, exactly as it does for
   the rest of the climb. */
static void had_vault_step(Hadad *h, int32_t *speed, int *step_frames) {
    if (h->vault == HAD_VAULT_PENDING) {
        /* Still walking up to the take-off line. He may already be past it when
           the lever is thrown (the kill band's south edge is north of it), in
           which case this passes on the very first frame and he goes up from
           where he stands — which is still south of the plates. */
        if (h->z < HAD_VAULT_Z) return;
        h->vault = HAD_VAULT_AIRBORNE;
        h->vy    = HAD_VAULT_VY;
    } else if (h->vy == 0) {
        /* Down. `vy` is zeroed by apply_ddog_height on the frame his feet meet
           the floor, and that call has already run this frame — so this is the
           landing, not the take-off, and the corridor is flat so there is no
           ramp to make the test lie (see the note beside the Reception's own
           landing test). */
        h->vault = 0;
        sound_play(SFX_RUMBLE);   /* six hundred units of him hitting gravel */
        return;
    }
    *speed       = HAD_VAULT_SPEED;
    *step_frames = HAD_VAULT_STEP_FRAMES;
}

int hadads_grinder_vault(int32_t z_south) {
    int i, latched = 0;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (h->area != current_area || !had_vulnerable(h)) continue;
        /* The march only. A following Hadad has left the authored path behind
           and steers round the room instead, and the leap is a leg of that path
           — see the header. */
        if (h->follow || h->state != HAD_WALK) continue;
        if (h->role != HAD_ROLE_PLINTH || h->stage != HAD_PLINTH_STAGE_CLIMB) continue;
        if (h->z >= z_south) continue;      /* not south of the band after all */
        if (h->vault) continue;             /* one leap; the lever is one-shot */
        h->vault = HAD_VAULT_PENDING;
        latched  = 1;
    }
    return latched;
}

/* ---- THE EJECTION WATCH: OUT OF THE LEVEL IS DEATH --------------------------
   >>> HIS PUSH CAN POST THE PLAYER THROUGH A WALL, AND NOTHING BRINGS THEM
   BACK. <<< Two of his moves teleport the player rather than steer them:
   hadads_collide sets them flat to HAD_PUSH_RADIUS + the wall standoff off his
   centre — 495 units, in one frame, from wherever they were — and the blow adds
   a HAD_KNOCKBACK of 200 on top, which update_camera then spends in a single
   step before the room's walls get a look. The wall pass that follows is
   collide_wall_frontonly_y, and it returns early on a NEGATIVE dot: a body
   already behind a face is not pushed, ever. So one bad angle against a wall
   ends with the player outside the room with health to spare, free to walk
   around the back of the level for as long as they like.

   The rule is now: IF HE PUTS YOU OUTSIDE THE LEVEL, YOU ARE DEAD. It costs one
   segment test a frame, and only in the rooms he is standing in.

   >>> IT IS TWO TESTS, AND THE FIRST IS THE REAL ONE. <<<
     CROSSED   the settled position at the end of this frame is on the far side
               of a wall face the settled position at the end of the last one
               was in front of (collision_wall_crossed). That is the ejection
               itself, caught on the frame it happens, in any room, whatever
               shape it is — and it cannot be confused with ordinary play,
               because the wall pass guarantees every normal frame ends in FRONT
               of every face it is gated into, and walking round the end of a
               wall (which is what a doorway is) is not a crossing.
     OUTSIDE   the player is beyond the room's own bounding box. A backstop for
               anybody already out — ejected on a frame this did not run, or
               through some gap in the shell the wall data does not describe.
               Legal play never comes near it: the four rooms' outer walls sit
               ON their bounds and hold the player a full standoff inside.

   >>> AND IT IGNORES INFINITE LIFE, WHICH IS THE POINT OF WRITING IT HERE. <<<
   player_hurt() returns without doing anything under DBG_INFINITE_LIFE, so a
   kill routed through it would leave the cheat player exactly where the bug put
   them. This sets the bar to zero and raises game_over by hand — it is not
   damage, it is the level rejecting a position, and no cheat covers that. The
   player asked for this explicitly.

   THREE THINGS INVALIDATE THE WATCH, each because the two samples would
   otherwise not be two steps of one walk:
     - a room change (had_watch_area), so a position in the last room is never
       measured against this room's walls;
     - a camera-locked scene at either end (player_anchor_epoch), because a
       scene may TELEPORT the anchored body — the Library Destroyed's crawl goes
       under the collapsed shelving and stands up on the far side — and it does
       it on frames this function is not called at all;
     - him not being in the room, which is also the whole gate on the feature:
       the set tested is exactly the set hadads_collide pushes with, so if
       nothing here can shove the player, nothing here can kill them for it.

   The sample itself lives at the top of the file, beside the music latches, so
   hadads_reset() can clear it. */

/* Is a Hadad standing in this room in a state that can push the player? The
   same three skips hadads_collide makes, and deliberately no more: ABSENT is
   not in the room and DEAD is rubble, and every other state — the plinth
   statue included — is a solid cylinder. */
static int had_solid_here(void) {
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (!h->active || h->area != current_area) continue;
        if (h->state == HAD_DEAD || h->state == HAD_ABSENT) continue;
        return 1;
    }
    return 0;
}

static void had_kill_ejected(void) {
    /* NOT player_hurt(): see the note above. The bar goes to zero whatever the
       debug menu says. */
    player_health = 0;
    game_over     = 1;
    flash_timer   = 90;
    sound_play(SFX_DIE);
}

static void had_watch_ejection(void) {
    CollisionRoom *r = &current_collision_room;
    int32_t px, py, pz;
    int     epoch = player_anchor_epoch();
    int     fresh;

    /* Already dead, or he is not here: nothing to watch, and the next frame
       that does watch starts a fresh pair. */
    if (game_over || !had_solid_here()) { had_watch_valid = 0; return; }

    px = player_x(); py = player_y(); pz = player_z();

    /* The two samples have to be consecutive frames of the same walk in the
       same room — see the invalidation list above. */
    fresh = had_watch_valid &&
            had_watch_area  == current_area &&
            had_watch_epoch == epoch;

    if (fresh && (px != had_watch_x || pz != had_watch_z) &&
        collision_wall_crossed(had_watch_x, had_watch_y, had_watch_z,
                               px, py, pz)) {
        had_kill_ejected();
        had_watch_valid = 0;
        return;
    }

    /* The backstop. Strictly outside the room's own bounds — no margin, because
       there is nothing to be generous about: the shell IS the bounding box and
       the player is held a standoff inside it. */
    if (px < r->min_x || px > r->max_x || pz < r->min_z || pz > r->max_z) {
        had_kill_ejected();
        had_watch_valid = 0;
        return;
    }

    had_watch_x     = px;
    had_watch_y     = py;
    had_watch_z     = pz;
    had_watch_area  = current_area;
    had_watch_epoch = epoch;
    had_watch_valid = 1;
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

    /* >>> BEFORE ANYTHING ELSE, INCLUDING HIS OWN AI. <<< This reads the
       position the frame settled on — the room branch in update_current_area
       has already run update_camera, the pushes and the walls by the time this
       function is reached — so it sees exactly what the player is standing on
       now, and it must see it before a blow or a leg of a walk changes anything
       about him. See the long note at had_watch_ejection. */
    had_watch_ejection();

    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        /* current_area, NEVER game_state: gating on game_state would freeze the
           whole enemy for as long as the inventory menu is up, letting the
           player pause the chase with Start (tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (!h->active || h->state == HAD_DEAD || h->area != current_area)
            continue;

        /* >>> A DIRECTOR HAS HIM: DO NOTHING AT ALL WITH HIM. <<< Not the AI,
           not the timers, and above all not `music_wanted` below — see the
           block on Hadad.frozen in hadad.h, and the door_anim_active() guard at
           the top of this function, which is the same bug wearing a different
           hat. Skipping him here is what makes the stalker track stop when the
           Rear Gate's plates take him, and it stops it through the one
           mechanism that owns the track rather than racing it. */
        if (h->frozen) continue;

        if (h->hit_timer    > 0) h->hit_timer--;
        if (h->damage_timer > 0) h->damage_timer--;

        int flag_one = game_flag(FLAG_HADAD_ONE);
        int flag_two = game_flag(FLAG_HADAD_TWO);
        int flag_three = game_flag(FLAG_HADAD_THREE);

        /* player_x/y/z, not cam_*: a camera-locked puzzle flies the camera off
           to a fixed shot while the PLAYER stays standing where they were, and
           an enemy that tracked the camera would close on an empty spot
           (camera.h). Enemies keep acting through those puzzles. */
        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - h->x;
        int32_t dz = pz - h->z;
        int32_t dist = had_isqrt(dx * dx + dz * dz);

        /* The vertical half of the blow, and it is the GAP FROM THE EYE TO HIS
           SPRITE'S SPAN — not the distance to its centre. hadads_try_hit calls
           the same quantity `gv` and computes it the same way; the two being
           one measurement is what keeps the axe's reach inside his own at every
           height. See HAD_CATCH_VGAP for why measuring to the centre let him
           strike through the Reception's balcony floor. */
        int32_t htop = h->y + HAD_Y_OFFSET - HAD_HALF_H;
        int32_t hbot = h->y + HAD_Y_OFFSET + HAD_HALF_H;
        int32_t vgap = (py < htop) ? (htop - py)
                     : (py > hbot ? py - hbot : 0);

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
            /* ---- The West Corridor's two encounters ---------------------------
               The AMBUSH's gate is FLAG ONE SET AND FLAGS TWO AND THREE CLEAR.
               Flag three is read the same way round as hadad_lever_locked():
               it REPLACES flag one everywhere in this enemy, so arming the third
               Rear Gate encounter closes this one for good. Flag two closes it
               because it OPENS THE OTHER ONE — see below. The RETURN's gate is
               flag two on its own, and nothing ever shuts it again.

               Both are tested here, live, rather than latched at room entry, so
               a debug flag thrown from the title screen takes effect the way
               every other branch here does.

               The trigger is the corner where the corridor turns, true radial
               so it reads the same coming down the west arm as coming along the
               south one, and it is SHARED — the gates above guarantee at most
               one instance can answer it. Each appears at the far end of the
               room from where he will stop and walks two legs; follow = 0,
               because this is a fixed path and not a pursuit — where he ends up
               must not depend on where the player ran. had_arrive sounds the
               rumble and cues the stalker track, as every Hadad arrival does. */
            if (h->role == HAD_ROLE_WEST_CORR || h->role == HAD_ROLE_WEST_CORR_RET) {
                /* >>> ONE CIRCLE, TWO ENCOUNTERS, AND THE GATES DECIDE WHICH.
                   <<< The ambush wants flag one with flags two and three clear;
                   the RETURN wants flag two, on its own and for ever after. The
                   two conditions cannot both hold, which is the whole reason
                   flag two was added to the ambush's gate — a 600-wide corridor
                   holds one of him, and two armed at once would walk into each
                   other in it. Each appears where the OTHER stops: the ambush at
                   the double door, the return at the single one. */
                int armed = (h->role == HAD_ROLE_WEST_CORR)
                          ? (flag_one && !flag_two && !flag_three)
                          : flag_two;
                if (armed) {
                    int32_t cdx = px - HAD_WC_CORNER_X;
                    int32_t cdz = pz - HAD_WC_CORNER_Z;
                    if (had_isqrt(cdx * cdx + cdz * cdz) <= HAD_WC_TRIG_RADIUS) {
                        if (h->role == HAD_ROLE_WEST_CORR)
                            had_arrive(h, HAD_WC_START_X, HAD_WC_START_Z, 0);
                        else
                            had_arrive(h, HAD_WC_END_X, HAD_WC_END_Z, 0);
                    }
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
            if (h->ramp_armed && pz >= HAD_RAMP_FIRE_Z) {
                /* >>> follow = 0. HE WALKS BEFORE HE CHASES. <<< He used to
                   come off the ramp top already chasing, and a player who did
                   not run straight up the corridor could strand him in the
                   ramp's rails or a court's dead end — with the grinders, the
                   only thing that can kill him, nowhere near his path. He now
                   marches one authored leg up the room's centre line, the
                   whole length of the room to HAD_CLIMB on the lawn
                   (had_path_goal, stage 1), and takes up the pursuit there
                   (had_leg_entered) — on open grass, with the corridor and its
                   two dead-end courts behind him. `stage` is latched HERE, at
                   the arrival, so nothing can swing his goal mid-walk. */
                had_arrive(h, HAD_RAMPTOP_X, HAD_RAMPTOP_Z, 0);
                h->stage = HAD_PLINTH_STAGE_CLIMB;
            }
            break;

        case HAD_WALK:
        case HAD_ROOTED: {
            music_wanted = 1;

            /* >>> WHICH WAY THE PLAYER LEAVES THE BALCONY, WATCHED EVERY FRAME
               OF THE ARRIVAL WALK. <<< The Reception's two scenarios are told
               apart by whether the player used the STAIR or stepped off the
               EDGE, and by the time he reaches the head of the stair — where
               had_leg_entered latches it — both look identical: one player on
               one ground floor. The stair's two flights are the room's only
               FLOOR_RAMP zones, so player_on_ramp is the difference, and it has
               to be caught while it is happening rather than asked for
               afterwards. Sticky, and read once; had_reseat's zero-fill and
               hadad_reception_begin both clear it.

               Ahead of the `vy` gate below on purpose: he spends the first
               forty frames of stage 0 falling out of the ceiling, and a player
               who runs down the stair during them must still count. */
            if (h->role == HAD_ROLE_RECEPTION && h->stage == 0 && player_on_ramp)
                h->branch = 1;

            /* ---- The blow. Horizontal and vertical reach are tested
               SEPARATELY, for the reason every other enemy here does the same:
               the player's eye sits above the body's centre, and a combined
               budget eats the whole allowance vertically so an adjacent enemy
               never lands a hit. The horizontal half is the RADIAL `dist` — see
               HAD_CATCH_DIST for why a Manhattan sum cannot work at this size.

               The vertical half is `vgap`, the gap from the eye to his sprite's
               SPAN, against HAD_CATCH_VGAP — the axe's own SWING_RANGE, so that
               "he can be hit" stays a strict subset of "he can hit back" while
               still being far too tight to cross a storey. On the flat the eye
               is INSIDE his span, so vgap is 0 and this never binds in the
               corridor; it exists for the ramp, and for the two-storey room. */
            if (!game_over && dist < HAD_CATCH_DIST &&
                vgap < HAD_CATCH_VGAP &&
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
            {
                /* >>> HE LANDS WITH A NOISE. <<< Two of the Reception legs end
                   in a real drop — the step off the second level's edge in
                   scenario 2, and the far side of the circuit's leap — and both
                   were asked for with the rumble under them. A landing is the
                   frame a fast downward `vy` is zeroed by the floor;
                   HAD_RC_LAND_VY is what separates that from walking down a
                   stair tread, which also zeroes `vy` but never gets anywhere
                   near it.

                   >>> STAGE 0 IS DELIBERATELY EXCLUDED. <<< The drop out of the
                   ceiling already sounded this clip on the frame he appeared
                   (had_arrive), and SFX_RUMBLE is one voice: a second play 40
                   frames later would cut the arrival's own 127-frame clip in
                   half and leave the appearance thinner than it started. */
                int32_t vy_before = h->vy;
                apply_ddog_height(&h->x, &h->y, &h->z, &h->vy,
                                  &h->on_upper_floor, &h->on_ramp);
                if (h->role == HAD_ROLE_RECEPTION && h->stage != 0 &&
                    vy_before >= HAD_RC_LAND_VY && h->vy == 0)
                    sound_play(SFX_RUMBLE);
            }

            /* >>> THE RECEPTION ARRIVAL FALLS BEFORE IT WALKS. <<< He appears
               with his feet on the ceiling plane and the line above is what
               brings him down; without this he would stride his first leg out
               across 600 units of thin air on the way. STAGE 0 ONLY — scenario
               2's step off the second level's edge is a WALK-off and must keep
               marching through its fall, which is the whole read of it.

               `vy` is zero on every frame apply_ddog_height finds him at or
               below his floor, which on the flat second level is every frame
               after he lands. It is NOT a safe test on a ramp — the target
               moves out from under him each frame there — but stage 0 never
               touches one. */
            if (h->role == HAD_ROLE_RECEPTION && h->stage == 0 && h->vy != 0)
                break;

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
                    if (!h->follow && !had_path_goal(h, &tx, &tz))
                        h->state = HAD_ROOTED;
                } else if (had_path_is_open(h)) {
                    /* ...and if the lever went over while he was still short of
                       the grinders, this is where he jumps them. It only ever
                       takes the pace over for the airborne stretch; the march
                       below is what moves him, on this frame as on every
                       other. */
                    if (h->vault) had_vault_step(h, &leg_speed, &leg_step);
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

/* ---- The Reception director API ---------------------------------------------
   Two functions, and they are everything src/reception_hadad.c is allowed to
   know about this enemy (see the marked block at the foot of hadad.h). */

Hadad *hadad_reception_instance(void) {
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (!h->active || h->role != HAD_ROLE_RECEPTION) continue;
        if (h->area != current_area || h->state == HAD_DEAD) continue;
        return h;
    }
    return NULL;
}

void hadad_reception_begin(Hadad *h) {
    if (!h || h->role != HAD_ROLE_RECEPTION) return;
    h->stage = 0;
    /* The scenario observation starts CLEAR: `branch` is the record of the
       player having been on the stair since the drop, and a value carried in
       from anywhere else would decide the encounter before it began. */
    h->branch = 0;
    had_arrive(h, HAD_RC_CEIL_X, HAD_RC_CEIL_Z, 0);

    /* >>> HE ARRIVES IN THE CEILING, WHICH IS THE ONE THING had_arrive CANNOT
       DO. <<< That function probes the FLOOR for its Y (had_floor_anchor), so
       it has just stood him on the second level; put him back up on the -1200
       plane and let gravity do the rest. `vy` is zeroed with it so the drop
       starts from rest rather than from whatever the last state left behind. */
    h->y  = HAD_RC_CEIL_ANCHOR;
    h->vy = 0;

    /* >>> AND HE DOES NOT SERVE THE WAKE PAUSE. <<< Every other arrival stands
       still for a second first, which reads as the statue noticing; here the
       DROP is the announcement, and a second of hanging in the plaster before
       it would read as a sprite that had failed to spawn. The pause branch in
       update_hadads also returns BEFORE apply_ddog_height, so leaving it set
       would literally suspend him in mid-air for its duration. */
    h->pause_timer = 0;
}

void hadad_wc_return_rearm(void) {
    int i;
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (!h->active || h->role != HAD_ROLE_WEST_CORR_RET) continue;
        /* had_reseat is the whole of it: full health, every scrap of AI state
           cleared, and state = had_rest_state(role) = HAD_ABSENT on his
           appearance point. It does NOT test for HAD_DEAD the way hadads_rest
           does, which is precisely the property being used here — see the
           block on this function in hadad.h. */
        had_reseat(h);
    }
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

/* Crucifaxe strike. Reach is measured to the SURFACE THE PLAYER IS HELD AT, not
   to the body's centre, because the wall-like standoff puts the centre far out
   of the axe's range: the push holds the player HAD_PUSH_RADIUS + 195 = 395 off,
   against a SWING_RANGE of 350. Measured from that surface the same stand gives
   a 195 horizontal gap and, with the player's eye inside the body's vertical
   span, no vertical gap at all — 195 against 350, from every bearing.

   >>> THE SURFACE IS HAD_PUSH_RADIUS AND NOT HAD_BODY_RADIUS. <<< They are two
   different numbers now (see the Solid body note in hadad.h): the art is 600
   wide but the cylinder holding the player is 400, and it is the cylinder the
   axe has to cross. Measuring from the wider one would put the swing's edge
   100 units outside the range he can strike back from.

   >>> THIS FUNCTION AND THE CONTACT TEST IN update_hadads SHARE A NUMBER. <<<
   HAD_CATCH_DIST is defined as HAD_PUSH_RADIUS + SWING_RANGE precisely so that
   the outermost stand this will accept is also the outermost stand he can
   strike from — "the player should never be able to get close enough to hit him
   without also taking damage". Read the note at that constant before changing
   either side of it.

   The horizontal gap is derived from HAD_PUSH_RADIUS, the very constant
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
        int32_t gh = d > HAD_PUSH_RADIUS ? d - HAD_PUSH_RADIUS : 0;

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
           true sqrt (tools/ADDING_A_3D_ENEMY.txt STEP 6).

           HAD_PUSH_RADIUS, not HAD_BODY_RADIUS: the player is allowed inside his
           silhouette now that the body is drawn as a grid that survives being
           looked at from there. See the Solid body note in hadad.h. */
        int32_t dx   = *px - h->x;
        int32_t dz   = *pz - h->z;
        int32_t need = HAD_PUSH_RADIUS + radius;
        int32_t d    = had_isqrt(dx * dx + dz * dz);
        if (d >= need) continue;
        if (d == 0) { *px = h->x + need; continue; }   /* dead centre */
        *px = h->x + (dx * need) / d;
        *pz = h->z + (dz * need) / d;
    }
}

/* Bracket an already-filled RUN OF POLY_FT4s with a full/unmasked texture window
   and a restore of the area's own, all within one OT bucket. Identical to the
   statue's, and needed for the same reason: all three sprites sit at Voff 128.
   addPrim() prepends, so adding restore, then the polys BACK TO FRONT of the
   array, then disable, yields the draw order disable -> poly[0..n-1] -> restore.

   >>> ONE BRACKET FOR THE WHOLE BODY, NOT ONE PER POLYGON. <<< The body is
   HAD_GRID_QUADS quads now (see draw_had_sprite), and every one of them wants
   the same window. Wrapping each separately would put sixteen extra DR_TWINs in
   the bucket and make the GPU re-latch the window between neighbouring pieces of
   the same sprite for nothing. */
static void add_ft4_run_windowed(RenderContext *ctx, int32_t otz,
                                 POLY_FT4 **polys, int n) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;
    int       i;

    if (had_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &had_tw_restore);
        addPrim(&ot[otz], restore);
        ctx->next_packet += sizeof(DR_TWIN);

        for (i = n - 1; i >= 0; i--) addPrim(&ot[otz], polys[i]);

        RECT full = { 0, 0, 0, 0 };   /* mask 0 = no wrapping, full page */
        DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
        setTexWindow(disable, &full);
        addPrim(&ot[otz], disable);
        ctx->next_packet += sizeof(DR_TWIN);
    } else {
        for (i = n - 1; i >= 0; i--) addPrim(&ot[otz], polys[i]);
    }
}

/* One camera-facing body, drawn as a GRID of quads. No mirror flip and no roll,
   so there is deliberately no gte_nclip backface cull — a camera-facing quad has
   no back face to cull (tools/ADDING_AN_ENEMY.txt mistake 4).

   >>> WHY HE IS EIGHT POLYGONS AND NOT ONE. <<< He is the biggest sprite in the
   game: 600 x 1200 world units, twice the Living Statue's area, and a single
   quad that size falls off the hardware twice over as the player closes in.

     - The GPU refuses to draw any primitive whose screen extent exceeds 1023
       pixels in either axis. It does not clip it, it DROPS it, so the body
       blinks out of existence at exactly the distance where it starts to fill
       the screen. That is the "he clips if we get any closer" the old
       HAD_BODY_RADIUS of 300 was really holding the player away from.
     - Texture mapping is affine, so the bigger the quad on screen the more the
       art swims across it.

   Both faults are per-PRIMITIVE, so both are fixed by cutting the body up.
   HAD_GRID_COLS x HAD_GRID_ROWS is 2 x 4, and that is the split that matters:
   he is twice as tall as he is wide, so 2 x 4 makes every piece a SQUARE
   300 x 300 and neither axis runs out of screen before the other. Four times
   the height headroom and twice the width headroom — i.e. the player may come
   at least twice as close as the single quad allowed, which is what pays for
   the tightened HAD_BODY_RADIUS.

   The grid is SHARED VERTICES, not eight loose quads: 3 x 5 = 15 points
   transformed once each and indexed four ways. Neighbours therefore project to
   bit-identical screen positions and cannot crack apart along the seams, and
   the cost is 16 GTE transforms rather than the 32 loose quads would take. */
#define HAD_GRID_COLS   2
#define HAD_GRID_ROWS   4
#define HAD_GRID_STRIDE (HAD_GRID_COLS + 1)                            /* 3  */
#define HAD_GRID_QUADS  (HAD_GRID_COLS * HAD_GRID_ROWS)                /* 8  */
#define HAD_GRID_VX     (HAD_GRID_STRIDE * (HAD_GRID_ROWS + 1))        /* 15 */

static void draw_had_sprite(RenderContext *ctx, Hadad *h, const Sprite *sp) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);

    int32_t cx = h->x, cz = h->z, cy = h->y + HAD_Y_OFFSET;

    /* THE SQUASH (see Hadad.squash). Normally a no-op: squash is 0 for every
       Hadad in the game except the one in the Rear Gate's grinders, and both
       expressions below collapse to HAD_HALF_W and HAD_HALF_H at 0.

       The width scales DOWN by (256 - squash)/256 and the height UP by half of
       what the width lost, so a body squeezed to nothing between the plates
       gets half again as tall instead of merely thinner. The BOTTOM edge is the
       one that is held: vy_bot stays at the feet and the extra height is all
       taken off the top, because the grinder plates cannot push him into the
       gravel and a body growing downward would sink through it. */
    int32_t half_w = (HAD_HALF_W * (256 - h->squash)) / 256;
    int32_t half_h = HAD_HALF_H + (HAD_HALF_H * h->squash) / 512;

    int16_t dwx = (int16_t)((half_w * rx) >> 12);
    int16_t dwz = (int16_t)((half_w * rz) >> 12);
    int16_t vy_bot = (int16_t)(cy + HAD_HALF_H);
    int16_t vy_top = (int16_t)(vy_bot - 2 * half_h);

    /* The grid, row-major from the top-left: index r * HAD_GRID_STRIDE + c.
       Column c runs the half-width offset from -1 through 0 to +1 along the
       camera-facing axis, row r walks vy_top down to vy_bot. The four corners
       are therefore 0, 2, 12 and 14. */
    SVECTOR v[HAD_GRID_VX];
    int     r, c;
    for (r = 0; r <= HAD_GRID_ROWS; r++) {
        int32_t y = vy_top + ((vy_bot - vy_top) * r) / HAD_GRID_ROWS;
        for (c = 0; c <= HAD_GRID_COLS; c++) {
            int32_t  k = c * 2 - HAD_GRID_COLS;   /* -2, 0, +2 over 2 columns */
            SVECTOR *p = &v[r * HAD_GRID_STRIDE + c];
            p->vx  = (int16_t)(cx + (dwx * k) / HAD_GRID_COLS);
            p->vy  = (int16_t)y;
            p->vz  = (int16_t)(cz + (dwz * k) / HAD_GRID_COLS);
            p->pad = 0;
        }
    }

    DVECTOR sv[HAD_GRID_VX];
    DVECTOR out[3];
    int32_t sz[4], otz;

    /* The four CORNERS first, in the order the single quad used, so the OT sort
       is computed from exactly the same four depths it always was and the
       behind-the-camera reject still costs nothing extra. Every other vertex of
       a camera-facing billboard lies between the corners in depth, so testing
       the corners tests the body. */
    gte_ldv3(&v[0], &v[2], &v[12]);
    gte_rtpt();
    gte_stsxy3c(out);
    sv[0] = out[0]; sv[2] = out[1]; sv[12] = out[2];
    gte_ldv0(&v[14]);
    gte_rtps();
    gte_stsxy(&sv[14]);
    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return;

    gte_avsz4();
    gte_stotz(&otz);
    /* Sort on the room-geometry scale (raw average Z) so hedges between the
       camera and the body occlude it, with the SCENE_OT_MIN clamp every sprite
       here takes. ONE bucket for all eight pieces: they are coplanar and do not
       overlap, so there is nothing for them to sort against each other. */
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
    if (ctx->next_packet + HAD_GRID_QUADS * sizeof(POLY_FT4) > buf_end) return;

    /* The other eleven vertices, in batches of three. The last batch is one
       short, so it repeats index 13: the duplicate writes the same screen point
       into the same slot twice, which is cheaper than a special case for it. */
    static const uint8_t rest[12] = { 1, 3, 4,  5, 6, 7,  8, 9, 10,  11, 13, 13 };
    int b;
    for (b = 0; b < 4; b++) {
        const uint8_t *ix = &rest[b * 3];
        gte_ldv3(&v[ix[0]], &v[ix[1]], &v[ix[2]]);
        gte_rtpt();
        gte_stsxy3c(out);
        sv[ix[0]] = out[0]; sv[ix[1]] = out[1]; sv[ix[2]] = out[2];
    }

    /* The texture is cut on the same grid. u0..u1 and v0..v1 are INCLUSIVE texel
       bounds (load_owned_sprite already insets them by one), so neighbouring
       pieces SHARE the seam texel rather than each taking half of it: sharing
       samples one line twice at a magnification where that cannot be seen,
       while splitting it leaves a hairline of background between them. */
    uint8_t uc[HAD_GRID_COLS + 1], vr[HAD_GRID_ROWS + 1];
    for (c = 0; c <= HAD_GRID_COLS; c++)
        uc[c] = (uint8_t)(sp->u0 + ((sp->u1 - sp->u0) * c) / HAD_GRID_COLS);
    for (r = 0; r <= HAD_GRID_ROWS; r++)
        vr[r] = (uint8_t)(sp->v0 + ((sp->v1 - sp->v0) * r) / HAD_GRID_ROWS);

    POLY_FT4 *quads[HAD_GRID_QUADS];
    int       n = 0;
    for (r = 0; r < HAD_GRID_ROWS; r++) {
        for (c = 0; c < HAD_GRID_COLS; c++) {
            int tl = r * HAD_GRID_STRIDE + c;
            int tr = tl + 1;
            int bl = tl + HAD_GRID_STRIDE;
            int br = bl + 1;

            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            if (h->hit_timer > 0) setRGB0(poly, fog8, fog8 >> 2, fog8 >> 2);
            else                  setRGB0(poly, fog8, fog8, fog8);

            poly->x0 = sv[tl].vx; poly->y0 = sv[tl].vy;
            poly->x1 = sv[tr].vx; poly->y1 = sv[tr].vy;
            poly->x2 = sv[bl].vx; poly->y2 = sv[bl].vy;
            poly->x3 = sv[br].vx; poly->y3 = sv[br].vy;

            poly->u0 = uc[c];     poly->v0 = vr[r];
            poly->u1 = uc[c + 1]; poly->v1 = vr[r];
            poly->u2 = uc[c];     poly->v2 = vr[r + 1];
            poly->u3 = uc[c + 1]; poly->v3 = vr[r + 1];

            poly->tpage = sp->tpage;
            poly->clut  = sp->clut;

            ctx->next_packet += sizeof(POLY_FT4);
            quads[n++] = poly;
        }
    }
    add_ft4_run_windowed(ctx, otz, quads, n);

    if (h->hit_timer <= 0) return;

    /* The health bar hangs off the body's TOP CORNERS, which are grid vertices
       0 and 2 now rather than the old quad's 0 and 1. */
    int16_t bar_cx  = (sv[0].vx + sv[2].vx) / 2;
    int16_t bar_top = (sv[0].vy < sv[2].vy ? sv[0].vy : sv[2].vy) - 8;
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
