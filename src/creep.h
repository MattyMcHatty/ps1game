#ifndef CREEP_H
#define CREEP_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState — each creep is tagged with its area */

/* -----------------------------------------------------------------------
 * Creep — the small swarming thing the Crib pours out of itself.
 *
 * Read src/crib.h first if you are here because of the encounter: this file is
 * only the MONSTER, and every decision about when and where ten of them appear
 * belongs to the prop. The one thing the two share is creep_spawn(), which the
 * crib calls and nothing else does.
 *
 * >>> IT IS THE FIRST ENEMY IN THIS GAME THAT IS SPAWNED RATHER THAN PLACED,
 * AND THAT CHANGES ITS PERSISTENCE MODEL ENTIRELY. <<<
 * Every other enemy is seeded once in world_enter()'s first-visit block, put
 * back at its spawn by a *_rest() on the way out of the room, and remembered
 * across a save as one dead/alive bit per placement (tools/ADDING_AN_ENEMY.txt
 * STEP 6, models A and B). A creep has no placement to go back to and no bit to
 * carry: it did not exist when the player walked in and it will not exist when
 * they walk out.
 *
 * So it is the THIRD model, the one src/web.c already uses for the spider's
 * projectiles: a fixed pool, TRANSIENT, with no world.c wiring of any kind.
 * There is no creeps_rest(), no WorldDelta field, no _Static_assert and no
 * SAVE_VERSION cost. The whole obligation is creeps_reset() in reset_game() and
 * on leaving a room. What DOES persist is one bit saying the crib's encounter
 * was beaten, and that bit lives with the crib (src/crib.h) rather than here,
 * because it is a fact about the prop and not about any monster.
 *
 * The consequence is worth stating plainly: THE ENCOUNTER IS NOT RESUMABLE.
 * Leave the room, die, or load a save in the middle of it and the creeps are
 * gone and the crib is back to being a crib, ready to be woken again. Only
 * finishing it sticks. That is the same bargain the Greenhouse flood makes
 * (greenhouse_flood.h) and it is the right one here for the same reason: a
 * half-poured crib is a running timer and nine positions, none of which is
 * re-derivable, against one bit for the outcome.
 *
 * ---- Behaviour -------------------------------------------------------------
 * ONE HP AND NO WEAKNESSES, so anything that touches a creep kills it: one
 * crucifaxe swing, one round of any type, one tick of the lantern. There is no
 * health bar for the same reason — a bar that is only ever full or absent is a
 * pixel of noise on ten bodies at once.
 *
 * IT GOES STRAIGHT FOR THE PLAYER'S CENTRE and holds at CRP_STOP_DIST. There
 * are no approach lanes: an earlier version latched each creep to one of the
 * eight non-central cells of a 3x3 grid in the camera's frame so the swarm
 * arrived spread out, and that is gone. They all home on the same point.
 *
 * >>> WHICH MAKES THE SEPARATION PASS LOAD-BEARING RATHER THAN COSMETIC. <<<
 * Ten bodies homing on one point with nothing pushing them apart is ONE visible
 * sprite with nine hidden inside it — the player would see a single creep, take
 * 50 hp/sec off it, and have nothing to aim at. The soft push in the steering
 * and the hard push after every body has moved are now the only thing spreading
 * the swarm into a ring, so CRP_SEP_RADIUS and CRP_BODY_RADIUS are gameplay
 * numbers here and not polish. Do not remove either without replacing what they
 * do.
 *
 * ---- THEY ARE GHOSTS ------------------------------------------------------
 * >>> A CREEP HAS NO COLLISION WITH THE WORLD AT ALL. <<< No wall push, no
 * crates, no fat doors, no feeler, no wall-follow commit and no sightline test.
 * It drifts through the room's geometry and comes out the other side. That is
 * the whole of the movement code now: a direction, a separation bias, and a
 * step.
 *
 * This is worth stating loudly because it is the opposite of every other enemy
 * in src/, and a reader coming from one of them will assume the collision calls
 * were forgotten. They were deleted. The entire local steering layer went with
 * them — tools/ADDING_AN_ENEMY.txt mistakes 16 and 17 are both about machinery a
 * creep does not have, and mistake 17's "any enemy in a room with a
 * free-standing obstacle needs a routing layer too" does not apply to something
 * that treats obstacles as scenery.
 *
 * ONE THING IS STILL PROBED: THE FLOOR, AND THE HOVER HEIGHT IS NOW ITS ONLY
 * CONSUMER. apply_ddog_height keeps c->y on the ground under the body, because
 * that is what the hover height is measured from - the drop shadow was the other
 * reader of it and this enemy no longer has one — it is a height query, not a collision. A creep that
 * drifts outside every floor zone would match none, take the function's
 * `target = 0` default and sink; the Room of Arms cannot show that (its one
 * zone's rect is deliberately larger than its walkable floor), but a room whose
 * zones stop at its walls would, and that is the thing to check before standing
 * a crib in one.
 *
 * ---- Contact damage --------------------------------------------------------
 * A QUARTER of MAX_HEALTH per second per creep — 5 points every 12 frames, the
 * mushroom's arithmetic (mushroom.h). >>> AND IT STACKS: each creep in contact
 * runs its own timer. <<< Ten of them touching at once is 50 hp/sec and the
 * player is dead in two seconds, which is the point: the encounter is lost by
 * letting them gather, not by any single one of them. Nothing clamps the total,
 * deliberately — see the note on the damage block in update_creeps().
 *
 * ---- Steering --------------------------------------------------------------
 * NO HEADING BLEND. Every other enemy in this game smooths its turn with
 *     blend = (prev * (8 - RATE) + move * RATE) >> 3
 * and tools/ADDING_AN_ENEMY.txt mistake 16 lists the five separate ways that
 * line is wrong at the size these numbers are (a fixed point at zero, a
 * negative component that never decays, a face() helper at the wrong scale,
 * truncation snapping a turn back onto its axis, and a lerp that cannot
 * reverse). A creep simply does not have one: it is a small fast thing that
 * scurries, an instant turn reads as correct on it, and the cheapest way not to
 * inherit a known-broken line is not to copy it. Do not "fix" this by adding
 * the blend back.
 *
 * It keeps the feeler and the wall-follow commit, because those are a LOCAL
 * rule about getting along a face it is already touching and the Room of Arms
 * has a screen wall and an alcove to get round (mistake 17).
 * ----------------------------------------------------------------------- */

