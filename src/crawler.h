#ifndef CRAWLER_H
#define CRAWLER_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState — each crawler is tagged with its area */

/* -----------------------------------------------------------------------
 * Crawler — Chapter 3's monster, and the game's first SURFACE-CRAWLING enemy.
 *
 * Built on the spider (src/spider.c, itself built on the zombie): a textured
 * quad, an IDLE -> active wake, feeler-based steering, contact damage, a health
 * bar, fog, a shared walk loop and the Voff>=128 texture-window bracket. Read
 * spider.h first; only the differences are written out here.
 *
 * WHAT IT ADDS
 *
 *   1. IT LIVES ON A SURFACE, not just on the floor. `surface` is FLOOR, WALL
 *      or CEILING, and the sprite is built in that surface's plane so its legs
 *      are always against whatever it is standing on (see draw_crawlers). A
 *      crawler that runs into level geometry MOUNTS it, and what it does next
 *      depends on which way it is going (see CrawlerWallMode):
 *
 *        HUNTING, it goes ROUND — slides along the face and drops off the far
 *        end, which is "if the player is round a corner it climbs the wall and
 *        follows it round": the XZ path is the zombie's wall-follow, drawn
 *        where the creature really is.
 *
 *        RUNNING AWAY, it goes OVER — straight up the face and onto whatever
 *        the top belongs to, a walkway or the ceiling, and on along the same
 *        line it started on. A wall is not an obstacle to a retreat, it is the
 *        part of the retreat that happens to be vertical.
 *
 *   2. IT IS A HIT-AND-RUN FIGHTER, not a chaser and not a kiter. The loop is
 *      RUSH -> (hit the player, or get hit) -> RETREAT the room's unlit fog
 *      distance -> PAUSE, scream -> RUSH again, and it runs until one of them is
 *      dead. That is a different shape from the spider's three concentric bands:
 *      the crawler's distance to the player does not choose its behaviour, its
 *      STATE does, and the state only changes when a blow lands.
 *
 *      THE RETREAT IS A STRAIGHT LINE IN THREE DIMENSIONS. One direction,
 *      latched the frame the blow lands and never re-aimed; no separation, no
 *      left/right sidestep, no sightline. Climbing is the only turn it is
 *      allowed to make, and when even that runs out the stall watch ends the
 *      retreat rather than leaving it grinding in a corner (CRW_STALL_FRAMES).
 *
 *   3. ITS WAKE RADIUS LOOKS DOWN BUT NOT UP. See CRW_WAKE_RADIUS.
 *
 * PERSISTENCE is the spiders' model B: ONE global array tagged by area, with
 * every update/draw/weapon loop skipping any crawler whose area != current_area
 * — current_area and never game_state, so the player cannot pause a fight by
 * opening the inventory. See the note in spider.h and STEP 6 of
 * tools/ADDING_AN_ENEMY.txt.
 *
 * MAX_CRAWLERS is the budget for the WHOLE GAME, not per room: five in the Up
 * Down Maze's lower level today, and crawler_add_* returns -1 and places
 * nothing once the pool is full.
 * ----------------------------------------------------------------------- */

#define MAX_CRAWLERS          6    /* 5 placed in the Up Down Maze + 1 spare */

#define CRW_MAX_HEALTH        6    /* six crucifaxe swings or six rounds      */

/* SPEED. "Faster than the demon dogs", which run at DDOG_SPEED 10; the zombie
   and the spider are both 8. 14 is a real gap rather than a nominal one — it
   crosses one 600-wide maze cell in about 43 frames — and it is what makes the
   rush unrunnable-from and the retreat quick enough not to be dead time. */
#define CRW_SPEED            14
#define CRW_RETREAT_SPEED    14    /* it BOLTS away, it does not back off      */

