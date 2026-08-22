#ifndef HADAD_H
#define HADAD_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "player.h"   /* MAX_HEALTH — the contact blow is a quarter of it */
#include "title.h"    /* GameState — tagged with its area like every global enemy */
#include "crucifaxe.h" /* SWING_RANGE — HAD_CATCH_DIST is derived from it */

/* -----------------------------------------------------------------------
 * HADAD — the thing on the Rear Gate's plinth.
 *
 * Built on the Living Statue (src/living_statue.c), which is the enemy the user
 * pointed at, and it keeps that one's shape almost exactly: a camera-facing
 * billboard standing on a plinth, a solid cylinder so it cannot be walked
 * through, the mushroom's feeler steering, a surface-measured crucifaxe reach,
 * and stone-grey particles instead of blood. What it drops is the teleport
 * cycle and the two watching gates — Hadad does not sneak. What it ADDS, and
 * what most of this file is about, is that he is a SCRIPTED ENCOUNTER rather
 * than a wandering monster: two persistent flags decide, between them, which of
 * three completely different things the room contains.
 *
 * >>> HE IS ALSO THE PLINTH'S EPITAPH MADE GOOD. <<< rear_gate.c's inscription
 * reads "In memoriam of Hadad, a great hero", and the statue standing over it
 * is him. Nothing in the code depends on that; it is why the placement is where
 * it is.
 *
 * ======================================================================
 * THE THREE ROOMS, BY FLAG
 * ======================================================================
 * Read FLAG_HADAD_TWO first: it does not layer on top of flag one, it REPLACES
 * it. The user's words were "once this flag is active we ignore flag one".
 *
 *   NEITHER FLAG          Hadad is a statue on the plinth (HAD_IDLE), and he
 *                         is INVULNERABLE — hadad_damage refuses outright, so
 *                         an axe swing at him is a silent no-op the way a swing
 *                         at a stalking Living Statue is. The corridor lever
 *                         works normally.
 *                         Walking up to the grinders (HAD_TRIG_RADIUS of
 *                         HAD_GRINDER_X/Z) sets FLAG_HADAD_ONE and starts the
 *                         first encounter.
 *
 *   FLAG ONE ONLY         The first encounter. He is off the plinth for good.
 *                         On the frame it arms he TELEPORTS to the corridor's
 *                         north mouth (HAD_MOUTH_Z) — behind the player, across
 *                         the only way back to the lawn — and walks the corridor
 *                         south to HAD_END_Z, where he stops for ever.
 *                         >>> AND THE LEVER GOES DEAD. <<< hadad_lever_locked()
 *                         is true for exactly this state, so the player cannot
 *                         close the corridor on him and cannot re-open it
 *                         either. Every later visit to the room finds him
 *                         already standing at HAD_END_Z (HAD_ROOTED).
 *
 *   FLAG TWO (either way)  The second encounter. The lever WORKS again. Hadad
 *                         is not in the room at all to begin with (HAD_ABSENT:
 *                         not drawn, not solid, not damageable). Walking down
 *                         the ramp and back up into the corridor makes him
 *                         appear at the TOP of the ramp, behind the player,
 *                         and from there he simply follows them — up the
 *                         corridor, out onto the lawn, anywhere they go.
 *                         Leaving the room ENDS him: `spent` is set, and on the
 *                         next visit he is back on the plinth in HAD_IDLE and
 *                         can never be armed again by anything.
 *
 * `spent` is the one bit of this that is not derivable from the flags, so it is
 * the one bit that rides the save (WorldDelta.hadads_state, bit 1).
 *
 * ======================================================================
 * THE SIZE, AND WHY THE END SPOT IS WHERE IT IS
 * ======================================================================
 * 600 x 600 world units — as wide as the plinth (x[-300,300], so 600) and, as
 * of the halving, square. He was 600 x 1200 (twice his own width) for one build
 * and it read as too much; the WIDTH is the load-bearing half of that pair and
 * did not move. For scale: the zombie sprite is 124 x 250 and the Living Statue
 * 128 x 322, so he is still nearly twice a statue tall and five times its
 * volume. Standing on the plinth his crown reaches mesh y -800 against a
 * 500-tall perimeter hedge, so he still stands 300 proud of the hedge line and
 * is visible across the lawn from the east gate — which is the point of putting
 * him on a plinth in the middle of it.

 * >>> HALVING THE HEIGHT DID NOT MOVE HIS FEET. <<< HAD_Y_OFFSET absorbed all
 * of it (see the invariant at that constant), so every placement below, and the
 * plinth anchor in world_seed_room, are the same numbers they were.
 *
 * >>> HIS BODY IS AS WIDE AS THE CORRIDOR, AND THAT IS LOAD-BEARING. <<<
 * The southern path is 600 wide between collision walls 0 and 1. His push
 * cylinder is HAD_BODY_RADIUS + the room's 195 wall radius = 495, which covers
 * x[-495,495] across a lane the player can only occupy x[-105,105] of. Standing
 * anywhere on the centre line he is a plug, not an obstacle.
 *
 * THE END SPOT IS THE CORRIDOR'S SOUTH MOUTH (0, -1500), NOT THE FOOT OF THE
 * RAMP. The design says "the bottom of the corridor... blocking the way back
 * for the player", and those two have to be the same point. The ramp's foot is
 * at z=-2100, and a body parked there blocks nothing: z[-2100,-1500] is the
 * 1800-wide opening onto the two side courts, so the player simply walks round
 * him. The corridor mouth at z=-1500 is the ONE place south of the lawn that is
 * 600 wide, and walls 25 and 28 (the courts' north faces, normals pointing
 * south) mean the courts are dead ends with no way north of their own. So
 * z=-1500 is the only spot that actually delivers "no choice but to go into the
 * door", and it is also the bottom of the corridor as drawn. Both readings land
 * on the same number.
 *
 * ======================================================================
 * DAMAGE
 * ======================================================================
 * 100 HP and the CRUCIFAXE ONLY, at 1 a swing — a hundred connected swings. As
 * with the Living Statue there is deliberately no Grave-olver targeting loop
 * anywhere in the codebase for him, so a round spent on him is wasted, and
 * hadad_damage refuses every hit while he is IDLE or ABSENT so the axe is a
 * silent no-op there too.
 *
 * THE ONE EXCEPTION IS THE GRINDERS, and only under flag two: the corridor
 * lever is live again in that state, and if he is standing between the plates
 * when it is thrown his health is emptied in one go (hadads_grinder_crush).
 * That is the intended answer to a hundred-swing health bar.
 *
 * ======================================================================
 * SOUND AND MUSIC
 * ======================================================================
 * SFX_RUMBLE on every arrival and on death — the Living Statue's own clip,
 * reused deliberately rather than for want of a new one, because it is the
 * noise stone makes moving and Hadad is stone. It is in the HOUSE bank AND the
 * GARDEN one, so it sounds wherever he is put: the Rear Gate is on GARDEN and
 * the West Corridor, Reception and the Library are on HOUSE (main.c's
 * STATE_LOADING maps every room to a bank). >>> IF HE IS EVER PLACED IN THE
 * GARDEN COURTYARD, HE ARRIVES IN SILENCE <<< — that room takes SND_BANK_BOSS
 * for the Rabisu, and the boss bank has no copy of this clip. See sound.h for
 * why a third copy is not affordable.
 *
 * The stalker music is CD-DA track 8 and it starts HAD_RUMBLE_FRAMES after the
 * rumble is triggered, which is the clip's own length — "as soon as the rumble
 * sound has finished playing". It loops from 4.015 s rather than from the top;
 * that is a property of the track and lives in cdaudio.c, not here. It stops
 * the moment the player leaves the room (hadads_silence, called from
 * world_silence_monsters) and when he dies.
 *
 * ======================================================================
 * PERSISTENCE
 * ======================================================================
 * The spiders'/statues' model: ONE global array tagged by area, in world.c's
 * WorldState global section, with every update/draw/weapon/collide loop
 * skipping any instance whose area is not current_area — current_area and NEVER
 * game_state, or he freezes for as long as the inventory menu is open
 * (tools/ADDING_AN_ENEMY.txt STEP 6). MAX_HADADS is a whole-game budget.
 * ----------------------------------------------------------------------- */

