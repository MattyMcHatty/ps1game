#ifndef LIVING_STATUE_H
#define LIVING_STATUE_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState — each statue is tagged with its area */

/* -----------------------------------------------------------------------
 * Living Statue — the thing on the plinth that is only ever still while you
 * are looking at it.
 *
 * Built on the Mushroom Head (src/mushroom.c, itself built on the spider and
 * the zombie), and it drops most of that lineage: no separation, no knockback,
 * no shadow, no walk cycle. It has TWO movement models and they belong to
 * different halves of its life — it TELEPORTS while it is stalking, and once it
 * has arrived it stops teleporting for good and WALKS. Most of this file is
 * about the first: when a jump is allowed and where it may land. The second is
 * the mushroom's feeler steering, kept whole because a maze punishes anything
 * that charges in a straight line (ls_steer).
 *
 * THE STATES
 *
 *   IDLE    A statue. It does nothing at all and looks like set dressing —
 *           which, for a placement made with `living` = 0, is exactly what it
 *           is and what it stays (see THE DEAD ONES below).
 *   STALK   Armed. The player has been inside LST_ACTIVATE_RADIUS once, and
 *           that is irreversible. It still does nothing while it can be seen
 *           or while the player is close; the teleport cycle only turns over
 *           under the two gates below.
 *   ATTACK  It has closed to one floor poly. The texture swaps to
 *           living_statue_atk, the quad flares out to its full height in width
 *           (LST_ATK_HALF_W), and it takes a fifth of the player's health away
 *           every second for as long as they are inside LST_CATCH_DIST.
 *           >>> AND IT STOPS BEING A STATUE. <<< It teleports no more, and both
 *           gates below stop applying to it: it WALKS after the player at their
 *           own walk speed, watched or not, until it is destroyed or they are
 *           dead. Standing still was a stalking behaviour, and the stalk is
 *           over. See LST_CHASE_SPEED.
 *   DEAD
 *
 * THE TWO GATES ON A TELEPORT. Both must hold, every frame, or LST_TP_INTERVAL
 * is put back to full — the countdown does not pause, it RESETS:
 *
 *   1. THE PLAYER MUST BE LOOKING AWAY. Tested as the dot of the camera's
 *      forward vector with the offset to the statue: > 0 means the statue is
 *      somewhere in the 180 degrees the player is facing, and it freezes. That
 *      is deliberately the whole front half-plane and not the ~60 degrees the
 *      screen actually shows, because it is the only formulation that
 *      GUARANTEES a teleport is never drawn happening. A statue that visibly
 *      jumped once would stop being frightening for the rest of the game.
 *   2. THE PLAYER MUST BE FAR ENOUGH AWAY — beyond LST_STALK_GATE, three times
 *      the activation radius.
 *
 * >>> GATE 2 RE-FREEZES, AND THAT HAS A CONSEQUENCE WORTH KNOWING. <<<
 * It is re-tested every frame, not just once to start the sequence. So the
 * first teleport (which lands within six polys, i.e. 1200 — just outside the
 * 1050 gate) leaves the statue still able to move, but the SECOND lands it at
 * 800, inside the gate, and it then holds there until the player puts distance
 * back between them. In practice that means a statue closes on someone who is
 * RUNNING and stalls on someone who stands still with their back to it. That
 * is the design as specified; if it should instead run the four teleports to
 * completion once started, clear LST_REFREEZE and nothing else changes.
 *
 * THE FOUR TELEPORTS. Each one lands on a floor poly BEHIND the player, no
 * further from them than the stage's range:
 *
 *     stage 0   within 6 polys   1200
 *     stage 1   within 4 polys    800
 *     stage 2   within 2 polys    400
 *     stage 3+  within 1 poly     200   -> and it is in ATTACK from here on
 *
 * ...unless the player is ALREADY closer than the stage's range when the
 * countdown fires, in which case that stage is skipped and it goes straight to
 * one poly. A statue armed at point-blank range does not politely reappear six
 * polys away.
 *
 * Reaching one poly is the LAST teleport it ever makes. From there it walks —
 * see ATTACK above.
 *
 * THE LANDING SPOT is searched for, not computed: ls_pick_landing samples
 * random bearings in the rear hemisphere and, for each, walks the radius down
 * from the stage's range in LST_TP_STEP increments, taking the first point that
 * is on a floor zone and not inside a wall. Failing all of them it teleports
 * NOWHERE and leaves the countdown expired, so it retries next frame. That
 * matters in a hedge maze: most bearings at 1200 units are inside a hedge, and
 * the alternative to searching is a statue embedded in the greenery.
 *
 * DAMAGE. 2 HP, and the crucifaxe is the ONLY thing that can spend them — there
 * is no Grave-olver targeting loop for this enemy anywhere in the codebase, on
 * purpose, so a round fired at one is simply a wasted round. Worse, the axe
 * only bites while the statue is in ATTACK: a swing at a stalking one is a
 * silent no-op that does not even consume the swing (crucifaxe.c skips
 * non-ATTACK statues before its reach test, so the same swing can still land on
 * something else). That is the specified behaviour and it is meant to be a dead
 * end for now — the weapon that can break a stalking statue is not designed
 * yet. Death is grey, not red: LST_BLOOD_* are the stone chips.
 *
 * THE DEAD ONES. living_statue_add() takes a `living` flag, so the same enemy
 * places as either the real thing or as ordinary masonry. A `living` = 0 statue
 * never leaves IDLE, never arms, never teleports and cannot be damaged, but it
 * is still SOLID and still drawn — which is the entire point, because it is the
 * cover the living ones hide behind.
 *
 * SIZE. 128 x 322 world units, a flat 128 x 322 whatever it is standing on —
 * the body is NO LONGER sized off its plinth. It was (the plinth's own 184
 * width, and 2.5x that tall) and it read as far too big; the plinth now fixes
 * only where the statue STARTS. The attack frame keeps the art's own square
 * aspect at the full height, so it flares to 322 wide; the HIT BOX does not
 * follow it (see living_statue_body). Full numbers at LST_HALF_W, including
 * what shrinking it cost in visibility over the hedges.
 *
 * TEXTURES. Two, each owning its VRAM slot outright — a plain startup LoadImage
 * apiece and no re-upload on a transition. They are 64x96 and 128x96 rather
 * than the 128x128 the older sprite enemies use, because the only VRAM left is
 * the 96-row band under the HUD (tools/VRAM_MAP.txt); both sit at Voff 128, so
 * every quad brackets its own texture window. The quad's world size is set here
 * and not by the art, so the 96 rows cost resolution and nothing else.
 *
 * PERSISTENCE is the spiders' and mushrooms' model: ONE global array tagged by
 * area, with every update/draw/weapon loop skipping any instance whose area is
 * not current_area — current_area and NEVER game_state, or the enemy freezes
 * for as long as the inventory menu is open (tools/ADDING_AN_ENEMY.txt STEP 6).
 * MAX_LIVING_STATUES is the budget for the WHOLE GAME, not per room.
 *
 * SOUND. SFX_RUMBLE, on the teleport and on the death, and nothing else — a
 * living statue is otherwise completely silent, which is most of what makes it
 * work. It is GARDEN bank only (it does not fit the house bank's 6.6 KB of
 * spare); one placed inside the house would move and die without a sound. The
 * axe hit borrows the resident SFX_AXEHIT like every other enemy's.
 * ----------------------------------------------------------------------- */