/* ---- The two radii, and the scale they come from -------------------------
   The brief is 6 m, and this game has never written down a metres-per-unit. The
   one modelled real-world object with a known size is the fat door, 375 units
   tall for a ~2 m doorway, so ~190 units to the metre: 6 m -> 1100. For scale
   sense in this room specifically, the lower maze's corridors are 600 wide, so
   a crawler notices the player about two cells away, just inside the 1600 the
   unlit fog lets them see (UDM_BASE_FOG_FAR).

   >>> THE WAKE TEST LOOKS DOWN BUT NOT UP, AND IT IS A CYLINDER, NOT A SPHERE.
   <<< The brief's example is a crawler on the CEILING waking as the player
   passes underneath, and a ceiling in this room is 1000-1800 above the floor —
   more than the radius — so a true 3D sphere could never fire on exactly the
   case it was written for. What is meant is a radius in plan view that extends
   DOWNWARD without limit and not at all upward: XZ radial distance within
   CRW_WAKE_RADIUS, and the player not on a level ABOVE the crawler. A player on
   a walkway above a crawler in the corridor below walks past unnoticed; the
   same player down in the corridor wakes it from any height above them. */
#define CRW_WAKE_RADIUS    1100    /* true radial XZ distance; see above       */

/* >>> "NOT ABOVE" NEEDS A TOLERANCE, AND LEAVING IT OUT MAKES THE ENEMY INERT.
   <<< The obvious test — wake only if the player's Y is at or below the
   crawler's — is wrong by 40 units and never fires at all. apply_height() rests
   the PLAYER's eye 40 above the floor surface it stands on (the standoff at the
   end of that function, player-only); apply_ddog_height, which every enemy
   uses, has no such standoff. So a player sharing a floor with a crawler is
   permanently 40 units ABOVE its anchor and a strict test reads that as
   "overhead" on every frame of the game. It presents as a crawler that stands
   still forever while still taking damage and still screaming when killed —
   i.e. as the updater not running, which is a different bug with the same face.

   The tolerance is the crawler's own body height above its anchor, which is the
   sprite's top edge: Y_OFFSET - HALF_H below the anchor in a -Y-up space, so
   HALF_H - Y_OFFSET above it. That reads as "the player is level with me if
   their eye is no higher than the top of my body", and it scales with the
   sprite instead of being a magic 50. The real separation it has to reject is a
   whole storey — 1000 units in the Up Down Maze — so there is no ambiguity
   between the two cases. */
#define CRW_WAKE_ABOVE     (CRW_HALF_H - CRW_Y_OFFSET)   /* 290 */

/* The listening radius, 50% further out, and this one DOES look up: it is the
   only cue an idle crawler gives, and hearing one from the walkway above is the
   point. Note it is fractionally past the unlit fog line at 1600, so the
   whisper reaches the player from somewhere they cannot yet see. */
#define CRW_WHISPER_RADIUS 1650
#define CRW_WHISPER_INTERVAL 240   /* frames between re-triggers, ~4 s. The clip
                                      is 1.82 s, so there is a real gap of
                                      silence between one whisper and the next */

/* ---- The retreat ----------------------------------------------------------
   "Far enough back that it goes into the darkness", i.e. the room's unlit fog
   distance, measured with the HELLUMINATOR PUT AWAY: UDM_BASE_FOG_FAR in the
   Up Down Maze (src/up_down_maze.c) and CE_BASE_FOG_FAR in the entry, both
   1600.

   A CONSTANT AND NOT THE ROOM'S LIVE fog_far, deliberately: the lantern scales
   that value by up to 2x, and a crawler that retreated to the lit distance
   would go further the brighter it got — exactly the adaptive behaviour the
   brief rules out. Raising the lantern is supposed to REVEAL a retreated
   crawler, not push it back out of sight.

   >>> IT IS A DISTANCE TRAVELLED, NOT A DISTANCE FROM THE PLAYER. <<< The old
   retreat ended when the GAP reached this figure, which a maze cannot always
   grant: a crawler that backed into a dead end, or that the player walked in
   on, could never open the gap however far it ran, so it ground where it stood
   until an eight-second timeout let it go. The timeout is gone. A retreat now
   spends this much PATH — all three axes, so the vertical leg up a wall counts
   the same as a run down a corridor — and a crawler that has to go up and over
   something to spend it is doing the retreat, not failing it. It always ends,
   and it always ends after the same amount of running. */
