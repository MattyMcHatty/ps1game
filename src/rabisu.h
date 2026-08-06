#ifndef RABISU_H
#define RABISU_H

#include <stdint.h>
#include <psxgte.h>  /* VECTOR — rabisu_anchor_world hands back a world point */
#include "render.h"
#include "damage.h"
#include "player.h"  /* MAX_HEALTH — the two attacks are stated as percentages */
#include "title.h"   /* GameState — each rabisu is tagged with its area */

/* -----------------------------------------------------------------------
 * Rabisu — the first BOSS, and the first enemy in the game drawn as a real
 * 3D MODEL rather than a camera-facing textured quad.
 *
 * Read tools/ADDING_A_3D_ENEMY.txt before changing anything structural here;
 * it is the companion runbook to tools/ADDING_AN_ENEMY.txt and records what a
 * model enemy does differently from a sprite one. The short version:
 *
 *   - Its art is assets/bosses/Rabisu.smx -> RABISU.SMD, loaded once at
 *     startup and drawn by walking the SMD prim stream, exactly the way the
 *     rooms and the concrete props do (src/concrete_props.c is the closest
 *     reference — a rotatable SMD prop).
 *   - It owns NO VRAM. The mesh is untextured: every one of its 476 quads is
 *     flat-shaded in one colour baked into the SMD, so there is no TIM, no
 *     CLUT line, no VRAM slot and no texture window to bracket. That is the
 *     single biggest difference from a sprite enemy, and it is why STEP 3 of
 *     ADDING_AN_ENEMY.txt does not apply.
 *   - Being a solid model it is backface-culled per poly (the SMD's nocull bit
 *     is clear), so roughly half its quads reach the GPU on any frame.
 *   - It turns to face the player. There is no billboard flip; the model is
 *     rotated for real, by composing a Y-rotation with the camera view matrix
 *     (CompMatrixLV) before the prim loop, and the view matrix is restored
 *     afterwards for whatever the caller draws next.
 *
 * BEHAVIOUR: this file owns the BODY — the model, the animation, the damage
 * bookkeeping, and the combat AI (the sweep, the two attacks and the fireball
 * projectiles). It does NOT own the ENCOUNTER. The reveal cutscene, the camera,
 * the lights, the subtitles, the music and the death sequence all live in
 * src/rabisu_boss.c, which drives this module through the small "director API"
 * at the bottom of this header.
 *
 * The split is worth keeping. Everything here is per-instance and would work
 * for a second Rabisu dropped into another room with no cutscene at all;
 * everything in rabisu_boss.c is a one-off script bolted to the Garden
 * Courtyard. Mixing them would make the second encounter a copy-paste job.
 *
 * Like the fat doors, tentacles and spiders, rabisus are ONE global array
 * tagged by area (update/draw/hit skip any whose area != game_state) rather
 * than a per-room array in world.c's RoomState. See STEP 6 of
 * ADDING_AN_ENEMY.txt: a copy of the array in all 14 rooms would spend the
 * memory-card blob's remaining headroom storing empty copies.
 * ----------------------------------------------------------------------- */

/* One global pool for the WHOLE GAME, not per room. Every placement in
   world_enter() draws on it and rabisu_add() silently places nothing once it
   is full. Two, so a second encounter can be dropped in without touching the
   save-blob size again. */
#define MAX_RABISUS            2

/* >>> THE CRUCIFAXE CANNOT HURT THIS ENEMY. <<< It is holy iron against a thing
   that is already damned, and it is the one enemy in the game the axe does
   nothing to — there is deliberately no rabisus_try_hit() for crucifaxe.c to
   call. The axe still matters, but as a PARRY: see the two deflect windows
   below. Damage comes from the grave-olver (1 standard, 2 flame) and from a
   fireball deflected back into its owner (1).

   12 HP is therefore two full cylinders of standard rounds, six flame rounds,
   or any mix eked out with deflects. */
#define RBS_MAX_HEALTH        12
#define RBS_BAR_TIMER_MAX    120   /* frames the health bar stays up after a hit */