#define MAX_LIVING_STATUES    4
#define LST_MAX_HEALTH        2    /* two crucifaxe swings, in ATTACK only     */

/* ---- Ranges.
   LST_ACTIVATE_RADIUS is half a Rafflesia's engagement range — that flower
   works off RAF_PULL_RADIUS, 700 — and is TRUE RADIAL, so 350 means 350
   whichever way the player walks in from. Arming is one-way: once a statue has
   seen the player this close it is armed for the rest of the visit.

   LST_STALK_GATE is three times it. Read the note above about re-freezing
   before retuning either: the gate and the stage ranges interact, and the only
   stage range OUTSIDE the gate is the first one. */
#define LST_ACTIVATE_RADIUS 350
#define LST_STALK_GATE     1050    /* 3 x LST_ACTIVATE_RADIUS                  */
#define LST_REFREEZE          1    /* 0 = once started, the sequence completes */

/* ---- The floor grid. Maze Two's mesh is a 200-unit quad grid — 668 floor
   polys at y=0, 316 of them exactly 200x200 — so "within N polys" is N x 200.
   Measured, not assumed; re-measure if this enemy is ever placed in a room
   whose floor is tessellated differently. */
#define LST_POLY            200
#define LST_TP_INTERVAL     120    /* 2 s at 60 fps between teleports          */
#define LST_TP_STAGES         4
/* Stage ranges, in polys: 6, 4, 2, then 1 for the fourth and every one after.
   Kept as a table rather than a formula because the design states them. */
