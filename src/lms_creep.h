#ifndef LMS_CREEP_H
#define LMS_CREEP_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState — each creep is tagged with its area */

/* -----------------------------------------------------------------------
 * LAMASHTU CREEP — what Lamashtu pours out of her own body in the Lamashtu
 * Arena (src/lamashtu.h). A copy of src/creep.c, the crib's Creep, and
 * DELIBERATELY a copy rather than a flag on it: the two differ in their art,
 * their size and their weaknesses, and the crib rooms should not have to
 * reason about an arena they never see. Read src/creep.h for everything the
 * two still share - the ghost movement (no world collision), the separation
 * pass that keeps a swarm from collapsing into one sprite, the float at eye
 * level, the stacking contact damage, the transient pool.
 *
 * WHAT IS DIFFERENT, and nothing else is:
 *
 *   ART       textures/lamashtu/lamashtu creep.png -> textures/lamashtu creep.tim,
 *             8bpp 64x64 ON THE CREEP'S OWN SLOT x[640,672) y[320,384) and its
 *             CLUT line (672,511): a time-share. No crib stands in the arena,
 *             and every crib room's uploader puts the Creep's pixels and
 *             palette back on its way in. The UVs are the Creep's exactly.
 *
 *   SIZE      2/3 of the Creep's: 90 x 90 against 136 x 136. The separation
 *             radii shrink with it (the Creep's note on why they are sized to
 *             the sprite), the reach and the hold distance do not.
 *
 *   THE LANTERN can hurt it. The Creep is not in src/helluminator.c's sweep;
 *             this one is, at the Creep's one hit point, so a tick of burn
 *             kills it.
 *
 *   THE CRY.  SFX_CREEP - the cribs' baby cry - plays ON A LOOP for as long as
 *             any of them is close enough to bite, and stops when none is. It
 *             is re-keyed from C every LCR_CRY_FRAMES (the clip's length), the
 *             crib's way, so voice 20 is never given a hardware loop.
 *
 *   THE POOL reuses a dead slot before it appends: Lamashtu pours them for the
 *             whole of her fight, and the Creep's append-only pool would run
 *             dry on the thirteenth.
 * ----------------------------------------------------------------------- */

/* The whole-game pool, not a per-room one. A crib releases CRIB_CREEP_TOTAL
   (10); the two spare are headroom for a second crib being woken before the
   first one's last body has been cleared, which the code allows even though no
   room places two. lms_creep_spawn() returns -1 and places nothing when it is full,
   and the crib counts that as a spawn anyway (see crib.c) so a full pool cannot
   deadlock an encounter that is waiting on ten of them. */
#define MAX_LMS_CREEPS            12

#define LCR_MAX_HEALTH         1   /* anything that connects kills it          */
#define LCR_SPEED             11   /* a shade quicker than the demon dog's 10  */

/* Contact: a quarter of MAX_HEALTH (100) per second, i.e. 5 points every 12
   frames, PER CREEP. LCR_CATCH_DIST is Manhattan, as every enemy's reach test
   in this game is, and it has to cover a creep that has stopped on the
   DIAGONAL: the stop below is a true radius, and a radius of 110 reaches a
   Manhattan 110 * sqrt(2) = 156. 170 clears that with a margin rather than
   leaving a ring of lms_creeps that have arrived and cannot bite. */
#define LCR_CATCH_DIST       170
#define LCR_DAMAGE_AMOUNT      5
#define LCR_DAMAGE_TICK       12

/* Where it stops: a TRUE radial distance from the player, so the ring is round
   whichever bearing a creep came in on. Comfortably inside the crucifaxe's
   SWING_RANGE, which is what keeps a held player able to answer them. */
#define LCR_STOP_DIST        110

/* The cry's re-key: creep.vag's length, 2302 ADPCM blocks of 28 samples at
   11025 Hz = 5.85 s - the crib's CRIB_LOOP_FRAMES. */
#define LCR_CRY_FRAMES       350