/* --- Mesh-derived geometry ---------------------------------------------
   >>> THESE COME FROM THE UNION BOUNDING BOX OVER EVERY ANIMATION FRAME,
       NOT FROM THE BIND POSE. <<<
   The boss flaps: its silhouette at full wing-spread is 505 wide against the
   bind pose's 336, and it reaches 559 tall against 461. Sizing the hit box and
   the collision cylinder off the static mesh would leave the wings unshootable
   and let the player stand inside them.

   tools/io_export_pva.py prints exactly these three numbers at the end of every
   bake, so re-deriving them is not a separate step — re-export the animation
   and read them off. Current values are the 19-frame idle:

     x [-252.0, 253.8]   span 505.7
     y [-427.2, 131.5]   span 558.7      (-Y is up)
     z [-185.1,  37.6]   span 222.7

   As authored the model does NOT sit on its own origin — and after rigging it
   sits BELOW it: its lowest point is at mesh y=+131.5, i.e. 131 units below the
   origin plane. RBS_FOOT_OFF cancels that (hence negative now, where the
   pre-rig bind pose wanted +168), so the entity anchor means "the model's
   UNDERSIDE" and a placement states a hover height directly. */
#define RBS_FOOT_OFF        (-132)  /* -(max mesh y over all frames)           */
#define RBS_HEIGHT           559    /* union y span                            */
#define RBS_HALF_W           254    /* union x half-span, at full wing-spread  */
#define RBS_HALF_H  (RBS_HEIGHT / 2)

/* Body cylinder for the player push-out and the melee reach test. The model
   rotates, so an axis-aligned box (what the concrete props use) would swell
   and shrink as it turned; a cylinder of the largest horizontal extent is the
   only shape that is stable under rotation. 255 covers the wings at full
   spread, so the player cannot stand inside the wing sweep. Drop it toward the
   ~90-unit torso if the wings should pass through the player instead — it is
   only this one constant, and rabisu_gap() keeps the melee reach in step. */
#define RBS_BODY_RADIUS      255

/* --- Idle animation -----------------------------------------------------
   Game frames per animation frame. The clip is authored at 24 fps and the game
   runs at 60, so 3 gives 20 fps — the closest clean divisor. 19 frames at 20
   fps is a 0.95 s loop.

   The animation is BAKED VERTEX POSITIONS (assets/bosses/Rabisu_idle.pva), not
   bones: the rig smooth-skins up to 13 influences per vertex, which no runtime
   on this hardware could evaluate. See tools/ANIMATING_A_3D_MODEL.txt. */
#define RBS_ANIM_TICKS         3

/* How far the underside hovers above the floor it is placed over. 1 m. */
#define RBS_HOVER            100

/* Which way the model faces in its own space. The upper body spreads along X
   (the wings) and the low tail trails off toward -Z, so +Z is read as forward.
   If the boss turns out to face away from the player in-game, flip this to 1
   and nothing else changes. */
#define RBS_FACE_BACKWARD      0

/* --- Named points ON the model, in MESH-LOCAL space ------------------------
   Read off the idle clip's frame 0 (the pose the death sequence freezes on, so
   the wings are at full spread and these are exact rather than average):

     head crown   the highest cluster of vertices, y = -427..-417 (-Y is up)
     wing tips    the extreme +/-X vertices, (+/-250, -278, -35)
     chest        centre-line, on the front (-Z) face, ~62% up the silhouette

   The death sequence hangs its four lights on these. They are LOCAL: the model
   rotates to face the player, so rabisu_anchor_world() turns one into a world
   point through the same matrix the draw uses (RBS_FOOT_OFF included). */
#define RBS_A_HEAD_X            0
#define RBS_A_HEAD_Y        (-420)
#define RBS_A_HEAD_Z        (-20)
#define RBS_A_WING_X          250   /* mirrored to -X for the other tip */
#define RBS_A_WING_Y        (-278)
#define RBS_A_WING_Z         (-35)
#define RBS_A_CHEST_X           0
#define RBS_A_CHEST_Y       (-215)
#define RBS_A_CHEST_Z        (-40)

/* --- The arena -------------------------------------------------------------
   Garden Courtyard, and stated here rather than passed in because there is
   exactly one Rabisu encounter. Three polys in from each perimeter wall the
   terrace drops 100 units to the sunken lawn, and that retaining lip is a
   one-way step: the player can fall down it but cannot climb back up (walls 0
   and 5 in garden_courtyard_mesh_collision.c, y 800..900). Those two lips —
   x = -1722 west and x = +1142 east — bound the fight, and the boss's spawn at
   x = -290 sits exactly 1432 from each, which is what makes the sweep
   symmetric. Move the spawn in world.c and this stops being true. */