#define CRW_RETREAT_DIST   1600

/* ---- The stall watch ------------------------------------------------------
   >>> A DISTANCE TRAVELLED STILL NEEDS THIS, AND IT IS NOW THE ONLY OTHER WAY
   OUT. <<< A budget spent out of travel is only spent by a body that travels:
   one wedged somewhere it can neither run from nor climb out of would hold its
   remaining distance forever, and with the timeout gone there would be nothing
   behind it at all. Such a crawler has finished retreating the moment it stops
   moving, and three quarters of a second of nothing is the most the player
   should ever watch.

   So the retreat also ends when the body has genuinely stopped: less than
   CRW_STALL_MIN of travel in a frame — a third of one frame's worth, so a
   crawler grinding along a face at a shallow angle still counts as moving — on
   CRW_STALL_FRAMES consecutive frames. Three quarters of a second is long
   enough that nothing a moving crawler does trips it, and short enough that a
   wedged one comes back at the player while the exchange is still live. */
#define CRW_STALL_FRAMES     45
#define CRW_STALL_MIN         5

/* The beat at the far end of the retreat, before it comes back: one second,
   long enough that a player following a retreat with the Helluminator up
   actually SEES it stop rather than catching a turn. The scream fires on the
   frame this starts, and the clip (1.90 s) runs on well into the rush — which
   is the intent: you hear it coming. */
#define CRW_PAUSE_FRAMES     60

/* ---- Contact damage -------------------------------------------------------
   20% of the player's maximum, i.e. five hits from full. Written as the literal
   20 rather than (MAX_HEALTH / 5) because player.h is not included here and the
   number is a design constant, not a derived one — but it IS 20% of MAX_HEALTH
   (player.h) and a static assert in crawler.c keeps the two honest. */
#define CRW_DAMAGE_AMOUNT    20
#define CRW_CATCH_DIST      180    /* Manhattan reach, as the spider's bite    */
#define CRW_BAR_TIMER_MAX   120

/* Frames per animation frame while moving. Four frames at 6 gives a full cycle
   in 24 frames, a fast scuttle to match CRW_SPEED. An idle or paused crawler
   holds frame 0 and does not tick this at all. */
#define CRW_ANIM_RATE         6
#define CRW_ANIM_FRAMES       4

/* ---- Sprite ---------------------------------------------------------------
   Square, because the art is: one 256x256 sheet quartered into four square
   128x128 frames (textures/catacombs/crawler.png, shipped as two 256x128
   halves — see disc.xml for why it cannot be one file). HALF_W and HALF_H are
   HALF-extents in world units, and Y_OFFSET + HALF_H == 150 is the same
   invariant every other enemy here keeps: the distance from the entity anchor
   down to the feet, so a crawler stands on the floor exactly where a zombie
   does. Growing HALF_H therefore has to come back out of Y_OFFSET, and at 220
   that makes the offset NEGATIVE — the anchor is at the creature's feet and
   the body reaches 290 above it, which is fine and is what the sum says.

   440 x 440, i.e. nearly twice the zombie's 250-tall body. It fills a good part
   of the lower maze's 600-wide corridors, which is the intent: the thing is
   supposed to blot out the passage when it comes at you.

   >>> AT THIS SIZE THE BILLBOARD MUST BE SUBDIVIDED. <<< The GPU DROPS any
   primitive whose screen extent passes 1023 pixels — it does not clip it — and
   at gte_SetGeomScreen(256) a span S reaches that at a VIEW DEPTH of
   S * 256 / 1023. A single 440 quad would vanish inside a depth of 110, and
   CRW_CATCH_DIST is 180, so it would blink out every time it closed to bite or
   the player turned past it. Mistake 13 in tools/ADDING_AN_ENEMY.txt, and the
   fix there is explicit: cut the billboard up, do not widen the collision.
   CRW_SUBDIV 2 makes each of the four pieces a 220 square whose own threshold
   is 55 — exactly where this sprite sat before it was doubled, and well inside
   the spider's shipped 280. Raise the subdivision, not the standoff, if the
   body ever grows again. */