#define MAX_HADADS            1
#define HAD_MAX_HEALTH      100   /* crucifaxe swings, at 1 apiece */

/* ---- Sprite geometry -------------------------------------------------------
   600 x 1200 world units. HAD_Y_OFFSET + HAD_HALF_H == 150 is the invariant
   every sprite enemy in this game keeps: that sum is the drop from the entity
   ANCHOR down to the feet, so a half-height this large has to come back out of
   the offset, which goes strongly negative. Break the invariant and he floats
   or sinks by exactly the error.

   The art is three 64x96 8bpp TIMs, not the 128x128 the older sprite enemies
   use, because the only VRAM left in the game is the 96-row band under the HUD
   and three frames need three slots in it (tools/ADDING_AN_ENEMY.txt STEP
   3b-ter). The quad's world size is set HERE and not by the art, so the smaller
   texture costs resolution and nothing else. All three sit at Voff 128, so
   every quad brackets its own texture window. */
#define HAD_HALF_W          300
#define HAD_HALF_H          300
#define HAD_Y_OFFSET       (-150)   /* 150 - HAD_HALF_H */
_Static_assert(HAD_Y_OFFSET + HAD_HALF_H == 150,
               "the anchor-to-feet drop is 150 for every sprite enemy here; "
               "break it and Hadad floats or sinks by exactly the error");