#define RBS_SWEEP_RADIUS     1432
#define RBS_ARENA_MIN_X   (-1722)
#define RBS_ARENA_MAX_X     1142
/* The lawn's own Z extent is [-857, 2000]. Inset a little at the south end so
   the arc's bow — which pulls the boss toward the player at the ends of the
   sweep — cannot carry it up onto the higher terrace, where its 1 m hover
   would put its feet in the paving. */
#define RBS_ARENA_MIN_Z    (-820)
#define RBS_ARENA_MAX_Z     1900

/* --- Combat timing, in frames at 60 fps ------------------------------------
   The pattern: sweep side to side across the arc a random 3..8 times, stop on
   one of the three stopping points, hold 1.5 s, attack, resolve, sweep again.

   The stopping points are the two arc ends and the spawn — the boss never
   halts mid-travel, so a stopped Rabisu is always somewhere the player has
   already seen it stop. */
#define RBS_SWEEP_FRAMES       60   /* 1 s from the spawn out to either lip     */
#define RBS_STOP_PAUSE         90   /* 1.5 s held still, then an attack         */
#define RBS_MOVES_MIN           3   /* traversals between attacks, inclusive    */
#define RBS_MOVES_MAX           8

/* Fireball : foot slash = 2 : 1, so a roll of 3 with one losing face. */
#define RBS_ATTACK_ROLL         3
#define RBS_SLASH_FACE          0

/* --- Attack 1: the fireball ------------------------------------------------
   Aimed at where the player IS when it leaves the chest and travelling in a
   straight line for exactly RBS_FB_FLIGHT, so sidestepping the line beats it —
   this is the dodgeable attack.

   It is also the only attack that can be turned back. Swing the crucifaxe
   inside RBS_FB_PARRY of contact and the ball reverses at DOUBLE speed into
   the thing that threw it, for 1 damage. That is the whole reason to keep the
   axe out against an enemy the axe cannot cut. */
#define RBS_FB_FLIGHT          30   /* 0.5 s chest -> player                    */
/* Aim BELOW the eye. player_y() is the camera, i.e. the very top of the
   player's body, and a ball arriving exactly there comes in at the horizon
   line where it is hardest to read against the far wall. Dropping the aim
   point puts it in the lower half of the screen, where it looks like something
   thrown AT you. The contact test uses the same dropped point, so this shifts
   where the ball goes without making it any easier or harder to dodge. */
#define RBS_FB_AIM_DROP        70
#define RBS_FB_DAMAGE   (MAX_HEALTH / 10)   /* 10% of the bar                   */
#define RBS_FB_PARRY           12   /* 0.2 s deflect window before contact      */
#define RBS_FB_HIT_RADIUS     130   /* contact sphere around the player anchor  */
#define RBS_FB_HALF            28   /* half-extent of the drawn cube            */
#define RBS_FB_RBS_RADIUS     300   /* how close a deflected ball must get back */
#define MAX_RBS_FIREBALLS       4

/* --- Attack 2: the foot slash ----------------------------------------------
   Races in over 1 s leaning further and further back, and lands feet-first.
   It CANNOT be dodged — the charge re-aims at the player every frame — but the
   deflect window is twice the fireball's to compensate, and a deflected slash
   simply deals nothing (it is a block, not a riposte). */
#define RBS_SLASH_IN           60   /* 1 s charge                               */
#define RBS_SLASH_BACK         90   /* 1.5 s drift back to where it launched    */
#define RBS_SLASH_DAMAGE (MAX_HEALTH / 5)   /* 20% of the bar                   */
#define RBS_SLASH_PARRY        24   /* 0.4 s deflect window before the landing  */
/* >>> THIS MUST LAND INSIDE THE PLAYER'S PUSH-OUT DISTANCE. <<<
   rabisus_collide is called with a player radius of 75 against
   RBS_BODY_RADIUS (255) and a 30 margin, so the two bodies touch at 360. An
   earlier 430 stopped the charge 70 short of even that: the damage still
   applied — it always does, the attack cannot be dodged — but nothing on
   screen made contact, so a landed hit was indistinguishable from a whiff.
   330 arrives just inside the bubble and shoves the player, which is what a
   kick in the chest should do. */
