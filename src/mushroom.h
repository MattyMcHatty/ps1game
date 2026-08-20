#ifndef MUSHROOM_H
#define MUSHROOM_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState — each mushroom is tagged with its area */

/* -----------------------------------------------------------------------
 * Mushroom Head — the garden's ambusher.
 *
 * Built on the spider (src/spider.c, itself built on the zombie): a camera-
 * facing textured quad, feeler-based steering with a wall-follow commit,
 * enemy-vs-enemy separation, knockback, a floor shadow and a health bar. What
 * it adds on top is a five-state ATTACK RITUAL, and the ritual is the enemy:
 *
 *   PACE    Unalerted, it walks a fixed two-point patrol at half the player's
 *           walk speed. The two points are AUTHORED per placement (see
 *           mushroom_add) rather than derived, exactly as a spider's ceiling
 *           height is, so world.c can rebuild it from a save without the room's
 *           geometry resident.
 *   SCREAM  Being NOTICED — see MSH_ALERT_RADIUS for the three tests — or
 *           taking a hit from anything, which bypasses all three because being
 *           hit IS being found. It turns to face the player and freezes for
 *           half a second on the closed cap, then the cap splits (SFX_HISS)
 *           and it holds the open
 *           frame. The instant the count is up it LEAPS — it does not step in
 *           first, and nothing can stall it (see MSH_SCREAM_TOTAL).
 *   LEAP    A ballistic hop along a straight line to a point MSH_LEAP_STANDOFF
 *           short of the player, arcing MSH_LEAP_HEIGHT into the air.
 *           >>> IT IGNORES WALLS ENTIRELY. <<< No collision, no feelers, no
 *           floor probe — the arc is interpolated between two fixed endpoints.
 *           That is deliberate and is the whole point of the "hides behind a
 *           wall" case below; the frame it lands, the ordinary floor and wall
 *           passes take over again and push it out of anything it landed in.
 *   CHASE   Closes at MSH_CHASE_SPEED, which sits between the player's walk
 *           (12) and their sprint (20): walking away never works, sprinting
 *           away does. In contact it deals a quarter of MAX_HEALTH per second.
 *   WAIT    Sprinting away only works if it also loses SIGHT of you. When the
 *           sightline breaks it stops dead for MSH_WAIT_FRAMES, then screams
 *           and leaps again — through the wall it lost you behind. Stepping
 *           back into view cancels the wait and it resumes the chase.
 *
 * 5 HP and NO weaknesses: five crucifaxe swings or five rounds of any kind.
 * The axe additionally shoves it MSH_KNOCKBACK back, which is a real reprieve
 * (~360 units all told) and no more than that — it closes that gap in about
 * two seconds.
 *
 * TEXTURES. Four, and they own their VRAM slots outright rather than time-
 * sharing anyone's, which is why they are a plain startup LoadImage and never
 * need re-streaming on a transition. They are 96x96 rather than the 128x128
 * every other sprite enemy here uses, because VRAM has no 128-row hole left at
 * all (tools/VRAM_MAP.txt) and 96 rows fit the band under the HUD. All four sit
 * at Voff 128, so every quad brackets its own texture window.
 *
 *   mushy_run     walking TOWARD the camera        } alternating with their
 *   mushy_behind  walking AWAY from the camera     } own mirror image, which
 *                                                    is the whole walk cycle
 *   mushy_closed  cap shut: the scream's wind-up, and the frame it waits on
 *   mushy_open    cap split: the scream itself, and the leap out of it
 *
 * PERSISTENCE is the spiders' model: ONE global array tagged by area, with
 * every update/draw/weapon loop skipping any instance whose area is not
 * current_area — current_area and NEVER game_state, or the enemy freezes for
 * as long as the inventory menu is open (see tools/ADDING_AN_ENEMY.txt STEP 6).
 * MAX_MUSHROOMS is therefore the budget for the WHOLE GAME, not per room.
 *
 * SOUND. SFX_HISS is in the GARDEN bank only — it does not fit the house
 * bank's 6.6 KB of spare. A mushroom placed inside the house would still work
 * but would scream silently; see src/sound.h before moving one indoors.
 * ----------------------------------------------------------------------- */

#define MAX_MUSHROOMS         4
#define MSH_MAX_HEALTH        5    /* five crucifaxe swings or five rounds     */

/* ---- Speeds, in units per frame. The player walks at 12 and sprints at 20
   (camera.c), and both of those numbers are load-bearing here:
     - PACE is half a walk, so an unalerted mushroom reads as ambling;
     - CHASE sits BETWEEN them, so walking away from an alerted one is hopeless
       and sprinting away is the only answer — which is exactly the escape the
       WAIT state is built to punish. Raise it past 20 and there is no escape at
       all; drop it under 12 and the enemy never lands a second hit. */
#define MSH_WALK_SPEED        6
#define MSH_CHASE_SPEED      15