#define CRW_HALF_W          220
#define CRW_HALF_H          220
#define CRW_Y_OFFSET        (150 - CRW_HALF_H)   /* keeps the feet on the floor */
#define CRW_SUBDIV            2    /* 2x2 grid of quads; see the note above    */

/* ---- Wall and ceiling work ------------------------------------------------
   MOUNT_DIST is how close to a wall face the crawler has to be pressed before
   it climbs, and it is deliberately a little wider than BODY_RADIUS so the
   mount happens as it arrives rather than after it has already stopped dead.

   SURF_OFFSET holds the sprite that far out from the surface plane, so the quad
   never coincides with the wall polys and z-fights them.

   CLIMB_RISE is how far up the face it settles, measured from the wall's own
   base. 380 puts the body clear of a standing player's eye line without
   reaching the top of a 1000-tall maze block, and it is clamped into the wall's
   real Y span at mount time, so a short wall simply holds it lower. */
#define CRW_MOUNT_DIST      120

/* >>> THE FIVE RULES. <<< Everything in this block exists to serve these, and
   anything that stops being able to state which one it implements is wrong:

     1. Backs into a WALL          -> retreats UP it, and over.
     2. Backs into a CORNER        -> up the FIRST face it touches.
     3. Backs into a corner on an EDGE -> the same: first face it touches.
     4. Advances into a WALL       -> up and OVER, then down on the player.
     5. Advances into a corner on an EDGE -> follows the face nearer the player
                                      to get round it.

   Read together they say: a wall is something to GO OVER, and following one
   round is the exception, not the rule — it is what an EDGE gets, because at an
   edge there is a way round and going over would be the long way. And "the
   first face it touches" is an instruction not to deliberate: at a corner two
   faces are equally good and any tie-break that re-runs every frame will flip
   between them and travel nowhere.

   >>> AND MOUNT_DIST ALONE IS WHY THE CRAWLERS NEVER CLIMBED ANYTHING. <<< The
   obstacle feeler reaches CRW_FEELER_LEN ahead of a body already held
   CRW_BODY_RADIUS off every face, so the frame `blocked` first goes true the
   crawler is ~280 from the wall — more than twice MOUNT_DIST. The mount
   therefore failed, and the ordinary wall-follow committed CRW_STEER_COMMIT
   frames PERPENDICULAR TO THE GOAL, which for a head-on approach is parallel to
   the wall: the body never closed, and the loop repeated for as long as the
   wall was there.

   Walking it in instead is no better and is what "moves to the corner really
   slowly before going up" looked like: a crawler converging on one face is
   being pushed along by every other face it is touching, so it closes the last
   190 units at whatever fraction of its speed points that way, which in a
   corner is almost none of it.

   SO IT TAKES HOLD THE MOMENT IT CAN SEE THE FACE, at up to CRW_MOUNT_REACH,
   and the body's real distance off the face becomes wall_off — eased down to
   CRW_SURF_OFFSET at CRW_OFFSET_EASE a frame while the climb is already under
   way. The decision is instant, and the approach is still drawn. */
#define CRW_MOUNT_REACH     (CRW_FEELER_LEN + CRW_BODY_RADIUS)   /* 280 */
#define CRW_OFFSET_EASE      32    /* units a frame the body settles onto the face */

/* How square-on the approach has to be before a face counts as a wall to climb
   rather than one to brush past: the goal's component into the face, at least a
   third of the goal's true length (about 70 degrees). Without it a crawler
   running the length of a corridor would mount the side it happens to be
   nearest, because "heading into it at all" is true of almost every diagonal.

   >>> THE LENGTH IT IS COMPARED AGAINST HAS TO BE A REAL ONE. <<< Measured
   against the MANHATTAN magnitude the fraction holds only for a goal running
   along an axis; at 45 degrees Manhattan is 1.41x the true length, so the same
   constant silently demanded half again as much. A crawler heading into a
   CORNER is diagonal by definition and is the exact case that throws away — it
   arrives, fails on BOTH of the two faces it is wedged between, and sits on the
   ground. Rules 2 and 3, lost to a missing square root. */
