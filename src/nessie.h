#ifndef NESSIE_H
#define NESSIE_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"         /* GameState — each Nessie is tagged with its area */
#include "crucifaxe.h"     /* SWING_RANGE — NSS_AXE_REACH is derived from it  */
#include "nessie_pools.h"  /* NessiePoolId — the lava it may swim in          */

/* -----------------------------------------------------------------------
 * Nessie — Chapter 4's lava monster, and the chapter's first enemy.
 *
 * A head on a neck of molten rock, rising out of a lava pool. It never leaves
 * that pool: every placement names one (NSS_POOL_*, generated from the room's
 * visual mesh by tools/gen_nessie_pools.py), and any step whose centre would
 * leave the pool's lava is refused. So it cannot cross a path, climb onto
 * rock, or get from one pool into another — the pools are separate bitmasks
 * and the generator floods each one 4-connected, so two lava cells that touch
 * only at a corner (the outcrop ridge in Sharur's Trial) are two pools.
 *
 * Built on the Lumberer (src/lumberer.c) for everything that is not movement:
 * the two-sheet 4bpp sprite and its texture-window bracket, the health bar, the
 * solid push-out with the axe measured to its surface, the global area-tagged
 * array and the canonical save keying. The fireball is the Rabisu's
 * (src/rabisu.c), pool, parry and all.
 *
 * TWO STATES, as specified:
 *
 *   IDLE   It swims: picks a random heading, holds it for NSS_LEG_FRAMES (three
 *          seconds), picks another. A step that would leave the lava or that
 *          the rim or an outcrop pushes back is a WALL, and the heading is
 *          REVERSED on that same frame ("the opposite trajectory") with a fresh
 *          three seconds on it. A body reversed straight into a second wall —
 *          a corner — takes a random heading instead, so it cannot ping-pong.
 *   ALERT  The player came inside the room's fog distance (g_fog_far, the very
 *          test that decides whether the body is DRAWN — so it notices you the
 *          moment it is visible), or something hit it. It swims at the player
 *          at the same slow speed, sliding along the lava's edge when the
 *          straight line runs out of lava, and comes right up to that edge.
 *          It drops back to IDLE when the player leaves fog distance, so it
 *          never spits at someone who cannot see it.
 *
 * THE ATTACK, every NSS_ATTACK_EVERY (3 s) while ALERT, rooted throughout:
 *
 *   PULL   frames 0, 1 over SFX_PULL (pullin.vag, 1.08 s), half each.
 *   SPIT   frames 2, 3 over SFX_FIREBALL (fireball.vag, 1.08 s), half each.
 *          The fireball leaves the mouth on the first frame of this phase,
 *          with its sound — the Rabisu's ball does the same.
 *   HOLD   frame 3 for one more second, then back to the swim frames and a
 *          fresh three-second countdown.
 *
 * The countdown only expires into an attack with a CLEAR SIGHTLINE from the
 * mouth to the player (collision_segment_blocked), or it would spit into the
 * outcrop it is hiding behind; until then it waits, still swimming.
 *
 * THE FIREBALL: straight-line, aimed once at spawn — sidestepping beats it.
 * Unlike the Rabisu's, its SPEED is fixed rather than its flight time: that
 * one fights at sweep range, this one spits from as far as the fog, and a
 * fixed half-second over 3500 units would be unreadable. A Crucifaxe swing
 * started in the last NSS_FB_PARRY frames before the nominal arrival turns it
 * round at double speed into the head, for 1 damage.
 *
 * 8 HP: eight crucifaxe swings, eight Standard or Scattershot rounds, eight
 * seconds of the Helluminator, or a deflected fireball for one of them. She is
 * IMMUNE TO FLAME ROUNDS - a creature of lava does not burn - and a flame round
 * does nothing at all but wake her (nessie_scale_damage).
 *
 * TEXTURES. ONE 256x256 4bpp sheet (textures/underworld/nessie.png), 3x2:
 *
 *     frames 0, 1   the pull-in            }  the top row:   NESSIEA
 *     frame  2      the mouth opening      }
 *     frame  3      the spit, held         }  the bottom row: NESSIEB
 *     frames 4, 5   idle / swimming, one a second
 *
 * Shipped as two 256x128 halves on ADAPA'S exact footprint, pixels and CLUTs:
 * x[832,960) y128, CLUTs (80,480)/(96,480). He is mansion-only and this is
 * Underworld-only, so the two are never drawn in the same room. The pages are
 * also the exit door's two leaves, so nessies_upload_textures() calls
 * door_anim_panels_taken() and the restore puts them back on the way out of
 * the chapter (main.c uploads this AFTER that restore, as it does Adapa).
 *
 * >>> ONLY SHARUR'S TRIAL AND LAVA U UPLOAD THE SHEET TODAY. <<< Placing one in another
 * room means: a pool line in tools/gen_nessie_pools.py, the placement in
 * world.c, the room in nessie_room_has_sheet() (the .c) — which is what main.c
 * and tools/gen_tex_rooms.py key the upload on — and update_nessies() /
 * draw_nessies() / nessies_set_texwindow() in that room.
 *
 * PERSISTENCE: the lumberers' model. ONE global array tagged by area, every
 * loop gated on current_area (NEVER game_state — the inventory menu must not
 * pause it), deaths keyed by canonical_index() in WorldDelta.nessies_dead.
 * MAX_NESSIES is the budget for the WHOLE GAME, and nessie_add drops SILENTLY
 * once it is full.
 *
 * SOUND. Both clips are BORROWED and both are SND_BANK_UNDERWORLD copies:
 * SFX_PULL is the Rafflesia's grab (and is fireball.wav reversed, which is why
 * the two read as one breath in and out) and SFX_FIREBALL is the Rabisu's.
 * Hits are SFX_AXEHIT, resident. Her death is her own clip, SFX_NSS_DIE
 * (nessie die, 2.58 s, UNDERWORLD only), played with the lava burst.
 * ----------------------------------------------------------------------- */