/* The whole-game pool, not a per-room one. A crib releases CRIB_CREEP_TOTAL
   (10); the two spare are headroom for a second crib being woken before the
   first one's last body has been cleared, which the code allows even though no
   room places two. creep_spawn() returns -1 and places nothing when it is full,
   and the crib counts that as a spawn anyway (see crib.c) so a full pool cannot
   deadlock an encounter that is waiting on ten of them. */
#define MAX_CREEPS            12

#define CRP_MAX_HEALTH         1   /* anything that connects kills it          */
#define CRP_SPEED             11   /* a shade quicker than the demon dog's 10  */

/* Contact: a quarter of MAX_HEALTH (100) per second, i.e. 5 points every 12
   frames, PER CREEP. CRP_CATCH_DIST is Manhattan, as every enemy's reach test
   in this game is, and it has to cover a creep that has stopped on the
   DIAGONAL: the stop below is a true radius, and a radius of 110 reaches a
   Manhattan 110 * sqrt(2) = 156. 170 clears that with a margin rather than
   leaving a ring of creeps that have arrived and cannot bite. */
#define CRP_CATCH_DIST       170
#define CRP_DAMAGE_AMOUNT      5
#define CRP_DAMAGE_TICK       12

/* Where it stops: a TRUE radial distance from the player, so the ring is round
   whichever bearing a creep came in on. Comfortably inside the crucifaxe's
   SWING_RANGE, which is what keeps a held player able to answer them. */
#define CRP_STOP_DIST        110

/* Sprite. 136 x 136 world units — a square, because the art is. That is 68 half
   extents against the 45 this shipped at: a 50% increase in each axis, which the
   first pass was asked for after 90 x 90 read as too small next to the demon
   dog's 180 x 120. A creep is now about three quarters of the dog in each
   direction and still comfortably the smallest thing in the game.

   >>> IT DOES NOT KEEP THE CRP_Y_OFFSET + CRP_HALF_H == 150 INVARIANT, AND
   NOTHING HERE SHOULD. <<< Every OTHER sprite enemy in this game does (zombie
   25 + 125, spider 57 + 93, mushroom 25 + 125) because that sum is the drop from
   the entity anchor to the FEET, and they all stand on the floor. A creep
   FLOATS — see the block below — so it has no feet, the sum is meaningless for
   it, and CRP_Y_OFFSET is NEGATIVE. Do not "restore" the invariant here; the
   thing that has to hold instead is the eye-level arithmetic below.

   >>> AND THERE ARE NO SHADOW EXTENTS TO SCALE ALONGSIDE THESE ANY MORE. <<<
   There were - CRP_SHADOW_W/D, a floor decal sized in plan - and they went with
   the shadow itself: this enemy floats and nothing about it touches the ground,
   so src/creep.c draws no decal where every other enemy in the game draws one.
   The note on that removal, and why putting it back is not the fix for a creep
   that is hard to place on the floor, is at the top of the .c. */
#define CRP_HALF_W            68
#define CRP_HALF_H            68

