#ifndef GOLEM_H
#define GOLEM_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"         /* GameState - each Golem is tagged with its area */
#include "crucifaxe.h"     /* SWING_RANGE - the axe test is measured from it  */

/* -----------------------------------------------------------------------
 * GOLEM - a squat giant of cracked rock and lava, and the Underworld's third
 * enemy. It NEVER MOVES.
 *
 * >>> WHERE IT STANDS MAY DEPEND ON THE ROUTE IN, OR ON A FLAG. <<< A Golem is
 * planted, not placed-and-left: the design allows one to stand somewhere
 * different depending on which door the player came through, or on a game
 * flag. WINGSCAPE'S DOES (Nov 2026): it stands in front of the door furthest
 * from the way in - world_place_wingscape_golem() in world.c, keyed on the
 * wingscape_from_plinth latch in main.c. The user says when one should move;
 * never invent a rule. The pattern:
 *   - the rule goes in world.c beside the placement, and golem_set_spawn()
 *     re-plants it (on room entry, AFTER world_enter - golems_rest has already
 *     put it back at the old spawn by then);
 *   - "which door" is a per-room arrival latch the room's own module keeps
 *     (sharur_arrival in main.c is the pattern), NEVER current_area - that does
 *     not survive a title load or a debug jump;
 *   - the save keys deaths by canonical_index, not by position, so moving a
 *     living Golem costs the save nothing.
 *
 * >>> EVERY GOLEM MAY BLOCK SOMETHING. <<< (the user's rule, Nov 2026) A Golem
 * carries a `blocks` tag (GolemBlock, below): the one thing in its room the
 * player cannot use while it stands - a door, a tablet. It is NOT a wall: the
 * Golem's plug already holds the player off; the tag is what the THING asks,
 * through golem_blocks(area, what), before it answers a Circle or shows its
 * prompt. A dead Golem (or an absent guard) blocks nothing, so killing it is
 * the way through. Today:
 *   - the Wings Tablet's guard blocks GLM_BLOCKS_WINGS_TABLET (tagged beside
 *     its seed in world.c);
 *   - Wingscape's door Golem blocks whichever door it is planted in front of
 *     (GLM_BLOCKS_WINGSCAPE_W_DOOR / _SE_DOOR), re-tagged beside each re-plant
 *     in world_place_wingscape_golem() - the tag moves with the Golem;
 *   - Ribcage's Golem blocks its east door, to Lightnings Edge
 *     (GLM_BLOCKS_RIBCAGE_E_DOOR, tagged beside its seed in world.c);
 *   - The Tube's Golem blocks its north door (GLM_BLOCKS_TUBE_N_DOOR, tagged
 *     beside its seed in world.c), to Destinys Cabin; the_tube.c's trigger
 *     and prompt ask.
 *   - The Tube's SECOND Golem blocks its south door (GLM_BLOCKS_TUBE_S_DOOR),
 *     to Heavens Gate. Seeded ABSENT as a guard; it only stands once the
 *     Destiny Tablet is solved (the_tube_apply_flags, FLAG_TUBE_S_GOLEM_KILLED
 *     in player.h).
 * A new Golem that guards something: a new GolemBlock value, golem_set_blocks()
 * where it is placed, and a golem_blocks() test in the thing's own trigger AND
 * its prompt. The tag survives golems_rest and every re-plant (glm_spawn keeps
 * it, as it keeps `guard`); golem_add / golem_add_guard clear it.
 *
 * THE FIGHT, in order:
 *
 * >>> AN ALERT GOLEM HAS NO IDLE. <<< (the user's rule for The Tube's, Nov
 * 2026: "alert and attacking as soon as the player enters the room".)
 * golem_set_alert() marks one; every spawn of it - the seed, golems_rest on
 * the way out, a re-plant, a revive - puts it straight into LAS_WAITING
 * instead of IDLE, so its first beam is one laser cycle (~3 s) after the
 * player walks in, and it never breathes waiting to be hit. The mark
 * survives glm_spawn's wipe like `guard` and `blocks`.
 *
 *   IDLE    Until the player DAMAGES it, with anything. It breathes - frames 1
 *           and 6 in turn, GLM_BREATH_RATE each - and does nothing else, at any
 *           range. Leaving the room before it dies puts it back here at full
 *           health (golems_rest, from world_leave); a death sticks.
 *
 *   ACTIVE  The first hit wakes it. From then on it does one of two things,
 *           and >>> A LASER CYCLE ALWAYS FINISHES <<< before it can choose
 *           again (the user's answer, Nov 2026):
 *
 *     PUNCH   Chosen whenever the player is within GLM_PUNCH_RADIUS (and on
 *             its storey) at a decision point. The Lumberer's swing, faster:
 *             frame 4 for GLM_WINDUP_FRAMES (0.75 s - it was 0.5 s, half the
 *             Lumberer's, and was raised 50% after play-testing) while the arm
 *             goes up, then frame 5 for GLM_STRIKE_FRAMES (0.5 s) - and on the
 *             first frame of the strike a DARK RED shockwave leaves its feet
 *             and SFX_RUMBLE plays (the only sound the punch makes). Lumberer
 *             damage, GLM_SHOCK_DAMAGE (35), once per wave.
 *
 *             THE WAVE IS HALF THE LUMBERER'S, MEASURED FROM THE BODY'S EDGE.
 *             The Lumberer's runs 600 from its centre; half is 300, and 300
 *             from the centre could never reach a player its own push-out holds
 *             425 off. So the wave starts at the push surface (GLM_PUSH_RADIUS)
 *             and travels GLM_SHOCK_TRAVEL (300) beyond it: GLM_SHOCK_REACH,
 *             530 from the centre. Chosen by the user over shrinking the body.
 *
 *     LASER   Otherwise. Frames 1/6 for GLM_LAS_WAIT (1.5 s), frame 2 for
 *             GLM_LAS_CHARGE (1.5 s; was 1 s, raised 50%) with Asag's charge
 *             glow growing on its chest, then frame 3 for GLM_LAS_FIRE (2.5 s)
 *             and the beam. Asag's beam (crossed additive quads,
 *             src/asag_fight.c) and his sound, SFX_LASER, on the frame the beam
 *             leaves it - and the clip PLAYS OUT IN FULL (4.4 s), never cut at
 *             the beam's end: a whole cycle is 5.5 s, so it is over before the
 *             next one starts.
 *
 *             AIMED LIKE THE SPIDER'S SPIT (src/spider.c, web_spawn): from the
 *             chest straight at the player's anchor, player_x/y/z(). It tracks
 *             the player through the wait and the charge and >>> LOCKS
 *             GLM_LAS_LOCK_LEAD (0.5 s) BEFORE THE BEAM FIRES <<<, so a player
 *             who moves in that half second is not where it is aimed. Locking
 *             on the firing frame itself (the first version) made every beam a
 *             certain hit. The beam runs on along that line until it meets a
 *             wall, a solid prop or the floor (glm_lock_beam), so it is dodged
 *             by stepping off the line, not by backing away. GLM_LAS_DAMAGE
 *             (25) a hit, at most once every GLM_LAS_HIT_COOLDOWN (1 s).
 *
 *             >>> THE BEAM IS DRAWN IN SHORT SEGMENTS, CLIPPED AT THE CAMERA.
 *             <<< Aimed at the player, it runs THROUGH the camera to the wall
 *             behind - so one quad end-to-end always has corners behind the
 *             eye or past 1023 pixels, and the GPU drops it whole: the first
 *             version drew no beam at all, only the damage. Asag's never met
 *             this because his points at the floor, away from the camera.
 *
 *   DEAD    SFX_RUMBLE and a burst of small rock cubes in browns and reds -
 *           Sharur's Tablet's shatter (src/sharur_tablet.c), smaller and kept
 *           to the Golem's own width.
 *
 * DAMAGE. 10 HP, held as HALF-POINTS (GLM_HP_SCALE) because FLAME ROUNDS DO
 * HALF - the Ahriman's model and its reason (damage_scale() never rounds a hit
 * below 1). A swing, a Standard or Scattershot round and a second of the
 * Helluminator (holy, not flame - the user's answer) are 2 each, a Flame Round
 * 1. golem_scale_damage() is the conversion; every weapon passes its plain 1
 * through it. No knockback: it does not move.
 *
 * SIZE. Wider than it is tall, as briefed - 500 x 440, against the Lumberer's
 * 346 x 520 - which STRETCHES the art (each frame is ~85 x 128, portrait) about
 * 1.7x sideways. The user chose the stretch over the art's own aspect.
 *
 * TEXTURES. ONE 256x256 4bpp sheet (textures/underworld/golem.png), 3x2,
 * numbered 1 2 3 / 4 5 6. Shipped as two 256x128 halves on the SECOND halves of
 * the crawler's blocks (x384 / x768, y128 - the Ahriman has the first),
 * CLUTs (176,480)/(192,480): tools/split_golem_sheet.py, disc.xml. Voff 128,
 * so every sprite brackets its own texture window.
 *
 * >>> ONLY WINGSCAPE, A WOMAN SCORNED, RIBCAGE AND THE TUBE UPLOAD THE SHEET
 * TODAY. <<< Placing one in another room
 * means: the placement in world.c, the room in golem_room_has_sheet() (the .c)
 * - which main.c and tools/gen_tex_rooms.py key the upload on - and
 * update_golems() / draw_golems() / golems_set_texwindow() in that room. The
 * room must not draw anything else at x384 or x768 y128.
 *
 * PERSISTENCE: the Ahriman's model. ONE global array tagged by area, every loop
 * gated on current_area (NEVER game_state), deaths keyed by canonical_index()
 * in WorldDelta.golems_dead. MAX_GOLEMS is the WHOLE-GAME budget.
 *
 * SOUND. Nothing of its own: SFX_RUMBLE (the punch landing, the death) and
 * SFX_LASER (Asag's beam), both given an UNDERWORLD copy in sound.c; SFX_AXEHIT
 * for a hit and SFX_HURT for the player's.
 * ----------------------------------------------------------------------- */