/* Alert radius, TRUE RADIAL (not the Manhattan sum the zombie's coarse wake
   uses). "About double the Rafflesia's": that flower's engagement range is
   RAF_PULL_RADIUS, 700, so 1400. Note this is well inside the Outside
   Catacombs' 2500 fog-far, so a mushroom never wakes to a player it cannot
   see — which matters, because the wake is what starts a scream the player is
   meant to be able to watch.

   >>> THE RADIUS IS ONLY ONE THIRD OF THE WAKE. <<< An UNALERTED mushroom has
   to actually NOTICE the player, and all three of these must hold at once:

     1. it is walking TOWARD them — the dot of its travel direction with the
        offset to the player is positive, i.e. they are somewhere in the 180
        degrees ahead of it. On the return leg of its patrol its back is turned
        and it can be walked straight past;
     2. they are inside MSH_ALERT_RADIUS;
     3. the sightline is CLEAR of level geometry.

   Point 3 is the ONLY place in this file where a wall stops the enemy seeing:
   once alerted it stops caring, and the leap goes through walls on purpose.
   (The WAIT state's own sightline test is a different question — that one is
   about LOSING a player it has already found.)

   The tests are ordered cheapest first — a squared radius, then a dot, then the
   segment trace — because this runs for every unalerted mushroom every frame. */
#define MSH_ALERT_RADIUS   1400

/* ---- The scream. Frame counts at 60 fps.
   SFX_HISS is 2.00 s / 120 frames and starts when the cap opens, so it is
   still sounding as the leap goes out — the lunge comes out of the middle of
   the scream rather than after it, which is the read asked for.

   >>> THE LEAP IS UNCONDITIONAL AND UNINTERRUPTIBLE. <<< The scream used to
   walk the body forward over its last 30 frames, and that made the whole ritual
   unreliable in two ways at once: the steps carried it up to 180 units closer,
   and mushroom_begin_leap USED TO BAIL OUT (straight into the chase, with no
   jump at all) whenever the player was already inside MSH_LEAP_STANDOFF. Close
   quarters therefore ate the leap exactly when it mattered most. Both are gone:
   there is no step phase, the leap has a floor of MSH_LEAP_MIN_TRAVEL so it
   always happens, and the scream counts down even while the body is being
   knocked back by the axe — a well-timed swing can shove a screaming mushroom
   around but can no longer freeze it mid-wind-up. */
#define MSH_SCREAM_CLOSED    30    /* half a second on the closed cap          */
#define MSH_SCREAM_TOTAL    120    /* ...then 90 more with it open, then leap  */

/* ---- The leap. A straight line between two fixed points with a parabola laid
   over it; nothing about it consults the level.
   STANDOFF is what "close to the player but not too close" means — it lands a
   little over MSH_CATCH_DIST away, so it arrives in the player's face without
   arriving already biting. MAX caps how far one hop can carry it, so a wake at
   the full 1400 alert radius does not cross the whole room in one bound. */
#define MSH_LEAP_STANDOFF   400
#define MSH_LEAP_MAX        900
/* ...and a FLOOR, so a mushroom that finishes its scream already in the
   player's face still pounces instead of quietly walking the last step. At
   point blank there is nothing to stand off from, so it simply lands on them —
   inside MSH_CATCH_DIST, i.e. biting on touchdown. That is the right answer for
   a creature that has just spent two seconds screaming at arm's length. */
#define MSH_LEAP_MIN_TRAVEL 220
#define MSH_LEAP_SPEED       18    /* horizontal units/frame -> the arc's length */
#define MSH_LEAP_MIN_FRAMES  18
#define MSH_LEAP_MAX_FRAMES  60
#define MSH_LEAP_HEIGHT     520    /* peak height of the arc above the ground.
                                      Twice the body's own 250, so it clears its
                                      own height again and comes down on top of
                                      the player rather than skimming in.      */

/* ---- Losing the player. The sightline is tested from the sprite's centre to
   the player's eye; MSH_WAIT_FRAMES is the three seconds it stands there
   before screaming again.
   MSH_REGROUP is a grace period after every landing during which the sight
   test is suppressed. Without it a mushroom that lands with the player still
   behind cover would drop straight back into WAIT and loop scream-leap-scream
   forever without ever getting a swing in. */
#define MSH_WAIT_FRAMES     180
#define MSH_REGROUP          60

/* ---- Contact damage: a quarter of MAX_HEALTH (100) per second, i.e. 5 points
   every 12 frames. Reach is Manhattan and matched to the sprite's own
   footprint, as every other enemy's contact test is. */
#define MSH_CATCH_DIST      200
#define MSH_DAMAGE_AMOUNT     5
#define MSH_DAMAGE_TICK      12

/* ---- Sprite. Square, because the source art is: a hunched body with its arms
   thrown wide fills a square frame. 250 x 250 world units, the same standing
   height as a zombie and twice its width.
   MSH_Y_OFFSET + MSH_HALF_H == 150 is the invariant every sprite enemy here
   keeps — that sum is the drop from the entity anchor to the feet, so growing
   the half-height has to come back out of the offset or the thing floats. */
#define MSH_HALF_W          125
#define MSH_HALF_H          125
#define MSH_Y_OFFSET         25