/* ---- Placement -------------------------------------------------------------
   All three are AUTHORED, not probed: only the plinth spawn goes through
   world_seed_room (which runs for rooms whose geometry is not resident), but
   keeping the other two here as well means the whole route reads in one place.
   The two teleports DO probe the floor for their Y, because they happen live
   with the room loaded — see had_floor_anchor.

     PLINTH   the block in the middle of the lawn, x[-300,300] z[2500,3100],
              200 tall, so its centre is (0, 2800) and its top face is mesh
              y=-200. The feet sit at anchor + 150, so the anchor is -350 and
              the crown reaches -800.
     MOUTH    the corridor's NORTH end, the 600-wide gap at z=1500 between
              collision walls 14 and 15. Standing in it he closes the only route
              back to the lawn, which is what "blocking the way back to the
              north part of this level" asks for.
     END      the corridor's SOUTH mouth. See the long note above for why this
              is not the foot of the ramp.
     RAMPTOP  the flag-two arrival. The ramp climbs from y=0 at z=-2100 to
              y=-500 at z=-3200; z=-2850 is 750 up that 1100 run, i.e. surface
              y=-341 and an anchor of -490.

              >>> IT IS -2850 AND NOT -2900 BECAUSE OF COLLISION WALL 39. <<<
              That is the brick wall's upper band at z=-3200, and
              apply_flat_entity_collision uses collide_wall_frontonly, which —
              unlike the player's collide_wall_frontonly_y — HAS NO Y GATE. So
              the wall pushes entities at every height, and a 300-radius body
              at z=-2900 sits 288 into a 300 push and is shoved 12 units north
              on its first frame. -2850 clears it by 50. (The PLAYER is held at
              z=-2994 by the same wall, so he still arrives well behind
              anybody standing at the door.) */
#define HAD_PLINTH_X            0
#define HAD_PLINTH_Z         2800
#define HAD_PLINTH_ANCHOR   (-350)   /* plinth top -200, feet at anchor + 150 */
#define HAD_MOUTH_X             0
#define HAD_MOUTH_Z          1500
#define HAD_END_X               0
#define HAD_END_Z           (-1500)
#define HAD_RAMPTOP_X           0
#define HAD_RAMPTOP_Z       (-2850)

/* ---- The two triggers ------------------------------------------------------
   FLAG ONE arms on proximity to the two grinders, which sit at z=-85 in the
   corridor's side hedges (grinder_puzzle.c's GP_Z). The radius is TRUE RADIAL,
   so 300 means 300 from whichever way the player walks in. It is sized against
   the one other thing in the corridor worth pressing: the lever is at z=300,
   which is 385 away, so throwing it does not arm Hadad by accident. It is also
   comfortably wider than the lane — the player is held to |x| <= 105 by the
   hedges, so anybody walking the corridor at all passes within 300 of the
   grinders and cannot dodge past on one side.

   FLAG TWO is a two-part latch, because "steps into the corridor FROM the
   bottom of the ramp" is a direction and not a place. HAD_RAMP_ARM_Z is far
   enough up the ramp that only a player who really went to the door trips it;
   HAD_RAMP_FIRE_Z is just north of the ramp's foot at z=-2100. One without the
   other would fire on a player who had merely walked south, which would put
   Hadad in front of them instead of behind. */
#define HAD_GRINDER_X           0
#define HAD_GRINDER_Z        (-85)
#define HAD_TRIG_RADIUS       300
#define HAD_RAMP_ARM_Z     (-2200)   /* player is genuinely up the ramp   */
#define HAD_RAMP_FIRE_Z    (-2050)   /* ...and has come back off the foot */