#define CRW_MOUNT_HEADON      3    /* 1/3 — into * HEADON >= |goal| */

/* Rule 5's "on an edge": the contact point is within this of either end of the
   face. An edge has a way round it and going over would be the long way; a face
   the crawler meets in the middle has no way round worth taking. */
#define CRW_WALL_EDGE       200

/* ---- Corners, and committing to a face ------------------------------------
   >>> A CORNER IS TWO EQUALLY GOOD ANSWERS, AND RE-ASKING EVERY FRAME IS HOW AN
   ENEMY GETS STUCK IN ONE. <<< Rules 2, 3 and 5 all say "the first one", and
   that is a rule about NOT DELIBERATING. Pressed into a corner a crawler is the
   same distance off both faces and heading into both, so any score computed
   fresh each frame flips between them on rounding alone and each flip throws
   away what the last one earned. So:

     - THE NEAREST FACE WINS, and ties go to the lower index. That is "the first
       one it comes into contact with", and it is stable: the same body in the
       same corner gets the same answer every frame, which is the property that
       actually matters — more than which of the two it picks.

     - IT STAYS ON WHAT IT PICKED for CRW_WALL_HOLD frames before the sightline
       may take it off again: "moves along that wall for a second before it
       thinks about another one". Running off the END of a face is not covered —
       that is geometry, not a decision.

     - IT DOES NOT GO STRAIGHT BACK ON THE FACE IT JUST LEFT, for
       CRW_WALL_COOL frames, so a crawler that has slid to the end of one face
       at a corner takes THE OTHER ONE and carries on round instead of
       re-grabbing the first and sliding back into the same dead end. */
#define CRW_WALL_HOLD        45
#define CRW_WALL_COOL        60

/* ---- Coming back down ------------------------------------------------------
   >>> A RUSH DOES NOT STAY UPSTAIRS, AND THIS IS WHAT "WENT UP INTO THE DARK
   AND NEVER CAME BACK" WAS. <<< A crawler that finished a retreat on a walkway
   or under the roof is a body that has to get to a player a whole storey below
   it. Walking at them does it while the two are apart — it reaches the lip and
   falls — but once it is ABOVE them the XZ goal is satisfied where it stands
   and nothing ever moves it again: it cannot bite (dy is 1000, far outside
   CRW_CATCH_DIST), it is out of the fog, and it simply stands there. The room
   goes quiet and the fight is over without either of them deciding it.

   So being overhead becomes its own instruction. Within CRW_ABOVE_DIST in plan
   view and more than CRW_UPPER_DROP_DY above the player, a rushing crawler on a
   raised floor heads for the NEAREST EDGE of that floor and walks off it, and
   one on the ceiling simply lets go. Rule 4's "dropping down towards the
   player", arrived at from the other direction. */
#define CRW_UPPER_DROP_DY   500    /* the player is a storey down, not a step  */
#define CRW_ABOVE_DIST      700    /* near enough overhead to stop closing     */

/* A rush has no odometer to run down and nothing else behind it, so it needs
   a stall watch of its own or any wedge in it is permanent. Longer than the
   retreat's, because a rush that is briefly held up against a prop is ordinary
   and should not be interfered with. */
#define CRW_RUSH_STALL      90
#define CRW_SURF_OFFSET      24
#define CRW_CLIMB_RISE      380
#define CRW_CLIMB_SPEED       8    /* units of Y a frame while settling        */
/* Going UP a face on purpose — the retreat's vertical leg — runs at the speed
   the rest of the retreat does, because the whole point is that it is ONE
   straight line that happens to turn a corner onto a vertical surface. The 8
   above is a settle, not a climb: it is what the rush's mount eases through
   while it is already sliding sideways. */
#define CRW_SCALE_SPEED      14
#define CRW_DESCEND_SPEED    24    /* coming back DOWN a face is a controlled fall */