#define MAX_NESSIES            8    /* WHOLE-GAME budget — see above. Four placed: three
                                       in Sharur's Trial, one Lava U                */
#define NSS_MAX_HEALTH         8    /* eight swings, eight non-flame rounds       */

/* ---- Speed, in 1/256 units per frame. "Very slowly": 384 is 1.5 units a
   frame, 90 a second. The player walks at 12 a frame and the Lumberer, the
   slowest walker before this, at 5. It is a turret that drifts, not a chaser.

   >>> FIXED POINT, BECAUSE A GAIT THIS SMALL CANNOT TURN. <<< At 1.5 units a
   frame an integer step has components of 0 or 1, so every heading but the
   axes and the diagonals would truncate away (tools/ADDING_AN_ENEMY.txt
   mistake 16d). The body keeps its position at 1/256 of a unit and the step is
   exact on any bearing. */
#define NSS_SPEED_Q8         384

/* One idle leg: a heading held for three seconds, then another. */
#define NSS_LEG_FRAMES       180

/* The swim cycle: frames 4 and 5, one a second, always — it is the idle
   animation as well as the movement one, so it runs when the body is still. */
#define NSS_ANIM_RATE         60

/* ---- The attack, in frames at 60 fps. Both clips are 1.08 s (65 frames at
   11025 Hz; tools/ADDING_A_SOUND.txt STEP 1 reads the length off the VAG
   header), and each carries two frames of the animation.

   >>> THESE ARE CUT TO THE CLIPS' LENGTHS AND RE-CUTTING A CLIP BREAKS THEM
   SILENTLY. <<< pullin.vag and fireball.vag are shared with the Rafflesia and
   the Rabisu; if either is trimmed or re-recorded, work its length out in
   frames again and reset the constant here. */
#define NSS_ATTACK_EVERY     180    /* 3 s of swimming between attacks          */
#define NSS_PULL_FRAMES       65    /* pullin.vag, 1.08 s: frames 0 then 1      */
#define NSS_SPIT_FRAMES       65    /* fireball.vag, 1.08 s: frames 2 then 3    */
#define NSS_HOLD_FRAMES       60    /* frame 3 held for one more second         */

/* ---- The fireball. The Rabisu's ball (src/rabisu.h, RBS_FB_*) at a fixed
   speed instead of a fixed flight time — see the head. 32 a frame is 1920 a
   second: from 1500 away it arrives in under 0.8 s, which a player at a walk
   (12) can just step off the line of, and a sprinting one (20) easily. */