#define RBS_SLASH_STANDOFF    330   /* how close the feet come to the player    */
/* And it must land in FRONT of the player's face, not at their ankles. The
   boss hovers with its underside on the terrace floor (y=800) while the
   player's eye is at 611, so a charge that keeps its spawn height swings its
   feet in below the bottom of the screen. The charge lifts the underside to
   this far below the eye — chest height — and the return leg puts it back. */
#define RBS_SLASH_RISE_TO      60
#define RBS_SLASH_KNOCKBACK    70
#define RBS_SLASH_LEAN        780   /* 4096ths of a turn: ~69deg back at impact */
/* Sign of the lean rotation. The model faces +Z, so leaning BACK means the
   crown tips toward -Z. If it ends up folding face-first into the ground
   instead, flip this to 1 and nothing else changes. */
#define RBS_LEAN_BACKWARD       0

/* Sentinel for Rabisu.clip_y: no vertical clipping. Far below any world Y the
   game uses, so the "is this poly under the cut" test is simply always false. */
#define RBS_NO_CLIP        0x7FFFFFFF

/* What the combat AI is doing. RBS_AI_DORMANT means it is doing nothing at
   all: the encounter director is posing the body by hand (the rise out of the
   floor, the death freeze), or the fight has not started yet. */
typedef enum {
    RBS_AI_DORMANT = 0,
    RBS_AI_MOVE,        /* travelling toward sweep_target                      */
    RBS_AI_PAUSE,       /* stopped, counting down to an attack                 */
    RBS_AI_FIRE,        /* fireball away, waiting for it to resolve            */
    RBS_AI_SLASH_IN,    /* charging the player feet-first                      */
    RBS_AI_SLASH_BACK,  /* drifting back to where the charge began             */
} RbsAiState;

typedef struct {
    int32_t   x, y, z;          /* anchor: XZ centre, y = the model's UNDERSIDE */
    int32_t   spawn_x, spawn_y, spawn_z;
    int32_t   health;
    int32_t   active;           /* slot in use                                  */
    /* Two death flags, not one. `dying` is set the instant the last point comes
       off and stops the AI, the collision, the health bar and every weapon from
       touching it — but the body is still DRAWN, because the death sequence is
       six seconds of watching it come apart. `dead` is set by the director at
       the very end of the fade and means "gone": skipped by everything. */
    int32_t   dying;
    int32_t   dead;
    int32_t   hit_timer;        /* health-bar flash countdown                   */
    /* Facing, as the sin/cos pair of its yaw in 4096ths (the same fixed-point
       isin/icos use). Stored rather than an angle because it is derived by
       normalising the vector to the player, which needs no atan2 — and because
       the draw wants exactly these two numbers to build its rotation matrix.
       Seeded facing +Z at spawn; update_rabisus turns it toward the player. */
    int32_t   face_s, face_c;
    /* Idle playback clock. Per instance so two bosses in a room do not flap in
       lockstep; persisted with the rest of the struct, which costs nothing and
       means a reload does not snap the pose. */
    int32_t   anim_frame, anim_tick;

    /* --- Combat AI ---------------------------------------------------------
       `sweep` is a signed lateral position along the arc in 4096ths, where
       -4096 is one lip, 0 is the spawn and +4096 the other. It is stored
       rather than a world offset because the arc SWINGS: the axis it is
       measured along is perpendicular to the boss->player line, so the same
       sweep value means a different world point as the player circles. */
    RbsAiState ai_state;
    int32_t   ai_timer;          /* frames left / elapsed in the current state  */
    int32_t   sweep;             /* -4096..+4096 along the arc                  */
    int32_t   sweep_target;      /* the stopping point being travelled to       */
    int32_t   moves_done;        /* traversals completed since the last attack  */
    int32_t   moves_target;      /* how many to do before stopping (3..8)       */
    /* The spot a foot slash launched from, so the return leg has somewhere to
       go. Kept as a world point rather than a sweep value because the charge
       leaves the arc entirely. */
    int32_t   slash_from_x, slash_from_y, slash_from_z;
    int32_t   lean;              /* backward lean in 4096ths; 0 upright         */
    /* Facing override. Normally it looks at the player, which is right for the
       whole fight — but during a cutscene the player is ANCHORED wherever they
       happened to be standing (just inside the door, usually), and a boss
       rising out of the ground while staring off toward the doorway is not the
       shot. The director points it at the camera instead. Off = 0. */
    int32_t   face_ovr;
    int32_t   face_ovr_x, face_ovr_z;

    /* --- Cutscene pose, written by the encounter director ------------------ */
    int32_t   frozen;            /* 1 = hold the current animation frame        */
    int32_t   shake;             /* jitter amplitude in world units; 0 = still  */
    int32_t   fade;              /* 256 = solid; below that it burns out to
                                    nothing (additive, scaled toward black).
                                    0 skips the model entirely — that is how it
                                    stays out of sight under the lawn before the
                                    reveal, rather than relying on the floor to
                                    hide it.                                    */
    /* World Y below which polys are dropped, so the boss can rise THROUGH the
       lawn instead of sliding up in front of it. It has to be an explicit clip:
       the room's floor quads sort by their FARTHEST corner (see render.h), which
       pushes them back in the OT and would let the still-buried half of the
       model draw over the grass it is supposed to be under.
       RBS_NO_CLIP disables it, which is the normal state. */
    int32_t   clip_y;
    GameState area;
} Rabisu;