#define LST_TP_R0  (6 * LST_POLY)
#define LST_TP_R1  (4 * LST_POLY)
#define LST_TP_R2  (2 * LST_POLY)
#define LST_TP_R3  (1 * LST_POLY)

/* The landing search. TP_TRIES bearings, each walked inward from the stage's
   range in TP_STEP increments down to TP_MIN — which is the closest it will
   ever place itself, and is sized so the body never lands inside the player.
   TP_REAR is how far round from dead-behind a bearing may stray, in the 4096-
   per-turn units isin/icos take: 1024 is 90 degrees, i.e. the whole rear
   hemisphere. */
#define LST_TP_TRIES         24
#define LST_TP_STEP         100
#define LST_TP_MIN          180
#define LST_TP_REAR        1024

/* ---- Contact damage: a FIFTH of MAX_HEALTH (100) per second, i.e. 1 point
   every 3 frames. Reach is one floor poly, matching the range the last teleport
   lands inside.

   >>> THIS ONE IS RADIAL, NOT THE MANHATTAN SUM EVERY OTHER ENEMY'S CONTACT
       TEST USES. <<< It has to be, and the arithmetic is worth keeping because
   it is mistake 9 in tools/ADDING_AN_ENEMY.txt wearing a different hat — a
   reach whose floor is set not by the room but by the enemy's OWN body. The
   statue's cylinder holds the player at LST_BODY_RADIUS + 75 = 175 from its
   centre, and 175 RADIAL is anywhere from 175 to 247 MANHATTAN depending on the
   bearing. A Manhattan test against 200 would therefore have been unreachable
   on the diagonals: the statue would strike from the four compass directions
   and stand harmlessly against a player approaching it cornerwise, which reads
   as an intermittently broken enemy rather than as a rule. Radial 175 against
   200 clears from every bearing, with 25 units to spare.
   LST_TP_MIN is likewise held just OUTSIDE that hold distance, so a statue
   materialising at its closest legal spot does not shove the player on arrival.

   >>> AND THE REACH MUST CLEAR THE HOLD DISTANCE, NOT THE TELEPORT RANGE. <<<
   These were the same number (200, one floor poly) while the body was collided
   like furniture at 75. Giving it the WALL radius instead moved the hold out to
   265 and, for one build, left the statue unable to touch anybody: it walked
   up, its own cylinder stopped the player 265 away, and a 200 reach could not
   span the gap. That is mistake 9 in tools/ADDING_AN_ENEMY.txt with the floor
   set by the enemy's own body rather than by the room, and it is why the two
   ideas now have two constants and a _Static_assert between them:

     LST_HOLD_DIST   where its own solidity parks the player. Derived, never
                     typed twice — it moves the instant either half moves.
     LST_CATCH_DIST  how far it can strike. Must be MORE than the hold, or the
                     enemy is inert. 35 units of margin.
     LST_TP_R3       one floor poly. Unrelated to either: it is where the last
                     teleport puts it, not where it can hit from. */
#define LST_DAMAGE_AMOUNT     1
#define LST_DAMAGE_TICK       3