/* Sprite. 136 x 136 world units — a square, because the art is. That is 68 half
   extents against the 45 this shipped at: a 50% increase in each axis, which the
   first pass was asked for after 90 x 90 read as too small next to the demon
   dog's 180 x 120. A creep is now about three quarters of the dog in each
   direction and still comfortably the smallest thing in the game.

   >>> IT DOES NOT KEEP THE LCR_Y_OFFSET + LCR_HALF_H == 150 INVARIANT, AND
   NOTHING HERE SHOULD. <<< Every OTHER sprite enemy in this game does (zombie
   25 + 125, spider 57 + 93, mushroom 25 + 125) because that sum is the drop from
   the entity anchor to the FEET, and they all stand on the floor. A creep
   FLOATS — see the block below — so it has no feet, the sum is meaningless for
   it, and LCR_Y_OFFSET is NEGATIVE. Do not "restore" the invariant here; the
   thing that has to hold instead is the eye-level arithmetic below.

   >>> AND THERE ARE NO SHADOW EXTENTS TO SCALE ALONGSIDE THESE ANY MORE. <<<
   There were - LCR_SHADOW_W/D, a floor decal sized in plan - and they went with
   the shadow itself: this enemy floats and nothing about it touches the ground,
   so src/creep.c draws no decal where every other enemy in the game draws one.
   The note on that removal, and why putting it back is not the fix for a creep
   that is hard to place on the floor, is at the top of the .c. */
/* 2/3 OF THE CREEP'S 68 (lms_creep.h's head): 90 x 90. */
#define LCR_HALF_W            45
#define LCR_HALF_H            45

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
 * rather than a second physics model. lms_creep_body_y() is the single place that
 * resolves it, and every reader — the sprite, the axe's reach, the gun's aim —
 * goes through that function rather than adding LCR_Y_OFFSET itself.
 *
 * THE ARITHMETIC, and it is worth writing down because the two heights come
 * from different functions (tools/ADDING_AN_ENEMY.txt mistake 14):
 *   an enemy's anchor   apply_ddog_height  ->  zone->y - GROUND_FLOOR_Y
 *   the player's eye    apply_height       ->  zone->y - GROUND_FLOOR_Y - 40
 * so the eye sits LCR_EYE_RISE (40) above an enemy anchor on the same floor,
 * permanently, on every frame of the game. Body centre = anchor + LCR_Y_OFFSET,
 * and LCR_Y_OFFSET is therefore (how far below the eye we want to be) minus
 * that 40 — a negative number, -22, meaning 22 units under the player's eye. */
#define LCR_EYE_RISE          40
#define LCR_BELOW_EYE         18
#define LCR_Y_OFFSET         (LCR_BELOW_EYE - LCR_EYE_RISE)

/* Per-creep variation about that height, latched at spawn: each body picks a
   number in [-LCR_FLOAT_VARY, +LCR_FLOAT_VARY] and keeps it, so a swarm is a
   loose cloud rather than ten sprites on one ruled line. 35 either way puts the
   highest 13 above the eye and the lowest 53 below it — visibly uneven, and
   short of any of them leaving the frame or dipping into the floor (the lowest
   body's bottom edge is still 68 clear of it).

   STATIC PER CREEP, NOT AN ANIMATED BOB. The brief asked for variation between
   them, not motion; and a sine bob on ten bodies is ten more sine calls a frame
   for something the eye reads as drift anyway once they are all moving. */
#define LCR_FLOAT_VARY        35

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
#define LCR_EMERGE_FRAMES     24

/* Separation, and it is the whole of the steering now that the lms_creeps are
   ghosts and all home on one point (see the note above on why this is
   load-bearing rather than polish). LCR_SEP_RADIUS is the range of the SOFT push
   that biases the direction; LCR_BODY_RADIUS * 2 is the hard minimum the
   post-move pass enforces. Both scaled with LCR_HALF_W when the body grew 50% —
   a 136-wide sprite held apart by a radius sized for a 90-wide one visibly
   interpenetrates.

   There is no LCR_FEELER_LEN, LCR_STEER_COMMIT or LCR_DOOR_CLEARANCE any more.
   Those belonged to the wall-follow layer, and a ghost has no walls. */