/* ---- The walk --------------------------------------------------------------
   HAD_SPEED is deliberately UNDER the player's own walk speed (camera.c: 12
   walking, 20 sprinting). It was 7 — a shade over half a walk — and is now 4,
   the "about 50% slower" that was asked for. Walking away from Hadad opens a
   gap, which is the whole reason the first encounter works as a shove down the
   corridor rather than as a fight. He does not need to catch anybody; he needs
   to arrive.

   >>> HAD_STEP_FRAMES HAS TO MOVE WITH IT, AND IN THE OPPOSITE DIRECTION. <<<
   The walk cycle is counted in FRAMES, so leaving it alone while the speed
   halves halves the ground covered per stride and he moonwalks. What has to
   stay constant is the DISTANCE per animation frame: it was 7 x 22 = 154, and
   4 x 39 = 156 holds it to within a unit and a half. Retune one and recompute
   the other from that product.

   The feeler is longer than the Living Statue's 170 because his BODY is: a
   probe shorter than the cylinder's own radius reports clear ground the body
   cannot fit into, and he would grind on every hedge corner in the room.
   400 clears HAD_BODY_RADIUS by a third of itself. */
#define HAD_SPEED             4
#define HAD_FEELER_LEN      400
#define HAD_TURN_RATE         3
#define HAD_STEER_COMMIT     30
/* Close enough to HAD_END_X/Z to call the walk finished. It has to be wider
   than one frame's travel (HAD_SPEED, 7) with room over, or he oscillates
   around the spot for ever without ever satisfying the test. */
#define HAD_ARRIVE_DIST      40

/* ---- Solid body ------------------------------------------------------------
   The user asked for "a collision zone around him so the player can't walk
   through him", exactly as the Living Statue has. Same arrangement as that
   enemy's: apply_collision_reception passes the room's own 195 WALL radius
   rather than the 75 a prop gets, and the push is applied BEFORE the wall
   passes so that hard geometry always gets the last say and the player can
   never be wedged into a hedge (tools/ADDING_AN_ENEMY.txt mistake 10).

   HAD_BODY_RADIUS is his sprite's own half-width, so the billboard still
   projects whole at the stop distance from every bearing. */
#define HAD_BODY_RADIUS     300
#define HAD_WALL_LIKE_PUSH  195   /* what apply_collision_reception passes in */
#define HAD_HOLD_DIST       (HAD_BODY_RADIUS + HAD_WALL_LIKE_PUSH)   /* 495 */

/* ---- The blow --------------------------------------------------------------
   A quarter of the player's MAXIMUM health, so four connected blows kill from
   full and the fourth one really does finish them — a quarter of CURRENT health
   would halve and halve and never reach zero. Plus a knockback stronger than
   anything but the Rabisu's shockwave (RBS_SHOCK_KNOCKBACK, 270).

   >>> THE REACH MUST CLEAR HIS OWN PUSH-OUT. <<< His cylinder parks the player
   at HAD_HOLD_DIST, 495, and a reach shorter than that is a body that walks up
   to somebody, stops itself, and can then never touch them — mistake 9 in
   tools/ADDING_AN_ENEMY.txt with the floor set by the enemy's own width instead
   of by the room. The static assert below is the whole guard.

   >>> AND IT MUST MATCH THE AXE'S OWN REACH EXACTLY. <<< "The player should
   never be able to get close enough that he can hit it without also taking
   damage" is a statement about two numbers agreeing, so the two are now ONE
   number: hadads_try_hit lands a swing when (d - HAD_BODY_RADIUS) + gv is under
   SWING_RANGE, which with the eye level with any part of him (gv = 0) is
   d < HAD_BODY_RADIUS + SWING_RANGE. That IS HAD_CATCH_DIST. Derived, never
   typed, so retuning either the body or the axe can never reopen the gap —
   at 545 there was a 105-unit band the player could swing from in safety.

   Both tests are strict `<` against the same figure, so the boundary agrees
   too: at d = 650 neither fires, at d = 649 both do. The vertical halves cannot
   reopen it either, because the axe needs gv < SWING_RANGE (so |dy| < HALF_H +
   350 = 650) while the contact budget is HAD_CATCH_DIST + HAD_HALF_H = 950 —
   strictly the more generous of the two, which is the safe direction.

   >>> WHAT THIS DOES NOT BUY IS IMMUNITY FROM THE COOLDOWN. <<< He strikes
   every HAD_HIT_COOLDOWN frames, so a player who accepts the first blow can
   swing freely for the rest of that second and a half. Closing THAT is a
   cadence change, not a range one; the rule above is about distance, and the
   first swing always costs them.

   HAD_CATCH_DIST is RADIAL like the Living Statue's, not the Manhattan sum most
   enemies here use: at this size the diagonal error is over 200 units, which
   would make him strike from the compass points and stand harmlessly against a
   player who approached cornerwise.

   The cooldown is a second and a half, which at 25 a blow is a kill in four and
   a half seconds of standing still next to him. */
