#ifndef RABISU_H
#define RABISU_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
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
 * BEHAVIOUR, as of this commit: it hovers. It does not move, wake, steer or
 * attack — the abilities come later. What it DOES do already is turn to face
 * the player, take damage from both weapons, show a health bar, die, and
 * persist correctly across room changes and saves. That is deliberate: the
 * facing code and the damage/persistence wiring are the parts an attack phase
 * will build on, so they are real rather than stubbed.
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

#define RBS_MAX_HEALTH        20   /* 20 crucifaxe swings, 20 standard rounds,
                                      or 10 flame rounds (see the weakness
                                      table in rabisu.c)                     */
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

typedef struct {
    int32_t   x, y, z;          /* anchor: XZ centre, y = the model's UNDERSIDE */
    int32_t   spawn_x, spawn_y, spawn_z;
    int32_t   health;
    int32_t   active;           /* slot in use                                  */
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

/* THE one damage entry point — both weapons go through it, so waking (later),
   the bar flash, death and sound live in one place. */
void rabisu_damage(Rabisu *r, int dmg);

/* Crucifaxe strike: damage the first boss in reach in front of the player.
   Returns 1 if one was hit. The reach test is against the body's SURFACE, not
   its centre: this thing is 4.6 m tall and 3.4 m wide, so a centre-to-eye
   Manhattan distance is ~500 against a SWING_RANGE of 350 and the axe could
   never land. */
int  rabisus_try_hit(void);

/* Grave-olver hitscan support: the aim box (centre Y, half-height, half-width). */
void rabisu_body(const Rabisu *r, int32_t *cyc, int32_t *hh, int32_t *hw);

/* Scale a hit by this enemy's weaknesses (see damage.h). */
int32_t rabisu_scale_damage(int32_t base, DamageType type);

#endif