/* ---- FLOATING AT EYE LEVEL -----------------------------------------------
 * A creep drifts at just below the player's eye line instead of crawling along
 * the floor. The first pass had it on the floor and the floor is exactly where a
 * small body cannot be seen once it is close: the camera looks out roughly
 * level, so anything at ankle height leaves the bottom of the screen at about
 * the distance it becomes dangerous. Ten of them could be biting from outside
 * the frame.
 *
 * THE ANCHOR IS STILL ON THE FLOOR and only the DRAWN BODY is lifted. That is
 * deliberate: c->y stays whatever apply_ddog_height settles it to, so the floor
 * probe, the ramps, the multi-storey zones and the drop shadow all keep working
 * unchanged, and "how high does it hover" is one offset applied at draw time
 * rather than a second physics model. creep_body_y() is the single place that
 * resolves it, and every reader — the sprite, the axe's reach, the gun's aim —
 * goes through that function rather than adding CRP_Y_OFFSET itself.
 *
 * THE ARITHMETIC, and it is worth writing down because the two heights come
 * from different functions (tools/ADDING_AN_ENEMY.txt mistake 14):
 *   an enemy's anchor   apply_ddog_height  ->  zone->y - GROUND_FLOOR_Y
 *   the player's eye    apply_height       ->  zone->y - GROUND_FLOOR_Y - 40
 * so the eye sits CRP_EYE_RISE (40) above an enemy anchor on the same floor,
 * permanently, on every frame of the game. Body centre = anchor + CRP_Y_OFFSET,
 * and CRP_Y_OFFSET is therefore (how far below the eye we want to be) minus
 * that 40 — a negative number, -22, meaning 22 units under the player's eye. */
#define CRP_EYE_RISE          40
#define CRP_BELOW_EYE         18
#define CRP_Y_OFFSET         (CRP_BELOW_EYE - CRP_EYE_RISE)

/* Per-creep variation about that height, latched at spawn: each body picks a
   number in [-CRP_FLOAT_VARY, +CRP_FLOAT_VARY] and keeps it, so a swarm is a
   loose cloud rather than ten sprites on one ruled line. 35 either way puts the
   highest 13 above the eye and the lowest 53 below it — visibly uneven, and
   short of any of them leaving the frame or dipping into the floor (the lowest
   body's bottom edge is still 68 clear of it).

   STATIC PER CREEP, NOT AN ANIMATED BOB. The brief asked for variation between
   them, not motion; and a sine bob on ten bodies is ten more sine calls a frame
   for something the eye reads as drift anyway once they are all moving. */
#define CRP_FLOAT_VARY        35

/* Coming out of the cot. A floating creep does NOT fall, so there is no gravity
   beat here and no landing to wait for: the body is interpolated from the crib
   corner it was released at up (or down) to its float height over these frames,
   holding position horizontally, and then it hunts. 24 frames is four tenths of
   a second — long enough to read as emerging, short enough that the first body
   is already coming for the player while the beam is still the loudest thing in
   the room.

   >>> THIS REPLACED A GRAVITY DROP AND IT HAD TO. <<< The three spawn points are
   all at the cot's rim, 195 above the floor, and the float height is about 171
   above it — so today every creep comes DOWN a modest 24 units and gravity would
   in fact have served. It did not when there were EIGHT spawn points straddling
   the float height (four on the rim at 195, four on the deck at 45): a deck
   release has to go UP to eye level and apply_ddog_height only falls, so those
   four snapped. The interpolation is kept now that they are gone because it is
   agnostic about direction and a spawn point below eye level is an obvious thing
   to add later — the geometry that broke gravity is one constant away. */
#define CRP_EMERGE_FRAMES     24

/* Separation, and it is the whole of the steering now that the creeps are
   ghosts and all home on one point (see the note above on why this is
   load-bearing rather than polish). CRP_SEP_RADIUS is the range of the SOFT push
   that biases the direction; CRP_BODY_RADIUS * 2 is the hard minimum the
   post-move pass enforces. Both scaled with CRP_HALF_W when the body grew 50% —
   a 136-wide sprite held apart by a radius sized for a 90-wide one visibly
   interpenetrates.

   There is no CRP_FEELER_LEN, CRP_STEER_COMMIT or CRP_DOOR_CLEARANCE any more.
   Those belonged to the wall-follow layer, and a ghost has no walls. */
#define CRP_BODY_RADIUS       60
#define CRP_SEP_RADIUS       150
#define CRP_SEP_WEIGHT         2

typedef enum {
    CRP_EMERGING,   /* rising (or settling) out of the crib to its float height */
    CRP_HUNTING,    /* adrift at eye level, homing on the player                */
    CRP_DEAD,
} CreepState;