extern Rabisu rabisus[MAX_RABISUS];
extern int    rabisu_count;

void rabisus_load_assets(void);   /* startup: load RABISU.SMD (resident)        */
void rabisus_init(void);          /* startup: the array starts empty            */
void rabisus_reset(void);         /* new game: clear every placement            */
/* Place one hovering RBS_HOVER above `ground_y` at (x,z). `ground_y` is the
   floor SURFACE in world units (the FloorZone's y), NOT an entity anchor — do
   not pre-subtract GROUND_FLOOR_Y (see ADDING_AN_ENEMY.txt mistake 2).
   Returns the index, or -1 when the pool is full. */
int  rabisu_add(int32_t x, int32_t ground_y, int32_t z, GameState area);
/* Put every living boss back at its spawn at full health; deaths stick. Called
   from world_leave beside zombies_rest(). */
void rabisus_rest(void);

void update_rabisus(void);
void draw_rabisus(RenderContext *ctx);

/* Player push-out against the body cylinder. Area-gated inside, so the shared
   room collision routine can call it unconditionally. */
void rabisus_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* THE one damage entry point — every source goes through it, so the bar flash,
   the sound, and the handover to the death sequence live in one place. There
   is no crucifaxe caller: see the note on RBS_MAX_HEALTH. */
void rabisu_damage(Rabisu *r, int dmg);

/* Grave-olver hitscan support: the aim box (centre Y, half-height, half-width). */
void rabisu_body(const Rabisu *r, int32_t *cyc, int32_t *hh, int32_t *hw);

/* Scale a hit by this enemy's weaknesses (see damage.h). */
int32_t rabisu_scale_damage(int32_t base, DamageType type);

/* ---- Fireballs -------------------------------------------------------------
   The boss's projectiles. A separate pool from the spider's webs because they
   behave differently in the one way that matters: a web only ever travels
   toward the player, while a fireball can be turned around. Transient, so —
   like webs — they are NOT part of the save blob. */
void rbs_fireballs_reset(void);
void rbs_fireballs_update(void);
void rbs_fireballs_draw(RenderContext *ctx);

/* ---- The encounter director's API (src/rabisu_boss.c) ----------------------
   Everything below exists so the cutscene can pose and drive the body without
   rabisu.c needing to know a cutscene exists. */

/* The one live boss in the current area, or NULL. */
Rabisu *rabisu_boss_instance(void);

/* Park the AI so the director can move the body by hand (the rise out of the
   floor, the return to the spawn on death). Cancels any attack in flight. */
void rabisu_go_dormant(Rabisu *r);

/* Hand the body to the combat AI: it starts sweeping immediately. Also drops
   any facing override, so it goes back to looking at the player. */
void rabisu_fight_begin(Rabisu *r);

/* Point it at (x,z) instead of at the player, for as long as `on`. The player
   is anchored during a cutscene and is generally not where the shot is. */
void rabisu_face_override(Rabisu *r, int on, int32_t x, int32_t z);

/* Turn a MESH-LOCAL point (one of the RBS_A_* anchors) into the world point it
   currently occupies, through the same yaw + lean + RBS_FOOT_OFF the draw
   uses. This is how the death lights stay stuck to the head and the wings. */
void rabisu_anchor_world(const Rabisu *r, int32_t lx, int32_t ly, int32_t lz,
                         VECTOR *out);

#endif