/* Crossing the top of a face onto whatever is up there. The landing has to be
   REAL: a floor zone at the wall's own top height, or a ceiling the face
   reaches. A crawler that stepped over a lip with nothing behind it would land
   outside every floor zone, and apply_ddog_height's `target` defaults to 0 —
   which is GROUND_FLOOR_Y BELOW the floor surface, i.e. buried in it, and out
   of reach of collide_wall_frontonly forever after (it is behind every face it
   could be pushed off). Probe before committing, never after. */
#define CRW_CROSS_TOL       200    /* floor zone Y vs the wall top             */
#define CRW_CEIL_TOL        160    /* how near the roof a face has to stop     */

/* The drop off a ceiling, seeded at terminal velocity so it falls fast from the
   first frame — the spider's SPD_DROP_VEL and for its reason. */
#define CRW_DROP_VEL         20

/* Steering, the zombie's and the spider's, retuned only where the speed made it
   necessary: the feeler has to reach further ahead than one frame's travel or a
   14-a-frame crawler arrives at the wall before the feeler notices it. */
#define CRW_SEP_RADIUS      180
#define CRW_SEP_WEIGHT        2
#define CRW_BODY_RADIUS      90
#define CRW_DOOR_CLEARANCE  100
#define CRW_FEELER_LEN      190
#define CRW_TURN_RATE         4    /* snappier than the spider's 3: it charges */
#define CRW_STEER_COMMIT     30

/* The floor shadow is the body's footprint, so it doubled with the body. It is
   fogged and culled on exactly the same curve as the sprite above it — a
   shadow that stayed sharp while the crawler faded into the dark was the tell
   that gave a retreated one away. */
#define CRW_SHADOW_W        260
#define CRW_SHADOW_D        110

typedef enum {
    CRW_SURF_FLOOR = 0,
    CRW_SURF_WALL,
    CRW_SURF_CEILING,
} CrawlerSurf;

/* What a crawler is DOING on the face it has mounted. The rush and the retreat
   want opposite things from a wall and always did: the rush wants to get ROUND
   it (slide along the face and drop off the far end), the retreat wants to get
   OVER it (straight up and onto the walkway or the roof, because the retreat is
   a straight line in three dimensions and a wall is just where that line turns
   vertical). */
typedef enum {
    CRW_WALL_FOLLOW = 0,  /* slide along the face — the rush's corner-turn    */
    CRW_WALL_CLIMB,       /* straight up, as far as the sprite may go         */
    CRW_WALL_OVER,        /* the last stretch, with a landing already probed  */
    CRW_WALL_DESCEND,     /* back down the face, then step off it             */
} CrawlerWallMode;

typedef enum {
    CRW_IDLE,       /* on its spawn surface, frame 0, silent but for the whisper */
    CRW_DROPPING,   /* woken off a ceiling, falling to the floor                 */
    CRW_RUSH,       /* closing on the player                                     */
    CRW_RETREAT,    /* bolting back out past CRW_RETREAT_DIST                    */
    CRW_PAUSE,      /* fully retreated: screams, then rushes again               */
    CRW_DEAD,
} CrawlerState;