typedef struct {
    int32_t    x, y, z;        /* y is the FLOOR ANCHOR; the body is lifted off
                                  it at draw time — see creep_body_y()          */
    int32_t    vy;             /* apply_ddog_height's, to keep y on the floor    */
    int32_t    float_off;      /* this body's share of CRP_FLOAT_VARY, latched   */
    int32_t    emerge_y;       /* the crib corner it came out of, WORLD y        */
    int        emerge;         /* frames of CRP_EMERGE_FRAMES left               */
    int        health;
    int        damage_timer;
    CreepState state;
    int32_t    active;
    int        on_upper_floor;
    int        on_ramp;
    GameState  area;
} Creep;

extern Creep creeps[MAX_CREEPS];
extern int   creep_count;

/* Startup: register the sprite. Deferred, like the rest of Chapter 3's art —
   the header is read at boot and the pixels only when TEXBANK_CATACOMBS is
   selected at the catacomb mouth. */
void creeps_load_textures(void);
/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. No CD access, so it is safe inside main's STATE_LOADING. Called from
   room_of_arms_upload_textures() beside the crib's. */
void creeps_upload_texture(void);

void creeps_init(void);
void creeps_reset(void);        /* drop the whole pool — new game, room change */

/* Put one creep into the world at (x,z), emerging from one of the crib's three
   spawn points, with its float offset latched here. Returns its index, or -1 if
   the pool is full. THE CRIB IS THE ONLY CALLER (src/crib.c).
 *
 * >>> IT TAKES TWO HEIGHTS AND THEY ARE NOT INTERCHANGEABLE. <<<
 *   anchor_y  the STANDING ANCHOR for the floor under the spawn, i.e.
 *             (floor surface) - GROUND_FLOOR_Y. c->y is seeded from this.
 *   emerge_y  the spawn point's own WORLD y — where the BODY starts, before it
 *             interpolates to its float height.
 *
 * They were one argument first, and that was a bug that only the higher spawn
 * points survived. apply_ddog_height REFUSES to lift an entity that starts below
 * a floor zone — `if (zone_target < *py - 2) continue;` in collision.c, the rule
 * that stops something on a lower storey being yanked onto an upper one — so
 * seeding the anchor from a point BELOW the standing anchor made every zone fall
 * through, left `target` at 0, and dropped the creep a further 149 units under
 * the floor. (It was FOUND by its shadow sitting below the world, back when it
 * had one. The sink is just as real without that tell and there is no longer a
 * decal to give it away, which is worth knowing before trusting this spawn path
 * again.) All three spawn points sit at the
 * cot's rim today and so are safely above it; passing the emergence height here
 * would still be wrong, and would bite again the moment a lower one is added.
 *
 * A crib's own `y` field IS the anchor it should pass, as it happens: the prop
 * stores a floor reference such that world y = y + GROUND_FLOOR_Y, so
 * (y + GROUND_FLOOR_Y) - GROUND_FLOOR_Y is y again. */
int  creep_spawn(int32_t x, int32_t z, int32_t anchor_y, int32_t emerge_y,
                 GameState area);

/* Where this creep's BODY is drawn, in world y: its floor anchor lifted to eye
   level by CRP_Y_OFFSET and its own float_off, or the emergence interpolation
   while it is still coming out of the cot. THE ONE PLACE THAT RESOLVES IT —
   src/graveolver.c aims through this and src/creep.c draws and swings through
   it, so the hover can be retuned without hunting for open-coded copies of the
   offset. */
int32_t creep_body_y(const Creep *c);

/* How many creeps tagged to `area` are still alive. The crib polls this to
   decide when its encounter is over. */
int  creeps_alive_in(GameState area);

void update_creeps(void);
void draw_creeps(RenderContext *ctx);

/* Deal damage from any source. At CRP_MAX_HEALTH 1 every call that reaches this
   is fatal, but it takes a `dmg` like every other enemy's so the weapons can
   stay uniform. */
void creep_damage(Creep *c, int dmg);

/* Scale a hit by this enemy's weaknesses (see damage.h). It HAS none, by
   design, and the table is the empty placeholder every enemy carries so that
   giving it one later is a line rather than a refactor. */
int32_t creep_scale_damage(int32_t base, DamageType type);

/* The crucifaxe's hit test, kept in this module rather than inlined into
   crucifaxe.c. Returns 1 if a creep was hit. It is here for the tentacle's and
   the Rafflesia's reason: a creep's body is small and low, so the reach has to
   be measured against ITS half-extents rather than against the one-size
   Manhattan budget the inlined blocks in crucifaxe.c use. */
int  creeps_try_hit(void);

/* Tell the renderer which texture window the current area has active. Creeps
   are exempt by placement — creep.tim sits at Voff 0 and Uoff 0 (see disc.xml)
   — so this currently stores the rect and nothing reads it. It exists so that
   the day the sprite moves to a page that DOES need bracketing, the room-side
   wiring is already in place and the change is confined to this file. */
void creeps_set_texwindow(const RECT *tw);

#endif