#define NSS_FB_SPEED          32
#define NSS_FB_DAMAGE   (MAX_HEALTH / 10)   /* 10% of the bar, the Rabisu's     */
#define NSS_FB_PARRY          12    /* 0.2 s deflect window before contact      */
#define NSS_FB_HIT_RADIUS    130    /* contact sphere around the aim point      */
#define NSS_FB_AIM_DROP       70    /* aim below the eye, as RBS_FB_AIM_DROP    */
#define NSS_FB_HALF           28    /* half-extent of the drawn cube            */
#define NSS_FB_HEAD_RADIUS   200    /* how close a deflected ball must get back */
#define MAX_NSS_FIREBALLS      4

/* ---- Sprite. "About 360 tall": half-height 180, and the half-width is the
   source frame's own 85:128 of it (120), so the art is not stretched.

   NSS_Y_OFFSET + NSS_HALF_H == 150 is every sprite enemy's invariant — the drop
   from the anchor to the feet — so the base of the neck sits on the lava. On
   Sharur's Trial's y=0 floor the anchor is -149, so the top of the head is at
   -359 and the mouth around -230: the head rears above the player's eye
   (-189) and spits slightly DOWN at them.

   ONE QUAD, NOT A GRID: the GPU drops a primitive past 1023 pixels, and a
   360-unit span reaches that at a view depth of 360*256/1023 = 90 — well
   inside NSS_HOLD_DIST, closer than the player can stand. */
#define NSS_HALF_W           120
#define NSS_HALF_H           180
#define NSS_Y_OFFSET        (150 - NSS_HALF_H)   /* -30; keeps the base down     */
/* The mouth, as a drop from the TOP of the sprite: about 36% down the frame in
   the art (the open mouth of frame 3). Where the fireball leaves from. */
#define NSS_MOUTH_DROP       130

/* ---- Body. NSS_BODY_RADIUS is how far the WALLS (the rim, the outcrops) hold
   the head off — the lava mask keeps the centre in the pool, this keeps the
   neck out of the rock. NSS_PUSH_RADIUS is how far it holds the PLAYER off, in
   apply_collision_reception before the walls (lumberers_collide's terms and
   reason), so the player cannot walk into the billboard and lose it. */
#define NSS_BODY_RADIUS       80
#define NSS_PUSH_RADIUS      100
#define NSS_WALL_LIKE_PUSH   195    /* what apply_collision_reception passes in */
#define NSS_HOLD_DIST       (NSS_PUSH_RADIUS + NSS_WALL_LIKE_PUSH)   /* 295     */
#define NSS_PUSH_STEP         48
/* An ALERT head stops this far short of the player rather than pressing into
   the push-out every frame. */
#define NSS_STOP_DIST       (NSS_HOLD_DIST + 20)

/* ...and the crucifaxe's reach, measured to the push surface (the Lumberer's
   LMB_AXE_REACH): on level ground the eye is ~10 above the body's centre, so a
   swing connects inside 100 + 350 - 10 = 440, past the 295 the player is held
   at. */
#define NSS_AXE_REACH       (NSS_PUSH_RADIUS + SWING_RANGE - 10)
_Static_assert(NSS_AXE_REACH > NSS_HOLD_DIST,
               "the push-out would hold the player outside the axe's reach");

/* ---- THE LAVA BURST. On death she bursts (spawn_lava_burst) and a player
   within NSS_SPLASH_RADIUS of her centre takes NSS_SPLASH_DAMAGE, once.

   >>> "SLIGHTLY BEYOND THE COLLISION RADIUS" IS MEASURED FROM WHERE THE PLAYER
   IS HELD, NOT FROM NSS_PUSH_RADIUS. <<< The push-out keeps the player
   NSS_HOLD_DIST (295) off her centre, so a radius of 100 + a little could
   never be true on any frame — the Rafflesia's dead-reach bug, mistake 9 in
   tools/ADDING_AN_ENEMY.txt. 80 past the hold distance is the "slightly":
   standing pressed against her is a burn, one step back is not.

   It sits INSIDE the axe's reach (440) on purpose, so the killing swing is a
   choice: land it from the edge of your reach and walk away clean, or from
   point blank and take the splash. The gun and a deflected fireball kill from
   range and are never burned. */
