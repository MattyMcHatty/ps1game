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
 * >>> HE IS NOW IN TWO ROOMS, AND THE SCRIPT IS PER-INSTANCE. <<< Everything
 * below down to "PERSISTENCE" describes the ROLE HAD_ROLE_PLINTH — the original
 * Rear Gate statue and its two-flag state table. The second instance is
 * HAD_ROLE_WEST_CORR, the West Corridor ambush, and it has a state table of its
 * own; see THE WEST CORRIDOR AMBUSH further down. A Hadad's `role` is fixed at
 * hadad_add() time and never changes, and every branch in hadad.c that is
 * specific to one of them tests it explicitly.
 *
 * ======================================================================
 * THE THREE ROOMS, BY FLAG   (HAD_ROLE_PLINTH only)
 * ======================================================================
 * Read FLAG_HADAD_THREE first: it does not layer on top of flag one, it
 * REPLACES it. The user's words were "once this flag is active we ignore flag
 * one".
 *
 * >>> "FLAG ONE, FLAG THREE" IS NOT A TYPO ANYWHERE IN THIS SECTION. <<< This
 * flag was called FLAG_HADAD_TWO up to the rename, and FLAG_HADAD_TWO now means
 * something else entirely: the key stones are back out of the Attic Exit's door
 * and the Library has collapsed. It gates the THIRD instance of this enemy (THE
 * LIBRARY DESTROYED ENCOUNTER, below) and has nothing to do with the plinth's
 * table, which reads flags ONE and THREE only.
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
 *   FLAG THREE            The third encounter, and it does not matter whether
 *                         flag one is set as well — this one REPLACES it. The
 *                         lever WORKS again. Hadad is not in the room at all
 *                         to begin with (HAD_ABSENT: not drawn, not solid, not
 *                         damageable). Walking down the ramp and back up into
 *                         the corridor makes him appear at the TOP of the
 *                         ramp, behind the player, and from there he simply
 *                         follows them — up the corridor, out onto the lawn,
 *                         anywhere they go. Leaving the room ENDS him: `spent`
 *                         is set, and on the next visit he is back on the
 *                         plinth in HAD_IDLE and can never be armed again by
 *                         anything.
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
 * THE ONE EXCEPTION IS THE GRINDERS, and only under flag three: the corridor
 * lever is live again in that state, and if he is standing between the plates
 * when it is thrown his health is emptied in one go (hadads_grinder_crush).
 * That is the intended answer to a hundred-swing health bar.
 *
 * ======================================================================
 * SOUND AND MUSIC
 * ======================================================================
 * >>> THE STANDING RULE, FOR EVERY HADAD IN EVERY ROOM, PRESENT AND FUTURE:
 * HE ARRIVES ON SFX_RUMBLE, AND THE STALKER TRACK COMES UP THE FRAME THAT CLIP
 * ENDS. <<< It is not a property of the Rear Gate encounter, it is the property
 * of the enemy, so a new placement never has to ask for it and never has to
 * wire it: had_arrive() does both, every arrival goes through had_arrive(), and
 * the wanted/on reconciliation at the bottom of update_hadads() carries the
 * track for as long as any Hadad in the current area is WALKing or ROOTED. The
 * only thing a new room owes is a sound bank with SFX_RUMBLE in it (HOUSE and
 * GARDEN have it, BOSS does not — see below).
 *
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
 * THE WEST CORRIDOR AMBUSH   (HAD_ROLE_WEST_CORR)
 * ======================================================================
 * A SECOND, SEPARATE instance, seeded into STATE_WEST_CORRIDOR by
 * world_seed_room. It shares this enemy's body, art, damage rules, music and
 * solid cylinder and NOTHING of the plinth's state table — it is never a
 * statue, it never teleports to a mouth or a ramp, and it never follows.
 *
 *   THE GATE      FLAG_HADAD_ONE set AND FLAG_HADAD_THREE clear. Read the same
 *                 way round as hadad_lever_locked(): flag three REPLACES flag one
 *                 everywhere in this enemy, so once the third Rear Gate
 *                 encounter is armed this ambush is over for good. Outside that
 *                 window he is HAD_ABSENT — not drawn, not solid, not
 *                 damageable, and the room is exactly as it was before he
 *                 existed.
 *
 *   THE TRIGGER   the player entering the corner where the corridor turns.
 *                 The room is an L (west_corridor.h): the WEST ARM runs north
 *                 to the double door, the SOUTH ARM runs east to the single
 *                 one, and they meet in the 600 x 650 pocket around
 *                 (HAD_WC_CORNER_X, HAD_WC_CORNER_Z). HAD_WC_TRIG_RADIUS is
 *                 TRUE RADIAL like the grinders' one, so it fires at the same
 *                 400 whichever arm the player walks in down.
 *
 *   THE WALK      he appears at the NORTH end, in front of the double door,
 *                 walks the west arm south to the corner, turns, and walks the
 *                 south arm east to a stop in front of the single door. TWO
 *                 LEGS, not one: the corridor turns, and a single straight goal
 *                 at the far door would steer him into the west arm's east wall
 *                 and leave the wall-follow to sort out a right angle it has no
 *                 need to. `leg` is the index; had_path_goal() in hadad.c is
 *                 the whole path.
 *
 *                 >>> HE MARCHES IT, HE DOES NOT STEER IT, AND HE IGNORES WALL
 *                 GEOMETRY WHILE HE DOES. <<< The user's rule: "he's on a set
 *                 path so collision with the geometry for Hadad isn't
 *                 important. He should collide with the player for definite."
 *                 Both legs run down the centre line of a straight, empty arm,
 *                 so there is nothing for steering to earn — and had_steer's
 *                 truncating heading blend actively broke the eastward leg,
 *                 crabbing him along at half speed into the south wall. See
 *                 had_march() in hadad.c for the arithmetic. HIS PUSH AGAINST
 *                 THE PLAYER IS UNAFFECTED: that is hadads_collide, a separate
 *                 mechanism, and he is still a plug in a 600-wide corridor.
 *
 *                 >>> HE ENDS UP BLOCKING THE DOOR HE STOPS AT. <<< His push
 *                 cylinder holds the player HAD_HOLD_DIST off, which is wider
 *                 than WCDOOR_TRIGGER_RADIUS, so once he is rooted at
 *                 HAD_WC_END the east door cannot be used and the north one is
 *                 the only way out. That is the point of walking him the length
 *                 of the room; it is not a side effect.
 *
 *   RE-ARMING     leaving the room puts him back to HAD_ABSENT at the double
 *                 door (hadads_rest), so it can happen again on the next visit
 *                 for as long as the flag window is open. `spent`, which is the
 *                 plinth role's one-shot latch, is not used by this role.
 *
 * HE ONLY JUST FITS. The corridor's drawn ceiling is y=-600 and this room's
 * floor is y=0, so his anchor is -149, his sprite centre -299 and his crown
 * -599: one unit of headroom. Anything that raises HAD_HALF_H puts his head
 * through the ceiling here before it does anywhere else.
 *
 * ======================================================================
 * THE LIBRARY DESTROYED ENCOUNTER   (HAD_ROLE_LIBRARY)
 * ======================================================================
 * A THIRD instance, seeded into STATE_LIBRARY_DESTROYED by world_seed_room. It
 * shares the body, art, damage rules, music and solid cylinder with the other
 * two and, like the West Corridor's, none of the plinth's state table.
 *
 * >>> THIS ONE IS THE ONLY INSTANCE WITH A DIRECTOR. <<< The walking is here;
 * the ESCAPE — the floating prompt on the crawl gap, the camera going under the
 * bookcase, and the handover that starts his second walk — is in
 * src/hadad_library.c, which is the encounter and not the enemy
 * (tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 1). The two talk through the four
 * functions in the marked block at the foot of this file and nothing else.
 *
 *   THE GATE      none of its own. The room only exists at all while
 *                 FLAG_HADAD_TWO is set (library_destroyed_active()), so the
 *                 flag test the other two roles need is already the price of
 *                 admission. He is HAD_ABSENT until the corner is entered.
 *
 *   THE ROOM      a U (library_destroyed.h). The EAST AISLE x[-350,350] runs the
 *                 whole z range, from the vestibule and its double door at the
 *                 north end down to z=-2080; a SPUR runs west from it at
 *                 z[-759,-309]; and the WEST AISLE x[-1780,-1299] runs south off
 *                 the spur's far end to the single door in the south wall. The
 *                 collapsed shelving fills the middle.
 *
 *   THE TRIGGER   the player reaching the north-west corner — the top of the
 *                 west aisle, just south of where the spur meets it. True radial
 *                 like the other two, so it reads the same whether they came up
 *                 the west aisle from the stairwell door or west along the spur.
 *
 *   THE FIRST WALK   he appears at the EAST end of the spur, the full width of
 *                 the room away from them, walks west to the corner and then
 *                 turns south down the west aisle to a stop in front of the
 *                 single door. TWO LEGS for the West Corridor's reason: the room
 *                 turns, and one straight goal would cut him through the corner
 *                 of the rubble block.
 *
 *                 >>> AT HALF PACE. <<< This walk alone runs at
 *                 HAD_LD_WALK1_SPEED rather than HAD_SPEED — it is the one the
 *                 player has to solve a room during, and at full speed he is
 *                 round the corner before they have worked out that the single
 *                 door has stopped answering. The second walk is at full speed.
 *
 *                 >>> THE SINGLE DOOR IS DEAD FROM THE MOMENT HE APPEARS. <<<
 *                 Not from the moment he arrives at it — hadad_library_seals_
 *                 sdoor() goes true on the appearance, the door's floating sign
 *                 goes with it, and the crawl gap's own prompt takes its place.
 *                 The player's only way out of the west aisle is under the
 *                 bookcase.
 *
 *   THE SECOND WALK   starts when the crawl ends. The director places him in
 *                 front of the single door — the place the first walk was aimed
 *                 at, so it reads as "he got there while you were under the
 *                 floor" whether he actually had time to or not — and he walks
 *                 north to the corner, east to the end of the spur, and then
 *                 down whichever arm of the east aisle the player is in. The
 *                 branch is LATCHED when he reaches the spur's east end
 *                 (had_leg_entered) and never re-read: solving it every frame
 *                 would have him pivot on the junction as the player crossed
 *                 HAD_LD_BRANCH_Z.
 *
 *                 He MARCHES both walks and roots at the end of the second. He
 *                 does not chase — where he ends up must not depend on how the
 *                 player ran, and at 600 wide in a 700-wide aisle he is a plug
 *                 either way. The double door is the only exit.
 *
 *   RE-ARMING     leaving the room (or dying in it) puts him back to HAD_ABSENT
 *                 at the appearance point, exactly as the West Corridor's does,
 *                 so the whole thing replays on the next visit. `spent` is not
 *                 used by this role either.
 *
 * HEADROOM IS FINE HERE. The drawn ceiling over the aisles is -730 against a
 * crown at -599 (see library_destroyed_init's collision_set_ceiling_y).
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

/* THREE: the Rear Gate's plinth statue, the West Corridor's ambush and the
   Library Destroyed encounter. This is a WHOLE-GAME budget and it is what the
   save's hadads_state field has to cover — two bits apiece against
   WD_MAX_HADADS (4), asserted in world.c. */
#define MAX_HADADS            3
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
     RAMPTOP  the flag-three arrival. The ramp climbs from y=0 at z=-2100 to
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

/* ---- The West Corridor ambush, in three points -----------------------------
   That room's own coordinate space (west_corridor.h): bounds x[-2700,0]
   z[-325,2000], one flat floor at y=0, west arm x[-2700,-2100] and south arm
   z[-325,325].

     START   in front of the NORTH DOUBLE DOOR, whose leaves span x[-2550,-2250]
             at z=2000, so x=-2400 is its centre and also the west arm's. Wall 7
             closes the opening at z=2000 and a 300-radius body cannot stand
             nearer than z=1700; 1650 leaves 50 over, the same clearance the
             ramp arrival takes from collision wall 39.

             >>> THE ARM IS 600 WIDE AND SO IS HE. <<< x=-2400 puts his cylinder
             exactly on both walls with nothing to spare, which is the Rear Gate
             corridor's arrangement (see the note above) and is why he plugs the
             room rather than being something to walk past.

     CORNER   where the two arms meet — the west arm's centre line crossed with
             the south arm's, which is also the trigger's centre. The turn has
             to be an authored waypoint: steering straight from START to END
             aims him through the west arm's east wall for the whole first leg.

     END      in front of the EAST SINGLE DOOR at x=0, z=0. Wall 0 stops a
             300-radius body at x=-300; -400 stands him a clear 100 off the leaf
             so the door still draws unclipped behind him. */
#define HAD_WC_START_X     (-2400)
#define HAD_WC_START_Z       1650
#define HAD_WC_CORNER_X    (-2400)
#define HAD_WC_CORNER_Z         0
#define HAD_WC_END_X        (-400)
#define HAD_WC_END_Z            0
/* The floor is y=0 and feet sit at anchor + 150, so the anchor is -GROUND_FLOOR_Y.
   Written as a literal for the reason HAD_PLINTH_ANCHOR is: this header is
   included by world.c, which does not pull in collision.h. */
#define HAD_WC_ANCHOR       (-149)
/* True radial, like HAD_TRIG_RADIUS. 400 from the corner reaches z=400 up the
   west arm and x=-2000 along the south one — a step inside either arm's mouth
   on whichever side the player arrives from, and nowhere near either door
   (both are over 1600 away, so neither transition can trip it on the way in). */
#define HAD_WC_TRIG_RADIUS    400

/* ---- The Library Destroyed encounter, in six points ------------------------
   That room's own coordinate space (library_destroyed.h and its collision file):
   bounds x[-1780,350] z[-2080,349], ONE flat floor at y=0, and a U-shaped
   walkable area — east aisle x[-350,350] over the whole z range, west spur
   z[-759,-309] running out to x=-1780, west aisle x[-1780,-1299] running south
   off the end of it. The collapsed shelving is the block in the middle.

     TRIG     the north-west corner, a step south of where the spur meets the
              west aisle. The user's point. TRUE RADIAL at 400, which reaches
              z=-434 up into the spur and x=-1129 along it, so it fires a step
              inside the corner from either approach — and both doors are over
              1200 away, so neither arrival can trip it on the way in.

     APPEAR   the EAST end of the spur, where it opens into the east aisle. The
              far side of the room from the trigger, which is the point: the
              player watches him come the whole way.

     CORNER   the spur's centre line crossed with the west aisle's. NOTE this is
              z=-548 and NOT the trigger's z=-834: the waypoint has to sit in the
              SPUR, because a leg drawn straight from APPEAR to the trigger point
              clips the rubble block's north-west corner at (-1303,-759) and he
              marches through wall geometry without noticing. The player's corner
              and his corner are the same corner; they are 286 apart because one
              is where a 195-radius player stands and the other is where a
              600-wide body turns.

     SDOOR    in front of the single door in the south wall (that door's own
              interaction point is x=-1400 z=-2015). Where the first walk ends
              and where the director puts him back for the second. Kept on the
              aisle's centre line rather than on the door's, because the aisle is
              only 481 wide and he is 600: x=-1400 would bury a third of him in
              the rubble block. His hold cylinder covers the door from here
              regardless — 495 against the door's 500 trigger radius.

     NORTH    the vestibule, in front of the double door: the branch he takes if
              the player ran for the exit.
     SOUTH    the east aisle's south end: the branch he takes if they did not.
              Both are on the aisle's centre line x=0.

     BRANCH_Z the line that separates the two. z=-309 is the spur's own north
              edge (collision wall 8), so "north of the branch line" is exactly
              "out of the spur and into the vestibule end of the aisle". */
#define HAD_LD_TRIG_X      (-1529)
#define HAD_LD_TRIG_Z       (-834)
#define HAD_LD_TRIG_RADIUS    400
#define HAD_LD_APPEAR_X       109
#define HAD_LD_APPEAR_Z      (-548)
#define HAD_LD_CORNER_X    (-1529)
#define HAD_LD_CORNER_Z      (-548)
#define HAD_LD_SDOOR_X     (-1529)
#define HAD_LD_SDOOR_Z     (-1900)
#define HAD_LD_NORTH_X          0
#define HAD_LD_NORTH_Z          0
#define HAD_LD_SOUTH_X          0
#define HAD_LD_SOUTH_Z     (-1900)
#define HAD_LD_BRANCH_Z      (-309)
/* >>> THE FIRST WALK IS AT HALF PACE. <<< At HAD_SPEED he crosses the spur and
   turns down the aisle before the player has worked out that the single door
   has stopped answering, which leaves the crawl gap's prompt as something they
   read on the way past rather than something they look for. Two is a quarter of
   a walk (camera.c: 12), so they can back off up the aisle and still lose no
   ground. The SECOND walk stays at HAD_SPEED — by then they know what the gap
   is and the pressure is the point.

   >>> AND THE STRIDE HAS TO MOVE WITH IT, IN THE OPPOSITE DIRECTION. <<< Same
   contract HAD_STEP_FRAMES documents for HAD_SPEED itself: the walk cycle is
   counted in FRAMES, so halving the speed without doubling this halves the
   ground covered per stride and he moonwalks. What stays constant is the
   DISTANCE per animation frame — 4 x 39 = 156 against 2 x 78 = 156, exactly.
   Retune one and recompute the other from that product. */
#define HAD_LD_WALK1_SPEED         2
#define HAD_LD_WALK1_STEP_FRAMES  78
/* The floor is y=0 and feet sit at anchor + 150, so the anchor is
   -GROUND_FLOOR_Y. A literal for HAD_WC_ANCHOR's reason: world.c includes this
   header and not collision.h. */
#define HAD_LD_ANCHOR       (-149)

/* ---- The two triggers ------------------------------------------------------
   FLAG ONE arms on proximity to the two grinders, which sit at z=-85 in the
   corridor's side hedges (grinder_puzzle.c's GP_Z). The radius is TRUE RADIAL,
   so 300 means 300 from whichever way the player walks in. It is sized against
   the one other thing in the corridor worth pressing: the lever is at z=300,
   which is 385 away, so throwing it does not arm Hadad by accident. It is also
   comfortably wider than the lane — the player is held to |x| <= 105 by the
   hedges, so anybody walking the corridor at all passes within 300 of the
   grinders and cannot dodge past on one side.

   FLAG THREE is a two-part latch, because "steps into the corridor FROM the
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
   and the pose he goes back to if flag three burns out. A Hadad who has ARRIVED
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

/* WHICH SCRIPT this instance runs. Fixed at hadad_add() time and never changed
   by anything — had_reseat() preserves it across every reset. Everything the
   two share (body, art, damage, music, the solid cylinder, the steering) is
   role-blind; everything that differs tests this explicitly. */
typedef enum {
    HAD_ROLE_PLINTH,      /* Rear Gate: the statue and its two-flag table   */
    HAD_ROLE_WEST_CORR,   /* West Corridor: the two-leg corner ambush       */
    HAD_ROLE_LIBRARY,     /* Library Destroyed: two walks round a U, with
                             the crawl-gap escape in between — the only
                             role with a director (src/hadad_library.c)    */
} HadadRole;

typedef enum {
    HAD_IDLE,     /* on the plinth: inert, solid, drawn, INVULNERABLE.
                     HAD_ROLE_PLINTH only — a West Corridor Hadad is never
                     in this state, because he has no plinth to stand on  */
    HAD_ABSENT,   /* not in the room: not drawn, not solid, not damageable.
                     PLINTH — flag three, before the ramp trigger.
                     WEST_CORR — the resting state, before the corner
                     trigger and outside the flag window entirely       */
    HAD_WALK,     /* moving — a scripted path, or after the player.
                     `follow` says which, and `leg` says where on the path */
    HAD_ROOTED,   /* the path is walked out: parked on its last point for
                     the rest of the visit                                */
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

    int         follow;         /* 1 = chase the player (flag three); 0 = walk
                                   this role's scripted path                 */
    int         leg;            /* index into that path while follow == 0 —
                                   see had_path_goal() in hadad.c            */
    int         stage;          /* HAD_ROLE_LIBRARY only: 0 = the walk to the
                                   single door, 1 = the walk back round the U
                                   after the crawl. `leg` restarts at 0 for
                                   each, so the pair is the whole cursor      */
    int         branch;         /* HAD_ROLE_LIBRARY stage 1 only: which arm of
                                   the east aisle he took. 0 = not decided
                                   yet, 1 = north (the vestibule), 2 = south.
                                   Latched once, at the spur's east end       */
    int         spent;          /* the flag-three encounter has been used up.
                                   HAD_ROLE_PLINTH only. Saved — see
                                   WorldDelta.hadads_state                   */
    int         ramp_armed;     /* flag three's latch: the player has been up
                                   the ramp. Per-visit, not saved            */
    HadadRole   role;
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
   full.

   `role` picks the script AND the state he is placed in: HAD_ROLE_PLINTH lands
   in HAD_IDLE (a statue, on show from the first frame), HAD_ROLE_WEST_CORR in
   HAD_ABSENT (nothing in the room until the corner is entered). Get that pair
   the wrong way round and a West Corridor Hadad stands in the middle of the
   passage as inert, invulnerable masonry. */
int  hadad_add(int32_t x, int32_t z, int32_t y, GameState area, HadadRole role);

void hadads_init(void);
void hadads_reset(void);
/* Put him back where this visit should START him, which is the one thing about
   this enemy that is not "back at his spawn": it depends on the flags. Called
   when leaving a room and when saving. Deaths stick, and so does `spent` —
   which this is also the function that SETS, because leaving the room mid
   flag-three encounter is what burns it out. */
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

/* ---------------------------------------------------------------------------
 * THE DIRECTOR API   (src/hadad_library.c and nothing else)
 * ---------------------------------------------------------------------------
 * Three functions, and they are the whole conversation between the enemy and
 * the Library Destroyed encounter — see STEP 1 of
 * tools/ADDING_A_BOSS_ENCOUNTER.txt for why the split is not cosmetic. The
 * director never touches a Hadad field directly and never reaches into the path
 * tables; this file never knows a camera exists.
 */

/* The live Library Destroyed Hadad, or NULL — not placed, not in this area, or
   dead. For ARMING only: latch the answer, do not re-ask it every frame
   (ADDING_A_BOSS_ENCOUNTER.txt STEP 4). */
Hadad *hadad_library_instance(void);

/* 1 once he is actually IN the room — walking or rooted, either stage. This is
   what kills the single door and raises the crawl gap's prompt, and it is
   deliberately the APPEARANCE and not the arrival. */
int  hadad_library_present(void);

/* The crawl is over: put him in front of the single door and start his second
   walk from the top. Idempotent in the sense that it always restarts stage 1 —
   the director calls it exactly once, on the frame control comes back. */
void hadad_library_begin_return(Hadad *h);

#endif