#define MAX_GOLEMS             8    /* WHOLE-GAME budget. Seven seeded: two in
                                       Wingscape (the west door's, and the
                                       Wings Tablet's summoned guard), two
                                       free-standing in A Woman Scorned,
                                       Ribcage's in front of its east door,
                                       and two in The Tube - in front of its
                                       north door, and (absent until the
                                       Destiny Tablet is solved) its south */

/* ---- Health, in half-points - see above. */
#define GLM_MAX_HEALTH        10
#define GLM_HP_SCALE           2

/* ---- Sprite. 500 wide x 440 tall (see SIZE above). GLM_Y_OFFSET + GLM_HALF_H
   == 150 is every sprite enemy's invariant: the drop from the anchor to the
   feet. Headroom in Wingscape: nothing - the west door stands under the sky. */
#define GLM_HALF_W           250
#define GLM_HALF_H           220
#define GLM_Y_OFFSET        (150 - GLM_HALF_H)   /* -70 */
/* The hit shape for the gun and the lantern: the art reaches nearly to the edge
   of each frame, so only a margin comes off. */
#define GLM_HIT_HALF_W       225
#define GLM_HIT_HALF_H       210
/* 2x2 pieces: a 500-wide quad is DROPPED by the GPU inside a view depth of
   500*256/1023 = 125; each 250-wide piece only inside 62, far inside the
   push-out (tools/ADDING_AN_ENEMY.txt mistake 13). */
#define GLM_SUBDIV             2

/* The chest core - the middle of the bright yellow-orange (229,122,39) blob -
   which is NOT in the same place in the two laser frames, so each has its own
   point: X along the billboard's right vector (+ = the player's right), Y from
   the sprite's centre (- = up). Measured as the centroid of the largest blob
   of that colour in each frame, scaled by the stretch (85 texels -> 500 wide,
   128 -> 440 tall):
     frame 2 (the charge)  u 51.2 of 86, v 54.8  ->  +48, -31
     frame 3 (the beam)    u 44.0 of 85, v 64.6  ->   +9,  +2
   The first version used one point, centred across and 30 up, which put the
   charge glow ~48 left of frame 2's core. Re-measure if the art changes. */
#define GLM_CHARGE_CORE_X     48
#define GLM_CHARGE_CORE_DY  (-31)
#define GLM_FIRE_CORE_X        9
#define GLM_FIRE_CORE_DY       2

/* Frames per breath: frame 1 then frame 6, 0.75 s each - a slow 1.5 s breath. */
#define GLM_BREATH_RATE       45

/* ---- The plug: the Lumberer's (lumberer.h, THE PLUG), sized to the wider
   body. The player is held GLM_HOLD_DIST off its centre, before the walls, at
   most GLM_PUSH_STEP a frame. */
#define GLM_PUSH_RADIUS      230
#define GLM_WALL_LIKE_PUSH   195   /* what apply_collision_reception passes in */
#define GLM_HOLD_DIST       (GLM_PUSH_RADIUS + GLM_WALL_LIKE_PUSH)   /* 425 */
#define GLM_PUSH_STEP         48

/* ---- The crucifaxe, measured to the PUSH SURFACE (the Lumberer's
   LMB_AXE_REACH): (d - GLM_PUSH_RADIUS) + |dy| < SWING_RANGE. The body centre
   is 30 below the eye on level ground, so the axe reaches 230 + 350 - 30 = 550
   - past the 425 the player is held at. */
#define GLM_AXE_REACH       (GLM_PUSH_RADIUS + SWING_RANGE - 30)   /* 550 */
_Static_assert(GLM_AXE_REACH > GLM_HOLD_DIST,
               "the plug would hold the player outside the axe's reach");

/* ---- Storey: every radius below is in PLAN, so a player a floor above or
   below is out of the punch and the wave (lumberer.h's LMB_STOREY_REACH). The
   laser is a straight line in 3D and needs no such test. */
#define GLM_STOREY_REACH     500

/* ---- The punch. 1.25 s against the Lumberer's two seconds. */
#define GLM_PUNCH_RADIUS     500   /* true radial: chosen when inside this     */
#define GLM_WINDUP_FRAMES     45   /* frame 4, the arm going up (0.75 s)       */
#define GLM_STRIKE_FRAMES     30   /* frame 5, the arm down and the wave       */

/* The wave: from the push surface, GLM_SHOCK_TRAVEL beyond it (see the head).
   It must reach past the punch radius, so a player who was in range when the
   arm went up and did not move is hit. */
#define GLM_SHOCK_TRAVEL     300
#define GLM_SHOCK_REACH     (GLM_PUSH_RADIUS + GLM_SHOCK_TRAVEL)   /* 530 */
#define GLM_SHOCK_EXPAND      12   /* half the Lumberer's 24                   */
#define GLM_SHOCK_LINGER       6
#define GLM_SHOCK_BAND        90   /* radial width of the ring                 */
#define GLM_SHOCK_WALL_H     110   /* the skirt on its leading edge            */
#define GLM_SHOCK_SEGS        16
#define GLM_SHOCK_DAMAGE      35   /* the Lumberer's LMB_SHOCK_DAMAGE          */
_Static_assert(GLM_SHOCK_REACH > GLM_PUNCH_RADIUS,
               "a player who stood still through the wind-up would not be hit");
_Static_assert(GLM_PUNCH_RADIUS > GLM_HOLD_DIST,
               "the plug would hold the player outside the punch");
_Static_assert(GLM_SHOCK_EXPAND + GLM_SHOCK_LINGER <= GLM_STRIKE_FRAMES,
               "the wave would outlive the strike");

/* ---- The laser, in frames at 60 fps. */
#define GLM_LAS_WAIT          90   /* 1.5 s idle (frames 1/6), still aiming    */
#define GLM_LAS_CHARGE        90   /* 1.5 s on frame 2, the glow growing       */
#define GLM_LAS_LOCK_LEAD     30   /* the aim freezes this long before firing  */
#define GLM_LAS_FIRE         150   /* 2.5 s on frame 3, the beam               */
_Static_assert(GLM_LAS_LOCK_LEAD < GLM_LAS_CHARGE,
               "the aim would lock before the charge began");
/* SFX_LASER is 4.44 s (266 frames) and is never cut, so a whole cycle must
   outlast it or the next beam's cue would re-key a clip still playing. */
_Static_assert(GLM_LAS_WAIT + GLM_LAS_CHARGE + GLM_LAS_FIRE > 266,
               "the next beam would cut the last one's sound off");
/* The beam is drawn in GLM_BEAM_SEG-long pieces, each clipped to sit at least
   GLM_BEAM_NEAR in front of the camera (see the head). */
#define GLM_BEAM_SEG         150
#define GLM_BEAM_NEAR         96
#define GLM_LAS_DAMAGE        25
#define GLM_LAS_HIT_COOLDOWN  60   /* at most one hit a second                 */
#define GLM_LAS_MAX_LEN     6000   /* past the far wall of any room it is in   */
/* The beam's half-width at the chest and at its far end (Asag's are 26 and
   64 over a much longer, steeper beam), and how far off its centre line the
   player's column still counts as hit. */
#define GLM_BEAM_HALF_NEAR    18
#define GLM_BEAM_HALF_FAR     40
#define GLM_BEAM_HIT_R        60

#define GLM_BAR_TIMER_MAX    120

typedef enum {
    GLM_IDLE,      /* breathing, until it is hurt                              */
    GLM_LAS_WAITING,
    GLM_LAS_CHARGING,
    GLM_LAS_FIRING,
    GLM_WINDUP,    /* the punch: arm up                                        */
    GLM_STRIKE,    /* the punch: arm down, the wave                            */
    GLM_DEAD,
} GolemState;

/* What a Golem stops the player using while it stands (see the head). One
   value per guarded thing, in any room; NOTHING is the default. */
typedef enum {
    GLM_BLOCKS_NOTHING = 0,
    GLM_BLOCKS_WINGS_TABLET,       /* Wingscape: the tablet's summoned guard   */
    GLM_BLOCKS_WINGSCAPE_W_DOOR,   /* Wingscape: the door Golem, west spot     */
    GLM_BLOCKS_WINGSCAPE_SE_DOOR,  /* Wingscape: the door Golem, south-east    */
    GLM_BLOCKS_RIBCAGE_E_DOOR,     /* Ribcage: the east door, to Lightnings Edge */
    GLM_BLOCKS_TUBE_N_DOOR,        /* The Tube: the north door, to Destinys Cabin */
    GLM_BLOCKS_TUBE_S_DOOR,        /* The Tube: the south door, to Heavens Gate -
                                      the Golem the Destiny Tablet's solve brings */
} GolemBlock;

typedef struct {
    int32_t     x, y, z;        /* y is the standing anchor (floor - 149)      */
    int32_t     spawn_x, spawn_z, spawn_y;
    int32_t     vy;

    int         health;         /* half-points: GLM_MAX_HEALTH*GLM_HP_SCALE    */
    int         hit_timer;
    int         anim_tick;
    int         timer;          /* frames into the current state               */
    int         las_cool;       /* frames until the beam may hit again         */

    /* The beam: the chest it leaves from, and where it ends - the aim point
       while waiting/charging, the locked end once firing. */
    int32_t     bx0, by0, bz0;
    int32_t     bx1, by1, bz1;

    int32_t     wave_t;         /* 0 = no wave; else frames since it launched  */
    int         wave_hit;       /* this wave has already hurt the player       */

    GolemState  state;
    int32_t     active;
    int         on_upper_floor;
    int         on_ramp;
    GameState   area;
    /* 1 = a SUMMONED Golem (the Wings Tablet's guard, src/wings_tablet.h):
       planted ABSENT (GLM_DEAD) and brought in by golem_summon(). Survives
       glm_spawn's wipe, so golems_rest() keeps it a guard. */
    int         guard;
    /* What it stops the player using while it stands (GolemBlock; the head).
       Survives glm_spawn's wipe like `guard`. */
    GolemBlock  blocks;
    /* 1 = ALERT: every spawn starts it attacking, never IDLE (the head).
       Survives glm_spawn's wipe like `guard`. */
    int         alert;
} Golem;

extern Golem golems[MAX_GOLEMS];
extern int   golem_count;

/* Register the two sheet halves with texmgr (TEXBANK_UNDERWORLD). Call ONCE at
   startup, beside the other enemies'. */
void golems_load_textures(void);
/* Stream both halves into VRAM. main.c calls it from STATE_LOADING for every
   area golem_room_has_sheet() names. */
void golems_upload_textures(void);
int  golem_room_has_sheet(GameState area);

/* Plant one at (x,z). Authored - nothing here reads the mesh - so world.c can
   rebuild a placement from a save. Returns its index, or -1 if the pool is
   full. */
int  golem_add(int32_t x, int32_t z, GameState area);
/* Re-plant a living Golem somewhere else (a route- or flag-dependent spot, see
   the head). Does nothing to a dead one. */
void golem_set_spawn(int idx, int32_t x, int32_t z);

/* ---- BLOCKING (see the head) -----------------------------------------------
     golem_set_blocks  tag Golem idx with what it blocks (alive or not).
     golem_blocks      1 while any STANDING Golem in `area` blocks `what` -
                       the guarded thing's own trigger and prompt ask this. */
void golem_set_blocks(int idx, GolemBlock what);
int  golem_blocks(GameState area, GolemBlock what);

/* Mark Golem idx ALERT (see the head): awake now, and on every spawn after.
   Set beside its seed in world.c, so a title load's re-seed sets it again. */
void golem_set_alert(int idx);

/* ---- SUMMONED GOLEMS (the Wings Tablet's guard, src/wings_tablet.h) --------
   A guard is a seeded slot like any other - so it costs one place in the
   MAX_GOLEMS budget and one golems_dead bit however often it is summoned -
   but it is planted ABSENT: GLM_DEAD, nothing drawn, nothing collided. A
   summon puts it back at its spawn; a kill makes it absent again, ready for
   the next summon. Its "is it standing" across a load is the owner's flag
   (FLAG_WINGS_GUARD_UP), not its dead bit - see player.h.
     golem_add_guard  plant one ABSENT at (x, z), standing on floor_y (the
                      floor's height, not the anchor: -500 for a terrace).
     golem_guard      the area's guard, or NULL.
     golem_summon     bring it in at its spawn, AWAKE: it chooses its first
                      attack on its next update, as a first hit would make it.
     golem_revive     bring it in at its spawn, IDLE - the load path's, so a
                      guard comes back after a load as one left behind does. */
int    golem_add_guard(int32_t x, int32_t floor_y, int32_t z, GameState area);
Golem *golem_guard(GameState area);
void   golem_summon(Golem *s);
void   golem_revive(Golem *s);

void golems_init(void);
void golems_reset(void);
/* Every living Golem back at its spawn, IDLE, at full health (deaths stick),
   and every shard and beam cleared. Called when leaving a room. */
void golems_rest(void);

void update_golems(void);
void draw_golems(RenderContext *ctx);

/* Damage in HALF-POINTS - pass every weapon's base through
   golem_scale_damage() first. Any damage wakes an idle Golem. */
void golem_damage(Golem *s, int dmg);
int32_t golem_scale_damage(int32_t base, DamageType type);
/* The crucifaxe test (GLM_AXE_REACH). 1 if it landed on one. */
int  golems_try_hit(void);
/* The player's push-out; called from apply_collision_reception. */
void golems_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* Grave-olver / Helluminator support: body-centre Y, half-height, half-width. */
void golem_body(const Golem *s, int32_t *cyc, int32_t *hh, int32_t *hw);

/* The area's texture window, restored after each sprite (the sheet is at Voff
   128). NULL for an area with none. */
void golems_set_texwindow(const RECT *tw);

#endif
