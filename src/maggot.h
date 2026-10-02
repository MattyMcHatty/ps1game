#ifndef MAGGOT_H
#define MAGGOT_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState — each maggot is tagged with its area */

/* -----------------------------------------------------------------------
 * Maggot — a winged grub that comes straight for the player and poisons them.
 *
 * >>> IT IS THE CREEP WITH TWO DIFFERENCES, AND src/creep.h IS THE REFERENCE.
 * <<< Read that file for the reasoning behind everything that is the same here
 * — the transient pool, the floating body, the ghost movement with no world
 * collision, the separation pass that stops a swarm collapsing into one
 * sprite, the absent heading blend, the stacking contact damage. None of it is
 * re-argued below; where a constant has the creep's value it has it for the
 * creep's reason. The differences are:
 *
 *   1. IT POISONS. Every bite that takes health also calls player_poison(),
 *      the spider web's status (half walk speed, no sprint, POISON_DURATION
 *      frames — player.h). A bite REFRESHES the timer rather than stacking it,
 *      which is player_poison()'s own rule, so a swarm keeps the player slowed
 *      for as long as any of it is biting and five seconds after.
 *
 *   2. IT HAS NO EMERGENCE. A creep rises out of the crib over
 *      CRP_EMERGE_FRAMES before it hunts. A maggot has ONE state: the moment
 *      maggot_spawn() returns it is attacking. It is placed directly at its
 *      float height, so there is nothing to interpolate.
 *
 * And two things a creep does not have at all:
 *
 *   - A TWO-IMAGE FLIGHT CYCLE. maggot.png is two 64x64 images side by side
 *     and the sprite alternates between them every MGT_ANIM_FRAMES, fast
 *     enough to read as wings buzzing rather than as two poses. It flaps
 *     whether or not the body is moving: it is flying, and a hovering fly
 *     still beats its wings (contrast mistake 15 in tools/ADDING_AN_ENEMY.txt,
 *     which is about a WALKING gait and is right to stop on a stuck body).
 *
 *   - A LOOPED BUZZ, SFX_BUZZ, on ONE voice for the whole swarm, keyed while
 *     any maggot is alive in the current area. See update_maggots().
 *
 * ---- Persistence ---------------------------------------------------------
 * The creep's model: a fixed pool, TRANSIENT, no world.c snapshot, no
 * WorldDelta field and no save cost. maggots_reset() runs in reset_game and in
 * world_leave and drops the whole pool. Whatever PLACES maggots decides
 * whether they come back — a spawn in world_enter's first-visit block would
 * mean once per playthrough, outside it would mean on every entry.
 * ----------------------------------------------------------------------- */

/* The whole-game pool. The creep's number; raise it if a placement wants more
   than a dozen at once. maggot_spawn() returns -1 and places nothing when it
   is full. */
#define MAX_MAGGOTS           12

/* TWO hits from anything. Every weapon does 1 to it — the axe's swing, one
   round of either type, one second of the lantern — because its weakness table
   pins every damage type at 100% (maggot.c), so "hits to kill" is this number
   whatever the player is holding. */
#define MGT_MAX_HEALTH         2
#define MGT_SPEED             11   /* the creep's: a shade quicker than a dog  */

/* Contact — the creep's arithmetic exactly: a quarter of MAX_HEALTH per second
   per maggot, Manhattan reach that covers a body stopped on the diagonal of
   MGT_STOP_DIST (110 * sqrt(2) = 156 < 170). Stacks across maggots. */
#define MGT_CATCH_DIST       170
#define MGT_DAMAGE_AMOUNT      5
#define MGT_DAMAGE_TICK       12

/* Where it stops: a TRUE radius from the player, inside SWING_RANGE. */
#define MGT_STOP_DIST        110

/* Sprite: 136 x 136 world units, the creep's size. The art is square (each
   image is 64x64) and the two are meant to be read as the same scale of pest. */
#define MGT_HALF_W            68
#define MGT_HALF_H            68

/* FLOATING AT EYE LEVEL — the creep's construction (creep.h has the
   arithmetic). The anchor stays on the floor under apply_ddog_height and only
   the drawn body is lifted; maggot_body_y() is the single place that resolves
   it, and every reader goes through that function. */
#define MGT_EYE_RISE          40
#define MGT_BELOW_EYE         18
#define MGT_Y_OFFSET         (MGT_BELOW_EYE - MGT_EYE_RISE)
#define MGT_FLOAT_VARY        35   /* per-body height, latched at spawn        */