/* Frames per mirror flip — the gait. Two rates, because one cadence cannot
   serve both speeds: at MSH_WALK_SPEED (6) a 10-frame flip reads as a frantic
   shuffle, while at MSH_CHASE_SPEED (15) it is right. The pacing rate is 2.5x
   slower, so the flip happens 60% less often. Keep the ratio roughly in step
   with the speed ratio if either is retuned. */
#define MSH_ANIM_RATE_PACE   25
#define MSH_ANIM_RATE_CHASE  10
#define MSH_BAR_TIMER_MAX   120

/* Steering and knockback, mirroring the spider's. No nav graph: a mushroom
   fights in the room it patrols. */
#define MSH_SEP_RADIUS      220
#define MSH_SEP_WEIGHT        2
#define MSH_BODY_RADIUS      90
#define MSH_DOOR_CLEARANCE  100
#define MSH_FEELER_LEN      170
#define MSH_TURN_RATE         3
#define MSH_STEER_COMMIT     30
#define MSH_KNOCKBACK        45    /* decays 7/8 a frame -> ~360 units total   */

#define MSH_WAYPOINT_REACH  140    /* Manhattan; flip the patrol leg here      */

#define MSH_SHADOW_W        150
#define MSH_SHADOW_D         70

typedef enum {
    MSH_PACE,     /* unalerted: walking the two-point patrol            */
    MSH_SCREAM,   /* rooted, facing the player, cap closed then open    */
    MSH_LEAP,     /* airborne, on a fixed arc, ignoring the level       */
    MSH_CHASE,    /* closing and biting                                 */
    MSH_WAIT,     /* lost sight: stopped, counting down to another scream */
    MSH_DEAD,
} MushroomState;

typedef struct {
    int32_t       x, y, z;
    /* The patrol. `pa`/`pb` are the two authored points and never change; `y`
       above is the standing anchor, authored too (mushroom_add takes it) so a
       save rebuild needs no floor probe. */
    int32_t       pa_x, pa_z, pb_x, pb_z;
    int32_t       spawn_y;
    int           to_b;          /* 1 = heading for pb, 0 = heading for pa    */

    int32_t       vy;
    int32_t       kb_vx, kb_vz;

    /* The leap's two endpoints and the ground it started from, so the arc is
       pure interpolation and the shadow stays planted while the body is up. */
    int32_t       leap_x0, leap_z0, leap_x1, leap_z1, leap_ground_y;
    int           leap_tick, leap_len;

    int           health;
    int           damage_timer;
    int           hit_timer;
    int           anim_tick;
    int           scream_tick;
    int           wait_tick;
    int           regroup_tick;  /* sight test suppressed while > 0           */

    int32_t       facing;        /* last travel dir, packed: hi16 X, lo16 Z   */
    int           steer_timer;
    int           steer_dir;

    MushroomState state;
    int32_t       active;
    int           on_upper_floor;
    int           on_ramp;
    GameState     area;
} Mushroom;

extern Mushroom mushrooms[MAX_MUSHROOMS];
extern int      mushroom_count;

/* Load the four sprite TIMs into VRAM. Call ONCE at startup — a CdRead is only
   safe before the per-frame render loop begins (tools/TEXTURING_NOTES.txt).
   The slots are this enemy's own, so there is no upload-on-entry counterpart. */
void mushrooms_load_textures(void);

/* Place one, patrolling between (ax,az) and (bx,bz) and standing at anchor `y`.
   Everything is AUTHORED — nothing here reads the current room's mesh — so this
   is safe to call for a room that is not loaded, which is what lets world.c
   rebuild every visited room from a save delta. Returns its index, or -1 if the
   global pool is full. */
int  mushroom_add(int32_t ax, int32_t az, int32_t bx, int32_t bz,
                  int32_t y, GameState area);

void mushrooms_init(void);
void mushrooms_reset(void);
/* Put every still-living mushroom back on patrol point A, unalerted, at full
   health (deaths stick). Called when leaving a room and when saving. */
void mushrooms_rest(void);
void update_mushrooms(void);
void draw_mushrooms(RenderContext *ctx);

/* Deal damage from any source. Wakes an unalerted mushroom into its scream,
   flashes the health bar, and handles death. Both the crucifaxe and a
   Grave-olver round pass 1 (scaled by mushroom_scale_damage). */
void mushroom_damage(Mushroom *m, int dmg);

/* Scale a hit by this enemy's weaknesses — there are none, by design. */
int32_t mushroom_scale_damage(int32_t base, DamageType type);

/* Grave-olver hitscan support: sprite-centre Y, half-height and half-width. */
void mushroom_body(const Mushroom *m, int32_t *cyc, int32_t *hh, int32_t *hw);

/* 1 while the body is off the ground, i.e. mid-leap. The crucifaxe uses it to
   skip knockback: shoving a mushroom sideways mid-arc would slide it off the
   interpolated line it is committed to. */
int  mushroom_airborne(const Mushroom *m);

/* Tell the renderer which texture window the current area has active, so each
   sprite can be drawn unmasked and the area's window then restored. Pass NULL
   for areas that use no texture window. */
void mushrooms_set_texwindow(const RECT *tw);

#endif