typedef struct {
    int32_t      x, y, z;
    int32_t      spawn_x, spawn_y, spawn_z;
    CrawlerSurf  spawn_surface;
    int32_t      vy;
    int          health;
    int          hit_timer;
    int          damage_timer;
    CrawlerState state;
    CrawlerSurf  surface;
    /* While CRW_SURF_WALL: the index into current_collision_room.walls, how far
       along that segment the body sits, and the segment's cached length and
       inward normal. The INDEX is only meaningful while this room's collision
       data is loaded, which is why no placement ever authors a wall spawn —
       the wall list is GENERATED and its indices shuffle on every re-export.
       A crawler only ever mounts a wall during play, and crawlers_rest() puts
       it back on its authored surface when the player leaves. */
    int          wall;
    int32_t      wall_t, wall_len;
    int32_t      wall_nx, wall_nz;
    int32_t      climb_y;        /* the Y it is settling to on that face       */
    int16_t      wall_mode;      /* CrawlerWallMode, while CRW_SURF_WALL       */
    int16_t      wall_off;       /* the body's real distance off the face      */
    int16_t      wall_hold;      /* frames left of the commit to this face     */
    int16_t      last_wall;      /* the face it most recently let go of        */
    int16_t      wall_cool;      /* frames that face stays off the candidates  */
    /* The retreat's LATCHED direction, Manhattan-normalised to 4096, fixed the
       frame the retreat starts and never re-aimed. "It retreats in a STRAIGHT
       LINE": re-deriving it from the player every frame is what made the old
       retreat a mirror-image chase that curved as the player moved. */
    int16_t      ret_x, ret_z;
    int16_t      stall_timer;    /* frames of no travel while retreating       */
    int32_t      active;
    int          on_upper_floor;
    int          on_ramp;
    int          anim_tick;
    int          moved;          /* travelled this frame: drives anim + scuttle */
    int          pause_timer;
    int          retreat_left;   /* units of path the retreat still owes      */
    int32_t      facing;         /* last move dir, packed: hi16 = X, lo16 = Z  */
    int          steer_timer;
    int          steer_dir;
    /* 1 once anything has ever woken it. crawlers_rest() puts a crawler back on
       its spawn but leaves this set, which is what makes "leave the room while
       it is active and it is STILL active when you come back" true. */
    int          roused;
    GameState    area;
} Crawler;

extern Crawler crawlers[MAX_CRAWLERS];
extern int     crawler_count;

/* Register the sprite sheet. Call ONCE at startup: it takes the TIM HEADER only
   (TEXBANK_CATACOMBS), and the pixels arrive when that bank is selected. */
void crawlers_load_textures(void);
/* Re-stream the sheet into its VRAM slot on a room transition. That slot is
   time-shared with the spiders' and the Rafflesia's first sprite (x320 y128),
   so main.c uploads exactly one of the three on every room entry. */
void crawlers_upload_textures(void);

/* Place a crawler standing on a floor at `floor_y` under (x,z), in `area`. The
   floor height is AUTHORED, not probed, so this works for a room that is not
   loaded — which is what lets world.c rebuild every visited room from a save
   delta without its geometry resident. Returns its index, or -1 if full. */
int  crawler_add_floor(int32_t x, int32_t z, int32_t floor_y, GameState area);
/* Same, stuck to a ceiling at `ceiling_y`. It hangs by its legs until woken and
   then drops to the floor, which is the one thing the brief says a surface
   changes about the attack. Nothing places one yet. */
int  crawler_add_ceiling(int32_t x, int32_t z, int32_t ceiling_y, GameState area);

void crawlers_init(void);
void crawlers_reset(void);
/* Put every living crawler back on its spawn, at full health, ON ITS SPAWN
   SURFACE — but AWAKE if it had ever been woken. Called when leaving a room and
   when saving. Deaths stick. */
void crawlers_rest(void);
/* Stop the shared scuttle loop and clear its latch (room transitions). */
void crawlers_silence(void);
void update_crawlers(void);
void draw_crawlers(RenderContext *ctx);

/* Deal damage from any source. Wakes a sleeping crawler, and turns an advancing
   one round into its retreat — which is the brief's "if the player manages to
   damage it while it is advancing, it retreats". Flashes the health bar and
   handles death. Both the crucifaxe and a Grave-olver round pass 1. */
void crawler_damage(Crawler *s, int dmg);

/* One crucifaxe swing against the crawlers: reach, facing and one-hit-per-swing
   all live here, the way the tentacle's and the Rafflesia's do, because a
   surface-crawling body is not at the height crucifaxe.c's inline blocks
   assume. Returns 1 if a swing connected. */
int  crawlers_try_hit(void);

/* Scale a hit by this enemy's weaknesses (see damage.h). 2x to DMG_HOLY. */
int32_t crawler_scale_damage(int32_t base, DamageType type);

/* Tell the crawler renderer which texture window the current area has active,
   so each sprite can be drawn unmasked and the area's window restored. Pass
   NULL for areas that use no texture window. */
void crawlers_set_texwindow(const RECT *tw);

#endif