/* The Creep's 60 and 150, scaled by the same 2/3 as the sprite. */
#define LCR_BODY_RADIUS       40
#define LCR_SEP_RADIUS       100
#define LCR_SEP_WEIGHT         2

typedef enum {
    LCR_EMERGING,   /* rising (or settling) out of the crib to its float height */
    LCR_HUNTING,    /* adrift at eye level, homing on the player                */
    LCR_DEAD,
} LmsLmsCreepState;

typedef struct {
    int32_t    x, y, z;        /* y is the FLOOR ANCHOR; the body is lifted off
                                  it at draw time — see lms_creep_body_y()          */
    int32_t    vy;             /* apply_ddog_height's, to keep y on the floor    */
    int32_t    float_off;      /* this body's share of LCR_FLOAT_VARY, latched   */
    int32_t    emerge_y;       /* the crib corner it came out of, WORLD y        */
    int        emerge;         /* frames of LCR_EMERGE_FRAMES left               */
    int        health;
    int        damage_timer;
    LmsLmsCreepState state;
    int32_t    active;
    int        on_upper_floor;
    int        on_ramp;
    GameState  area;
} LmsCreep;

extern LmsCreep lms_creeps[MAX_LMS_CREEPS];
extern int   lms_creep_count;

/* Startup: register the sprite. Deferred, like the rest of Chapter 3's art —
   the header is read at boot and the pixels only when TEXBANK_CATACOMBS is
   selected at the catacomb mouth. */
void lms_creeps_load_textures(void);
/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. No CD access, so it is safe inside main's STATE_LOADING. Called from
   room_of_arms_upload_textures() beside the crib's. */
void lms_creeps_upload_texture(void);

void lms_creeps_init(void);
void lms_creeps_reset(void);        /* drop the whole pool — new game, room change */

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
int  lms_creep_spawn(int32_t x, int32_t z, int32_t anchor_y, int32_t emerge_y,
                 GameState area);

/* Where this creep's BODY is drawn, in world y: its floor anchor lifted to eye
   level by LCR_Y_OFFSET and its own float_off, or the emergence interpolation
   while it is still coming out of the cot. THE ONE PLACE THAT RESOLVES IT —
   src/graveolver.c aims through this and src/creep.c draws and swings through
   it, so the hover can be retuned without hunting for open-coded copies of the
   offset. */
int32_t lms_creep_body_y(const LmsCreep *c);

/* How many lms_creeps tagged to `area` are still alive. The crib polls this to
   decide when its encounter is over. */
int  lms_creeps_alive_in(GameState area);

/* Kill every one alive in `area`, each as if struck (blood burst). Lamashtu's
   hit does this: her Creeps die with the blow and start again from wherever
   she reappears. */
void lms_creeps_kill_all(GameState area);

void update_lms_creeps(void);
void draw_lms_creeps(RenderContext *ctx);

/* Deal damage from any source. At LCR_MAX_HEALTH 1 every call that reaches this
   is fatal, but it takes a `dmg` like every other enemy's so the weapons can
   stay uniform. */
void lms_creep_damage(LmsCreep *c, int dmg);

/* Scale a hit by this enemy's weaknesses (see damage.h). It HAS none, by
   design, and the table is the empty placeholder every enemy carries so that
   giving it one later is a line rather than a refactor. */
int32_t lms_creep_scale_damage(int32_t base, DamageType type);

/* The crucifaxe's hit test, kept in this module rather than inlined into
   crucifaxe.c. Returns 1 if a creep was hit. It is here for the tentacle's and
   the Rafflesia's reason: a creep's body is small and low, so the reach has to
   be measured against ITS half-extents rather than against the one-size
   Manhattan budget the inlined blocks in crucifaxe.c use. */
int  lms_creeps_try_hit(void);

/* Tell the renderer which texture window the current area has active. LmsCreeps
   are exempt by placement — creep.tim sits at Voff 0 and Uoff 0 (see disc.xml)
   — so this currently stores the rect and nothing reads it. It exists so that
   the day the sprite moves to a page that DOES need bracketing, the room-side
   wiring is already in place and the change is confined to this file. */
void lms_creeps_set_texwindow(const RECT *tw);

#endif