/* ---- Sprite. 128 x 322 world units.
   This USED to be sized off the plinth — 184 x 460, the plinth's own width and
   two and a half times it — and that tie is deliberately gone: the body is now
   just under 70% of those figures and the only thing the plinth still fixes is
   where the statue STARTS. Keep the two in the same ratio if either moves; the
   art is drawn to it.

   >>> AT THIS SIZE THE HEAD NO LONGER CLEARS THE HEDGES. <<< Feet on the
   plinth's top face at mesh y -131 puts the crown at -453, and Maze Two's hedge
   runs are 500 tall (-500). It is 47 units short, where at the old size it
   stood 91 proud of them. So the statue can no longer be spotted over the maze
   from a distance — it is found by walking into its alcove. Raising
   LST_HALF_H to 185 (a ~20% reduction rather than 30%) is what would put it
   back over the hedge line, at the cost of the size ask.

   LST_Y_OFFSET + LST_HALF_H == 150 is the invariant every sprite enemy here
   keeps — that sum is the drop from the entity anchor to the feet, so a
   half-height this large has to come back out of the offset (it goes NEGATIVE
   here, which is fine and is what keeps the feet on the ground) or the body
   floats. Because the invariant holds, the plinth anchor did NOT move when the
   body shrank: it is still -281, and the feet are still at -131. */
#define LST_HALF_W           64
#define LST_HALF_H          161
#define LST_Y_OFFSET        (-11)
/* The attack frame's half-width. living_statue_atk.png is square where the idle
   art is 1:2, and the design calls for the statue to flare to its full height
   in width when it strikes rather than for the square art to be squashed. The
   flare is about the quad's CENTRE, so it changes nothing about where the body
   stands. */
#define LST_ATK_HALF_W      161

/* ---- The pursuit. Once it is in ATTACK it stops teleporting and WALKS, at
   exactly the player's own walk speed (camera.c: 12 walking, 20 sprinting), for
   as long as it or the player is alive. Two things follow from picking the walk
   speed rather than something between the two, and both are intended:
     - walking away from it never opens a gap, and never closes one either. It
       sits behind you at a fixed distance, which is a worse feeling than being
       outrun;
     - sprinting DOES escape it. That is the only escape, it costs stamina, and
       it does not un-attack the statue — it just walks after you again.
   It keeps the feeler steering rather than charging in a straight line, or it
   would grind into the first hedge corner between it and the player. */
#define LST_CHASE_SPEED      12
#define LST_FEELER_LEN      170
#define LST_TURN_RATE         3
#define LST_STEER_COMMIT     30

/* Solid body. The player must never be able to walk through a statue, so the
   standoff is deliberately WALL-LIKE: apply_collision_reception passes the
   room's own 195 wall radius rather than the 75 a prop gets, and 70 + 195 is a
   265 stop distance. In Maze Two's 600-wide lanes that makes a statue standing
   near the centreline a genuine roadblock, which is the point of "never walk
   through it".
   70 is over the sprite's own 64 half-width, so the billboard still projects
   whole at the stop distance.

   >>> 265 IS PAST WHAT THE CRUCIFAXE'S CENTRE TEST COULD REACH. <<< Every other
   melee block in crucifaxe.c measures Manhattan dist3d to the enemy's CENTRE
   against SWING_RANGE (350), and 265 radial is up to 375 Manhattan on the
   diagonals — the axe would simply never connect, which presents as "the statue
   is invulnerable". living_statues_try_hit therefore measures to the body's
   SURFACE instead, exactly as tools/ADDING_A_3D_ENEMY.txt STEP 7 prescribes and
   as tentacles_try_hit already does. Both it and the push-out read the same
   LST_BODY_RADIUS, so the two can never disagree about where the edge is. */
#define LST_BODY_RADIUS      70
#define LST_WALL_LIKE_PUSH  195   /* what apply_collision_reception passes in */

#define LST_HOLD_DIST       (LST_BODY_RADIUS + LST_WALL_LIKE_PUSH)   /* 265 */
#define LST_CATCH_DIST      300
_Static_assert(LST_CATCH_DIST > LST_HOLD_DIST,
               "a Living Statue's reach must clear its own push-out, or its "
               "solid body holds the player outside the range it can strike "
               "from and it can never land a hit");

#define LST_BAR_TIMER_MAX   120

/* Stone chips, not blood. The shared burst with the colour swapped for grey —
   see living_statue_damage. */
#define LST_BLOOD_R         150
#define LST_BLOOD_G         150
#define LST_BLOOD_B         155

typedef enum {
    LST_IDLE,     /* inert: never armed, or authored as plain masonry  */
    LST_STALK,    /* armed; teleporting closer under the two gates     */
    LST_ATTACK,   /* within one poly, striking                         */
    LST_DEAD,
} LivingStatueState;