#define NSS_SPLASH_RADIUS   (NSS_HOLD_DIST + 80)   /* 375, true radial        */
#define NSS_SPLASH_DAMAGE    10                     /* HP, as specified        */
_Static_assert(NSS_SPLASH_RADIUS > NSS_HOLD_DIST,
               "the lava splash could never reach a player held off the body");
_Static_assert(NSS_SPLASH_RADIUS < NSS_AXE_REACH,
               "every killing swing would be a burn - none could be landed clean");

#define NSS_BAR_TIMER_MAX    120

typedef enum {
    NSS_IDLE,     /* swimming its pool at random                              */
    NSS_ALERT,    /* swimming at the player; the attack countdown is running  */
    NSS_PULL,     /* rooted: frames 0/1 over the pull-in                      */
    NSS_SPIT,     /* rooted: frames 2/3 over the fireball; the ball is out    */
    NSS_HOLD,     /* rooted: frame 3 held                                     */
    NSS_DEAD,
} NessieState;

typedef struct {
    int32_t      x, y, z;
    /* Position at 1/256 of a unit — see NSS_SPEED_Q8. x/z above are these
       shifted down, kept in step every frame for everything else to read. */
    int32_t      qx, qz;
    int32_t      spawn_x, spawn_z, spawn_y;
    int32_t      vy;

    /* The idle heading, a unit vector at 4096 (isin/icos scale), and the
       frames left on this leg. `bumped` is set on the frame a reversal
       happened, so a second wall straight after picks a random heading. */
    int32_t      hx, hz;
    int          leg;
    int          bumped;

    int          health;
    int          hit_timer;
    int          anim_tick;     /* drives the swim frames, always ticking     */
    int          atk_timer;     /* ALERT: frames left before the next attack  */
    int          atk_tick;      /* frames into the current attack phase       */

    NessiePoolId pool;
    NessieState  state;
    int32_t      active;
    int          on_upper_floor;
    int          on_ramp;
    GameState    area;
} Nessie;

extern Nessie nessies[MAX_NESSIES];
extern int    nessie_count;

/* Register the two sheet halves with texmgr (TEXBANK_UNDERWORLD). Call ONCE at
   startup, beside the other enemies'. */
void nessies_load_textures(void);

/* Stream both halves into VRAM and mark the exit door's leaves taken. main.c
   calls it from STATE_LOADING, AFTER door_anim_restore_panels(), for every
   area nessie_room_has_sheet() names. */
void nessies_upload_textures(void);
int  nessie_room_has_sheet(GameState area);

/* Place one at (x,z), swimming in `pool`. Authored — nothing here reads the
   room's mesh — so world.c can rebuild a placement from a save without the room
   resident. Returns its index, or -1 if the global pool is full. */
int  nessie_add(int32_t x, int32_t z, NessiePoolId pool, GameState area);

void nessies_init(void);
void nessies_reset(void);
/* Every living Nessie back at its spawn, idle, at full health (deaths stick),
   and the air cleared of fireballs. Called when leaving a room and when saving. */
void nessies_rest(void);

void update_nessies(void);
/* The heads, then their fireballs. Call with the plain camera view loaded. */
void draw_nessies(RenderContext *ctx);

/* Deal damage from any source. Wakes an idle one, flashes the bar, handles
   death. Every weapon passes 1 (scaled by nessie_scale_damage). */
void nessie_damage(Nessie *s, int dmg);
/* The crucifaxe test, measured to the push surface (NSS_AXE_REACH). */
int  nessies_try_hit(void);
/* The player's push-out (NSS_HOLD_DIST); called from apply_collision_reception. */
void nessies_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

int32_t nessie_scale_damage(int32_t base, DamageType type);

/* Grave-olver / Helluminator support: sprite-centre Y, half-height, half-width. */
void nessie_body(const Nessie *s, int32_t *cyc, int32_t *hh, int32_t *hw);

/* The area's texture window, restored after each sprite (the sheet is at Voff
   128). NULL for an area with none. */
void nessies_set_texwindow(const RECT *tw);

#endif