#define HAD_CATCH_DIST      (HAD_BODY_RADIUS + SWING_RANGE)   /* 650 */
_Static_assert(HAD_CATCH_DIST > HAD_HOLD_DIST,
               "Hadad's reach must clear his own push-out, or his solid body "
               "holds the player outside the range he can strike from and he "
               "can never land a blow");
_Static_assert(HAD_CATCH_DIST >= HAD_BODY_RADIUS + SWING_RANGE,
               "the crucifaxe would then reach him from outside the range he "
               "can strike from, and the player could kill him in safety");
#define HAD_DAMAGE          (MAX_HEALTH / 4)   /* 25 — a quarter of FULL health */
#define HAD_HIT_COOLDOWN     90                /* 1.5 s between blows           */
#define HAD_KNOCKBACK       200

/* ---- Animation and cues ----------------------------------------------------
   Two step frames alternating on a walk cycle; hadad_idle is the plinth pose
   and the pose he goes back to if flag two burns out. A Hadad who has ARRIVED
   (HAD_ROOTED) holds HAD_TEX_STEP1 rather than reverting to hadad_idle — he is
   a monster standing in a corridor, not a statue again, and the idle art reads
   as the latter.

   HAD_RUMBLE_FRAMES is rumble.vag's own length: 23324 samples at 11025 Hz =
   2.1156 s = 126.9 frames at 60 fps. The stalker music starts on the frame that
   expires, which is what "as soon as the rumble sound has finished playing"
   asks for. >>> RETRIM rumble.vag AND THIS HAS TO MOVE WITH IT <<< — the same
   contract SFX_GATE has with door_anim.c's GATE_SWING_FRAMES and SFX_GRIND has
   with grinder_puzzle.c's GP_CLIP_FRAMES. */
#define HAD_STEP_FRAMES      39   /* frames per walk frame — see HAD_SPEED for
                                    why this is 39 and not 22 any more      */
/* >>> HE STANDS STILL FOR A SECOND FIRST. <<< Every arrival — both flags — puts
   him down, sounds the rumble and then does NOTHING for a second before the
   first stride. It reads as the statue noticing, and it is also the only window
   in the encounter where the player is looking at him and he is not closing.
   He holds the IDLE texture through it (draw_hadads), which is what makes the
   pose read as "not yet moving" rather than as a frozen walk cycle. */
#define HAD_WAKE_PAUSE       60   /* 1 s at 60 fps, before the first step */
#define HAD_RUMBLE_FRAMES   127
#define HAD_BAR_TIMER_MAX   120

/* Stone chips, not blood — the same grey the Living Statue dies in, and the
   user asked for it by name. spawn_burst carries a per-burst RGB already, so
   this is the shared effect with the colour swapped and nothing more. */
#define HAD_BLOOD_R         150
#define HAD_BLOOD_G         150
#define HAD_BLOOD_B         155

typedef enum {
    HAD_IDLE,     /* on the plinth: inert, solid, drawn, INVULNERABLE       */
    HAD_ABSENT,   /* flag two, before the ramp trigger: not in the room     */
    HAD_WALK,     /* moving — down the corridor (flag one) or after the
                     player (flag two). `follow` says which               */
    HAD_ROOTED,   /* flag one, arrived: parked at HAD_END_X/Z for good      */
    HAD_DEAD,
} HadadState;