/* Separation: the creep's, and for the creep's reason load-bearing — a swarm
   homing on one point is otherwise one visible sprite. */
#define MGT_BODY_RADIUS       60
#define MGT_SEP_RADIUS       150
#define MGT_SEP_WEIGHT         2

/* The flight cycle: game frames per image. 2 swaps the image 30 times a
   second, i.e. a full two-image beat at 15 Hz. 1 would be the fastest the
   hardware can show at all; 2 is the brief's "very quickly" with each image
   still held long enough to register. Each maggot starts at a random phase so
   a swarm does not flap in lockstep. */
#define MGT_ANIM_FRAMES        2

/* The buzz is re-keyed from C every MGT_BUZZ_FRAMES, which is the clip's own
   length: buzz.vag is 2.17 s at 11025 Hz (855 ADPCM blocks), and
   2.17 * 60 = 130.3, so 130 lands the restart a third of a frame before the
   sample ends rather than leaving a gap. Re-measure if the clip is re-cut. */
#define MGT_BUZZ_FRAMES      130

typedef enum {
    MGT_ATTACKING,  /* the only living state: from spawn, straight at the player */
    MGT_DEAD,
} MaggotState;

typedef struct {
    int32_t     x, y, z;       /* y is the FLOOR ANCHOR; see maggot_body_y()    */
    int32_t     vy;            /* apply_ddog_height's, to keep y on the floor    */
    int32_t     float_off;     /* this body's share of MGT_FLOAT_VARY, latched   */
    int         anim_phase;    /* offset into the flight cycle, latched          */
    int         health;
    int         damage_timer;
    MaggotState state;
    int32_t     active;
    int         on_upper_floor;
    int         on_ramp;
    GameState   area;
} Maggot;

extern Maggot maggots[MAX_MAGGOTS];
extern int    maggot_count;

/* Startup: register the sprite. DEFERRED, like all Chapter 3 art — the pixels
   only arrive once TEXBANK_CATACOMBS is selected at the catacomb mouth. */
void maggots_load_textures(void);
/* Room entry: pure LoadImage out of the RAM copy. main.c's STATE_LOADING calls
   it on entry to every room in the Catacombs bank, beside the lumberer's. */
void maggots_upload_textures(void);

void maggots_init(void);
void maggots_reset(void);       /* drop the whole pool and silence the buzz     */

/* Put one maggot into the world at (x,z), attacking from this frame on.
   Returns its index, or -1 if the pool is full.
 *
 * anchor_y is the STANDING ANCHOR for the floor under the spawn, i.e.
 * (floor surface) - GROUND_FLOOR_Y — -149 on an ordinary y=0 mesh floor. NOT a
 * body height: apply_ddog_height refuses to lift an entity that starts below a
 * floor zone, so an anchor seeded too low sinks the maggot through the world
 * (the two-heights bug creep.h's creep_spawn() note describes). The body is
 * drawn at its float height above this from the first frame. */
int  maggot_spawn(int32_t x, int32_t z, int32_t anchor_y, GameState area);

/* Where this maggot's BODY is, in world y. The one place the hover is
   resolved — the sprite, the axe's reach and the gun's aim all read it. */
int32_t maggot_body_y(const Maggot *m);

/* How many maggots tagged to `area` are still alive. */
int  maggots_alive_in(GameState area);

void update_maggots(void);
void draw_maggots(RenderContext *ctx);

/* Deal damage from any source. Two calls of 1 kill it. */
void maggot_damage(Maggot *m, int dmg);

/* 1x from every damage type, explicitly (see MGT_MAX_HEALTH). */
int32_t maggot_scale_damage(int32_t base, DamageType type);

/* The crucifaxe's hit test, measured to the body's own geometry — the creep's
   creeps_try_hit(), for the same reason. Returns 1 if a maggot was hit. */
int  maggots_try_hit(void);

/* Tell the renderer which texture window the current area has active.
   >>> UNLIKE THE CREEP, THIS ONE IS READ. <<< maggot.tim sits at x672 y320:
   Voff 64, but x672 is HALF WAY across its tpage (x640), so U runs 128..255
   and a room's 128-wide window would wrap it back onto 0..127 — somebody
   else's art. Each sprite is therefore drawn under a full/unmasked window and
   this rect restored after it, spider.c's add_ft4_windowed(). */
void maggots_set_texwindow(const RECT *tw);

#endif