typedef struct {
    int32_t           x, y, z;      /* y is the STANDING ANCHOR, feet at y+150 */
    int32_t           spawn_x, spawn_y, spawn_z;

    int               health;
    int               damage_timer;
    int               hit_timer;
    int               tp_timer;     /* frames left on the teleport countdown   */
    int               tp_stage;     /* how many teleports it has made          */

    /* The ATTACK walk. `facing` is the last travel step, packed hi16 X / lo16 Z,
       the way the mushroom and spider carry theirs; the steer timer/dir are the
       wall-follow commit. All three are meaningless until it reaches ATTACK. */
    int32_t           facing;
    int               steer_timer;
    int               steer_dir;
    /* Gravity/floor state, used only while it is walking in ATTACK. A stalking
       statue is never floor-probed at all — it has to be able to stand on a
       plinth, and apply_ddog_height would drag it off. */
    int32_t           vy;
    int               on_upper_floor;
    int               on_ramp;

    int               living;       /* 0 = ordinary masonry, forever           */
    LivingStatueState state;
    int32_t           active;
    GameState         area;
} LivingStatue;

extern LivingStatue living_statues[MAX_LIVING_STATUES];
extern int          living_statue_count;

/* Load the two sprite TIMs into VRAM. Call ONCE at startup — a CdRead is only
   safe before the per-frame render loop begins (tools/TEXTURING_NOTES.txt).
   The slots are this enemy's own, so there is no upload-on-entry counterpart. */
void living_statues_load_textures(void);

/* Place one standing at (x, z) with its FEET at anchor y + 150. `living` = 0
   makes it permanent scenery: solid and drawn, but never armed and never
   damageable. Everything is AUTHORED — nothing here reads the current room's
   mesh — so this is safe to call for a room that is not loaded, which is what
   lets world.c rebuild every visited room from a save delta. Returns its index,
   or -1 if the global pool is full. */
int  living_statue_add(int32_t x, int32_t z, int32_t y, int living,
                       GameState area);

void living_statues_init(void);
void living_statues_reset(void);
/* Put every still-living statue back on its plinth, unarmed, at full health
   (deaths stick). Called when leaving a room and when saving. */
void living_statues_rest(void);
void update_living_statues(void);
void draw_living_statues(RenderContext *ctx);

/* Deal damage. Refused unless the statue is in ATTACK — the crucifaxe already
   skips the others, so this is the second half of the same rule and the place
   to relax it when the weapon that CAN break a stalking statue arrives. */
void living_statue_damage(LivingStatue *s, int dmg);

/* Scale a hit by this enemy's weaknesses — there are none yet, by design. */
int32_t living_statue_scale_damage(int32_t base, DamageType type);

/* Sprite-centre Y, half-height and half-width, for anything that needs the hit
   box. It is the IDLE width in every state: the attack frame's flare is a
   drawing decision and making the body twice as easy to hit while it strikes
   would be exactly backwards. */
void living_statue_body(const LivingStatue *s,
                        int32_t *cyc, int32_t *hh, int32_t *hw);

/* Solid cylinder, area-gated so the call is a no-op everywhere one is not
   placed. Called for the PLAYER from apply_collision_reception, BEFORE the wall
   passes for the reason rafflesias_collide is: Maze Two's statue stands on a
   plinth hard against a hedge, so its push circle overlaps solid geometry and
   pushing it last would wedge the player into the greenery. */
void living_statues_collide(int32_t *px, int32_t py, int32_t *pz,
                            int32_t radius);

/* Crucifaxe strike: damage the first in-reach statue in front of the player,
   and return 1 if one was hit. Lives here rather than inline in crucifaxe.c —
   the way tentacles_try_hit and rafflesias_try_hit do — because the reach has
   to be measured to this body's SURFACE and the geometry is the enemy's
   business. See LST_BODY_RADIUS for why the centre test cannot work.
   Statues that are not in ATTACK are skipped and do NOT count as a hit, so the
   same swing goes on to reach anything else in range. */
int  living_statues_try_hit(void);

/* Tell the renderer which texture window the current area has active, so each
   sprite can be drawn unmasked and the area's window then restored. Pass NULL
   for areas that use no texture window. */
void living_statues_set_texwindow(const RECT *tw);

#endif