typedef struct {
    int32_t     x, y, z;        /* y is the STANDING ANCHOR, feet at y + 150 */
    int32_t     spawn_x, spawn_y, spawn_z;

    int         health;
    int         hit_timer;      /* health-bar flash / red tint countdown     */
    int         damage_timer;   /* frames until he may strike again          */
    int         pause_timer;    /* frames of the wake pause still to serve    */
    int         step_timer;     /* walk-cycle frame counter                  */
    int         step_frame;     /* 0 or 1: which of the two step textures    */

    /* Steering. `facing` is the last travel step, packed hi16 X / lo16 Z, the
       way the statue and the mushroom carry theirs. */
    int32_t     facing;
    int         steer_timer;
    int         steer_dir;
    /* Gravity/floor state, used only while HAD_WALK. A Hadad standing on the
       plinth is never floor-probed — apply_ddog_height would drag him off it. */
    int32_t     vy;
    int         on_upper_floor;
    int         on_ramp;

    int         follow;         /* 1 = chase the player (flag two); 0 = walk
                                   the corridor to HAD_END (flag one)        */
    int         spent;          /* the flag-two encounter has been used up.
                                   Saved — see WorldDelta.hadads_state       */
    int         ramp_armed;     /* flag two's latch: the player has been up
                                   the ramp. Per-visit, not saved            */
    HadadState  state;
    int32_t     active;
    GameState   area;
} Hadad;

extern Hadad hadads[MAX_HADADS];
extern int   hadad_count;

/* Load the three sprite TIMs. Call ONCE at startup — a CdRead is only safe
   before the per-frame render loop begins (tools/TEXTURING_NOTES.txt). The
   slots are this enemy's own, so there is no upload-on-entry counterpart. */
void hadads_load_textures(void);

/* Place one standing at (x, z) with its FEET at anchor y + 150. Everything is
   AUTHORED — nothing here reads the current room's mesh — so this is safe to
   call for a room that is not loaded, which is what lets world.c rebuild every
   visited room from a save delta. Returns its index, or -1 if the pool is
   full. */
int  hadad_add(int32_t x, int32_t z, int32_t y, GameState area);

void hadads_init(void);
void hadads_reset(void);
/* Put him back where this visit should START him, which is the one thing about
   this enemy that is not "back at his spawn": it depends on the flags. Called
   when leaving a room and when saving. Deaths stick, and so does `spent` —
   which this is also the function that SETS, because leaving the room mid
   flag-two encounter is what burns it out. */
void hadads_rest(void);
void update_hadads(void);
void draw_hadads(RenderContext *ctx);

/* Deal damage. Refused while he is IDLE, ABSENT or DEAD — the crucifaxe already
   skips those before its reach test, so this is the second half of the same
   rule and the place to relax it if he is ever meant to be breakable on the
   plinth. */
void hadad_damage(Hadad *h, int dmg);

/* Scale a hit by this enemy's weaknesses — there are none, by design: the
   crucifaxe does exactly 1 and nothing else can touch him. */
int32_t hadad_scale_damage(int32_t base, DamageType type);

/* Sprite-centre Y, half-height and half-width, for anything that needs the hit
   box. */
void hadad_body(const Hadad *h, int32_t *cyc, int32_t *hh, int32_t *hw);

/* Solid cylinder, area-gated so the call is a no-op everywhere he is not
   placed. Called for the PLAYER from apply_collision_reception, BEFORE the wall
   passes for the reason living_statues_collide is: he stands on a plinth hard
   inside its own collision box, so his push circle overlaps solid geometry and
   pushing it last would wedge the player into the stone. */
void hadads_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* Crucifaxe strike: damage him if he is in reach and in front of the player,
   and return 1 if he was hit. Lives here rather than inline in crucifaxe.c —
   the way tentacles_try_hit and living_statues_try_hit do — because the reach
   has to be measured to this body's SURFACE and the geometry is his business.
   A Hadad who is not in a damageable state is skipped and does NOT count as a
   hit, so the same swing goes on to reach anything else in range. */
int  hadads_try_hit(void);

/* 1 while the corridor lever must refuse to be thrown: the first encounter is
   running or has run, and the second has not replaced it. Read by
   grinder_puzzle_update. */
int  hadad_lever_locked(void);

/* The grinders have closed (or are closing) on the strip x[-x_half,x_half],
   z[z_min,z_max]. If Hadad is standing in it and can be hurt, he is destroyed
   outright. The strip is passed in rather than defined here because it is the
   GRINDERS' geometry and grinder_puzzle.c is where that lives. */
void hadads_grinder_crush(int32_t x_half, int32_t z_min, int32_t z_max);

/* Cut his sounds and his music dead. Called from world_silence_monsters() the
   instant a room transition begins — "the music should stop as soon as the
   player exits the current room". */
void hadads_silence(void);

/* Tell the renderer which texture window the current area has active, so each
   sprite can be drawn unmasked and the area's window then restored. Pass NULL
   for areas that use no texture window. */
void hadads_set_texwindow(const RECT *tw);

#endif
