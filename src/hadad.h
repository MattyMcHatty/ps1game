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
 *                         lever WORKS again — ONCE: the throw breaks it (see
 *                         THE THROW HAS TO CATCH HIM below). Hadad is not in
 *                         the room at all to begin with (HAD_ABSENT: not
 *                         drawn, not solid, not damageable). Walking down the
 *                         ramp and back up into the corridor makes him appear
 *                         at the TOP of the ramp, behind the player.
 *                         >>> AND THEN HE MARCHES BEFORE HE CHASES. <<< From
 *                         the ramp top he walks ONE AUTHORED LEG, straight up
 *                         the room's centre line, the whole length of the room
 *                         to HAD_CLIMB — out of the corridor's north mouth and
 *                         a little way onto the lawn. Only when he gets there
 *                         does `follow` come on and the pursuit begin.
 *                         He used to chase from the moment he appeared, and a
 *                         player who did not run straight up the corridor could
 *                         leave him wedged in the ramp's rails or in one of the
 *                         courts' dead ends, with the one thing that can kill
 *                         him a room away. The climb is a MARCH
 *                         (had_path_is_open): no feeler, no wall-follow and no
 *                         collision push, so none of that geometry can touch
 *                         him, and neither can the closing plates.
 *                         >>> AND THE MARCH IS WHEN HE IS KILLABLE. <<< It goes
 *                         straight through the grinders, so the player's window
 *                         is throwing the lever while he is between the plates
 *                         on his way past. He crosses the band in about three
 *                         seconds at HAD_SPEED, and the pair being wide open at
 *                         that moment is fine: the scene watches them shut on
 *                         him.
 *
 *                         >>> THE THROW HAS TO CATCH HIM, AND IT ONLY COMES
 *                         ONCE. <<< The verdict is taken on the ONE frame the
 *                         switch goes over (grinder_puzzle.c, THE THREE ANSWERS
 *                         TO THE LEVER) and the lever breaks on that same frame
 *                         while this flag is up. Three things can happen:
 *                           - he is in the band: the plates have him, and the
 *                             death scene plays.
 *                           - he is SOUTH of it, still on his way up: he marches
 *                             on and LEAPS the machine (see THE LEAP OVER THE
 *                             GRINDERS below), landing north of it and finishing
 *                             the climb.
 *                           - he is NORTH of it, already past: nothing happens
 *                             at all and he walks on into the pursuit.
 *                         Closed plates do NOT kill him by standing there. They
 *                         used to, which meant a lever thrown a minute early
 *                         still collected him when he eventually walked into
 *                         them — a kill nobody had timed and nobody had watched.
 *                         From the lawn on he follows them anywhere they go.
 *                         Leaving the room ENDS him: `spent`
 *                         is set, and on the next visit he is back on the
 *                         plinth in HAD_IDLE and can never be armed again by
 *                         anything.
 *                         >>> AND THIS IS THE STATE HE CAN BE KILLED IN. <<<
 *                         The player's job is to time the lever against his
 *                         march up the corridor. Doing it plays the HADAD DEATH
 *                         SCENE (src/hadad_grinder.c) — see DAMAGE below.
 *
 * >>> WHAT ARMS FLAG THREE <<< is not in this room at all: it is the player
 * LEAVING THE WEST CORRIDOR by its north double door — the one that comes back
 * out to the top of this room's ramp — with FLAGS ONE AND TWO BOTH ALREADY SET.
 * The set lives in src/main.c's STATE_WEST_CORRIDOR ndoor branch, and it is the
 * flag's ONLY trigger outside the debug menu. So the third encounter is always
 * armed on the walk that leads straight into it: the player comes out of that
 * door onto the ramp with him waiting to appear behind them.
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
 * when it is thrown he is killed outright. That is the intended answer to a
 * hundred-swing health bar.
 *
 * >>> AND IT IS A SCENE, NOT A HIT. <<< grinder_puzzle.c asks
 * hadads_grinder_caught() who the plates have and hands him to
 * src/hadad_grinder.c, the HADAD DEATH SCENE: the camera is taken, the plates
 * finish their travel with him squashed to the gap between them (the `squash`
 * field below), he roars, and his health is only then emptied — through
 * hadad_damage, so the grey burst and the cue are the ordinary ones. Read that
 * file's header for the beat sheet. Nothing in THIS file knows the scene
 * exists, which is the point of the split.
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
 *   THE GATE      FLAG_HADAD_ONE set, AND FLAG_HADAD_THREE AND FLAG_HADAD_TWO
 *                 BOTH clear. The first half is read the same way round as
 *                 hadad_lever_locked(): flag three REPLACES flag one everywhere
 *                 in this enemy, so once the third Rear Gate encounter is armed
 *                 this ambush is over for good. >>> FLAG TWO CLOSES IT TOO, AND
 *                 THAT IS WHAT HANDS THE ROOM OVER TO THE RETURN. <<< The stones
 *                 coming back out of the Attic Exit's door arms the SECOND West
 *                 Corridor encounter (THE WEST CORRIDOR RETURN, below), which
 *                 walks the same L the other way round. The two are separate
 *                 instances sharing one 600-wide corridor, so the gates have to
 *                 be mutually exclusive or they meet head-on in it. Outside that
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
 * THE WEST CORRIDOR RETURN   (HAD_ROLE_WEST_CORR_RET)
 * ======================================================================
 * A FIFTH instance, seeded into STATE_WEST_CORRIDOR alongside the ambush by
 * world_seed_room. >>> IT IS THE AMBUSH RUN BACKWARDS, AND NOTHING ELSE. <<<
 * Same room, same three authored points, same trigger circle, same march, same
 * two legs — walked from the far end to the near one instead of the near end to
 * the far one. Everything in THE WEST CORRIDOR AMBUSH above that is not a
 * compass direction applies here word for word and is not repeated.
 *
 *   THE GATE      FLAG_HADAD_TWO, on its own. The stones are back out of the
 *                 Attic Exit's door, the Library has collapsed and the Reception
 *                 is sealed — the same beat that puts a Hadad through the
 *                 Reception's ceiling — and this corridor is the way into that
 *                 room. Nothing closes it again: unlike the ambush's flag-three
 *                 half there is no later flag that supersedes it, so it re-arms
 *                 on every visit for the rest of the game.
 *
 *                 >>> IT IS A SEPARATE INSTANCE AND NOT A SECOND SCRIPT ON THE
 *                 FIRST ONE. <<< The two share nothing but the room. A player
 *                 who spent a hundred crucifaxe swings killing the ambush has
 *                 killed the ambush, and `dead` is saved per instance, so
 *                 reusing that slot would silently cancel this encounter for
 *                 them. It is also what forced WD_MAX_HADADS wider and
 *                 SAVE_VERSION to 17 (world.h, savegame.h).
 *
 *   THE TRIGGER   the corner where the corridor turns, and it is the AMBUSH'S
 *                 OWN circle — HAD_WC_CORNER_X/Z at HAD_WC_TRIG_RADIUS, true
 *                 radial, so it reads the same down either arm. Deliberately not
 *                 a circle of its own: the two encounters can never be armed at
 *                 the same time (see THE GATE above), so there is nothing for a
 *                 second one to disambiguate and one number that moves is better
 *                 than two that have to be kept level.
 *
 *   THE WALK      he appears at the EAST end, in front of the SINGLE door — the
 *                 one into the Reception, HAD_WC_END — walks the south arm west
 *                 to the corner, turns, and walks the west arm north to a stop
 *                 at HAD_WC_RET_END. The ambush's leg table with its two
 *                 entries swapped, and had_path_goal() in hadad.c is the whole
 *                 of it.
 *
 *                 >>> HE STOPS SHORT OF THE DOUBLE DOOR, IN FRONT OF THE FAT
 *                 DOOR'S GAP. <<< The one asymmetry with the ambush: it walks
 *                 its last leg all the way onto the far door, this one halts
 *                 450 south of it, level with the 300-wide opening in the west
 *                 arm's east wall (west_corridor.h; the breakable leaf is at
 *                 z=1200). That is z=1200 rather than HAD_WC_START's 1650 — see
 *                 HAD_WC_RET_END_Z for the numbers.
 *
 *                 >>> HE STILL BLOCKS THE DOUBLE DOOR, AND THE SINGLE ONE IS
 *                 THE ONLY WAY OUT. <<< The mirror of the ambush's last
 *                 paragraph and the point of the encounter. His hold cylinder
 *                 is 495 and the north door answers within
 *                 WCDOOR_TRIGGER_RADIUS (500) of (-2400, 2000): rooted at
 *                 z=1200 he keeps the player at z=705 or south of it, 1295 off
 *                 the leaf, so the way back out to the Rear Gate is shut and
 *                 the player is herded into the Reception — where the ceiling
 *                 drops on them.
 *
 *   RE-ARMING     the ambush's, unchanged: leaving the room puts him back to
 *                 HAD_ABSENT on his appearance point (hadads_rest, which tests
 *                 only for HAD_ROLE_PLINTH), so it replays on the next visit.
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
 * THE RECEPTION ENCOUNTER   (HAD_ROLE_RECEPTION)
 * ======================================================================
 * A FOURTH instance, seeded into STATE_RECEPTION by world_seed_room. It shares
 * the body, art, damage rules, music and solid cylinder with the other three
 * and, like the two before it, none of the plinth's state table.
 *
 * >>> IT IS THE SECOND INSTANCE WITH A DIRECTOR, AND THE FIRST WITH A TRIGGER
 * THAT IS NOT ITS OWN. <<< The walking is here; the ENTRY QUAKE and the trigger
 * that starts the walk both live in src/reception_hadad.c, because both are
 * things that happen to the PLAYER rather than properties of the enemy
 * (tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 1). This role therefore has no
 * self-trigger in the HAD_ABSENT branch at all: it waits to be begun.
 *
 *   THE GATE      FLAG_HADAD_TWO (the room is sealed) and FLAG_RECEPTION_HADAD
 *                 clear (it has not run yet). Both are the DIRECTOR's business
 *                 - the flag is set at the arm, on entry, so the encounter is
 *                 one-shot and cannot half-happen.
 *
 *   THE ROOM      two storeys (reception.c's floor zones). The GROUND floor is
 *                 the whole 3000 x 3000 box at y=0 less the stair mass
 *                 x[-100,700] z[-700,500]; the SECOND LEVEL is an L at y=-600 -
 *                 a north band x[-900,1500] z[500,1500] and a west strip
 *                 x[-1500,-900] running the full depth. The STAIR ("the ramp")
 *                 climbs in two flights from the ground at x=-100 z[-700,-300],
 *                 across a landing at y=-150, and up to the band's south edge at
 *                 z=500, x[300,700].
 *
 *                 The ceiling over the west strip is y=-1200 (Reception v2.smx:
 *                 the vault steps -1200 / -1350 / -1500 / -1600 inward), and a
 *                 Hadad standing on the second level reaches -1199. He clears it
 *                 by one unit, exactly as he does the West Corridor's.
 *
 *   THE ARRIVAL   he appears IN THE CEILING in front of the second level's
 *                 north-west door - the one into the West Corridor, which is the
 *                 room's only way on - with his feet on the -1200 plane, and
 *                 DROPS. Nothing else about him is different: the fall is
 *                 apply_ddog_height's ordinary gravity, and the only special
 *                 case is that he does not walk while it happens (see the
 *                 `vy` gate in update_hadads).
 *
 *                 >>> NO WAKE PAUSE. <<< Every other arrival stands still for
 *                 HAD_WAKE_PAUSE first; this one does not, because the DROP is
 *                 the beat that announces him and a second of hanging in the
 *                 plaster before it would read as a stuck sprite.
 *
 *   THE WALK      four STAGES, and `stage` is the cursor through them:
 *
 *                 0  the drop, then one leg east to the head of the stair.
 *                    At the end of it the SCENARIO is latched (had_leg_entered)
 *                    off where the player is: down the stair, or still up top.
 *                 1  SCENARIO 1 - they went down. He follows the stair: south
 *                    down the upper flight and across the landing, then west
 *                    down the lower flight and out onto the ground floor.
 *                 2  SCENARIO 2 - they backed off toward the East Hall door. He
 *                    follows them that way, STANDS FOR A SECOND
 *                    (HAD_RC_EDGE_PAUSE), then walks south off the second
 *                    level's south edge and drops to the ground floor; south
 *                    past the Kitchen Dining door into the room's south-east
 *                    corner; a square 90-degree turn west to the point directly
 *                    south of the stair; and north-west to the stair's foot.
 *
 *                    >>> HE STEPS OFF AT x=1100 BECAUSE THAT IS THE ONE PLACE
 *                    HE CAN. <<< The second level's edge band along z=500 is
 *                    modelled for x[-900,300], x[700,966] and x[1233,1500]
 *                    (Reception v2.smx) and the collision follows it - walls 12,
 *                    27 and 28, all y[-719,-600]. The stretch x[966,1233] has no
 *                    band and no wall: it is the only unguarded edge on the
 *                    floor, and 1100 is its middle.
 *
 *                    BOTH SCENARIOS END ON THE SAME POINT, HAD_RC_FOOT, which is
 *                    the ground floor just west of the stair's bottom step.
 *                 3  THE CIRCUIT, and it never ends. Seven legs, and leg 7
 *                    wraps back to leg 0: down and round the stair to the
 *                    KITCHEN DINING door; a LEAP from it up onto the second
 *                    level; west along the balcony to the head of the stair;
 *                    and back down the stair to HAD_RC_FOOT, where it starts
 *                    again. He is never HAD_ROOTED in this room at all — there
 *                    is no state after this one, and the stalker track plays
 *                    for as long as the player stays.
 *
 *                    >>> THE LEAP IS THE ONLY BALLISTIC MOVE IN THIS ENEMY.
 *                    <<< The second level does not reach the Kitchen Dining
 *                    door — it stops at z=500 and the door is at z=-414 — so
 *                    "jump up to the second level" is 975 units of ground as
 *                    well as 600 of height. He gets an upward `vy` kick and a
 *                    leg speed of his own, and apply_ddog_height's ordinary
 *                    gravity draws the arc; see HAD_RC_JUMP_VY for the
 *                    arithmetic that makes him clear the edge on the way over
 *                    rather than fall through it.
 *
 *                 He MARCHES every leg. He does not chase, and nothing about
 *                 the circuit depends on where the player is — the only
 *                 run-time decision left in the whole role is the scenario
 *                 latch at the head of the stair.
 *
 *   RE-ARMING     none. FLAG_RECEPTION_HADAD is set on the entry that arms it,
 *                 so a later visit finds the room empty - the director never
 *                 begins him again, and with no self-trigger he stays
 *                 HAD_ABSENT for ever. hadads_rest() still reseats him like the
 *                 other scripted roles; there is simply nothing left to start
 *                 it.
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

/* FIVE: the Reception's ceiling drop, the Rear Gate's plinth statue, the West
   Corridor's ambush, the West Corridor's return and the Library Destroyed
   encounter — in that order, which is room_areas[] order (and, for the two that
   share the West Corridor, world_seed_room's order within that room) and
   therefore the order the save's hadads_state indexes them in (world.c). This is
   a WHOLE-GAME budget and it is what that field has to cover — two bits apiece
   against WD_MAX_HADADS, asserted in world.c.

   >>> THE FIFTH IS WHAT WIDENED THE SAVE. <<< hadads_state was a uint8_t, which
   at two bits apiece holds exactly four; the West Corridor return made it a
   uint16_t and WD_MAX_HADADS 8, which is a WorldDelta change and therefore the
   SAVE_VERSION 17 bump (savegame.h). There are three spare placements in it
   now; the ninth is the next one that costs a bump. */
#define MAX_HADADS            5
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

/* Where flag three's march ENDS and the pursuit begins: the whole length of the
   room, out past the top of the corridor and one body's depth onto the lawn.

   z = 1600 is 100 north of HAD_MOUTH_Z, which is the corridor's north mouth
   itself — so he is CLEAR of the hedged tube when he is handed over rather than
   standing in its opening, and the pursuit's first steering decision is made on
   open grass instead of in a gap exactly his own width. x = 0 is the room's
   centre line, which the whole march runs down; both ends being on it is what
   lets the climb be a single leg. */
#define HAD_CLIMB_X             0
#define HAD_CLIMB_Z          1600

/* ---- THE LEAP OVER THE GRINDERS --------------------------------------------
   The second of the three answers the lever can give during the climb: the
   plates start closing while he is still SOUTH of them, so he clears the
   machine instead of walking into it. (Walking into shut plates and dying of it
   is what this replaced, and it made no sense — the lever has to be thrown
   WITH him in the band, not merely at some point before he arrives.)

   >>> IT IS THE RECEPTION CIRCUIT'S LEAP, RE-AIMED. <<< Same physics and the
   same two knobs: `vy` is kicked upward as he takes off and apply_ddog_height's
   own gravity (GRAVITY 1 a frame) draws the arc, so there is no second
   integrator anywhere in this enemy. What differs is that this one lands on the
   SAME floor it left, which makes both numbers fall out of the machine's size
   rather than out of a balcony's edge.

     TAKE-OFF  the plates' strip is z[-285,115] (grinder_puzzle.c's
               GP_CRUSH_Z_MIN/MAX) and he is HAD_BODY_RADIUS deep, so -585 puts
               his leading edge exactly on the strip's south face as his feet
               leave the ground. Any nearer and he would rise THROUGH a plate;
               any further back and the jump starts in open corridor for no
               reason. It also has to sit at or south of the kill band's own
               south edge, or a Hadad in the gap between the two would be told
               to leap from a spot he had already walked past — grinder_puzzle.c
               derives that edge from the same strip and the same leeway.

     HEIGHT    a kick of m rises m(m-1)/2 by frame m, so 31 peaks at 465 — just
               over the grinders' own 450 (GRINDER_SOLID_H), which is what makes
               it read as clearing the machine rather than passing through it.
               Below 20 the descent never reaches MAX_FALL_VEL, so the airtime
               is 2m-1; at 31 the cap costs a few frames back and the whole arc
               runs about 64.

     SPEED     the ground to cover is the 400-deep strip plus his own depth at
               each end, i.e. 1000, and 16 a frame across those 64 frames is
               1024 of it — he lands a little past z=439, clear of the plates
               with his whole body. Retune the kick and this must move with it.

     STRIDE    16 x 10 = 160 against the enemy's standing 4 x 39 = 156, the
               product HAD_STEP_FRAMES exists to hold. He is airborne for nearly
               all of it so the cycle barely shows; matched anyway, because the
               pair must never be picked apart later.

   He is NOT killable while he does this, for the same reason he is not killable
   at any other moment now: the plates only ever answer the frame the lever is
   thrown. */
#define HAD_VAULT_Z          (-585)
#define HAD_VAULT_VY          (-31)
#define HAD_VAULT_SPEED         16
#define HAD_VAULT_STEP_FRAMES   10

/* `vault` — see the field in the struct. */
#define HAD_VAULT_PENDING        1
#define HAD_VAULT_AIRBORNE       2

/* `stage` for the plinth role, which is the one role with TWO encounters in one
   room. 0 is flag one's walk (the corridor's north mouth south to HAD_END) and
   is what a zero-filled Hadad already is, so it needs no name. 1 is flag three's
   CLIMB: the ramp top north to HAD_CLIMB, marched, ending in the handover to
   the pursuit. Latched at the arrival; read by had_path_goal, had_path_is_open
   and had_leg_entered. */
#define HAD_PLINTH_STAGE_CLIMB  1

/* ---- The West Corridor's TWO encounters, in three points -------------------
   >>> ONE SET OF POINTS VERY NEARLY SERVES BOTH. <<< The ambush walks
   START -> CORNER -> END and the return (HAD_ROLE_WEST_CORR_RET, under
   FLAG_HADAD_TWO) walks END -> CORNER -> RET_END, so every note below about a
   point being clear of a door leaf, of a wall, or of the arm it stands in holds
   whichever direction the walk is going: the ambush's stop point is the
   return's appearance point. The one point that is not shared is where the
   return STOPS — it halts at the fat door's gap rather than carrying on to
   START, and RET_END is that spot. The trigger circle IS shared outright: the
   two are never armed at the same time.

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
             room rather than being something to walk past. The RETURN heads
             this way too but stops 450 short of it at HAD_WC_RET_END, so this
             point belongs to the ambush alone.

     CORNER   where the two arms meet — the west arm's centre line crossed with
             the south arm's, which is also the trigger's centre. The turn has
             to be an authored waypoint: steering straight from START to END
             aims him through the west arm's east wall for the whole first leg.

     END      in front of the EAST SINGLE DOOR at x=0, z=0. Wall 0 stops a
             300-radius body at x=-300; -400 stands him a clear 100 off the leaf
             so the door still draws unclipped behind him. It is also where the
             RETURN appears — at the player's back, if they came in through that
             door. */
#define HAD_WC_START_X     (-2400)
#define HAD_WC_START_Z       1650
#define HAD_WC_CORNER_X    (-2400)
#define HAD_WC_CORNER_Z         0
#define HAD_WC_END_X        (-400)
#define HAD_WC_END_Z            0
/* RET_END — where the RETURN stops, and the one point of that walk that is NOT
   simply the ambush's read backwards. He stops SHORT of the double door, in
   front of the FAT DOOR'S GAP: the 300-wide opening in the west arm's east wall
   at x[-2100,-2044] z[1050,1350] (west_corridor.h), whose centre z is 1200 and
   whose breakable leaf fatdoors_init() puts at (-2072, 1200). X stays on the
   west arm's centre line because that is the only x a 600-wide body has in a
   600-wide arm, so "in front of the gap" is a Z and nothing else.

   >>> STOPPING SHORT DOES NOT COST HIM THE DOOR. <<< The double door answers
   within WCDOOR_TRIGGER_RADIUS (500) of (-2400, 2000) and his hold cylinder is
   495, so rooted here he keeps the player at z <= 705 — 1295 off the leaf,
   further out than the 845 the old stop at HAD_WC_START gave. The way back to
   the Rear Gate is shut either way and the east door is still the only exit.

   IT DOES BLOCK THE GAP, and that follows from where he is asked to stand: the
   opening's own mouth is 300 from him, well inside the 495 hold, so while he is
   rooted the inner room cannot be entered. That room is seeded empty, so
   nothing is lost behind him today. */
#define HAD_WC_RET_END_X   (-2400)
#define HAD_WC_RET_END_Z     1200
/* The floor is y=0 and feet sit at anchor + 150, so the anchor is -GROUND_FLOOR_Y.
   Written as a literal for the reason HAD_PLINTH_ANCHOR is: this header is
   included by world.c, which does not pull in collision.h. */
#define HAD_WC_ANCHOR       (-149)
/* True radial, like HAD_TRIG_RADIUS. 400 from the corner reaches z=400 up the
   west arm and x=-2000 along the south one — a step inside either arm's mouth
   on whichever side the player arrives from, and nowhere near either door
   (both are over 1600 away, so neither transition can trip it on the way in).
   BOTH West Corridor encounters fire on this one circle; their gates are
   mutually exclusive, so it can only ever start one of them. */
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

/* ---- The Reception encounter, in ten points --------------------------------
   That room's own coordinate space (reception.c's floor zones and
   src/reception_mesh_collision.c): bounds x[-1500,1500] z[-1500,1500], TWO
   storeys. Ground floor y=0 over the whole box; SECOND LEVEL y=-600 as an L —
   north band x[-900,1500] z[500,1500] plus west strip x[-1500,-900] over the
   full depth. The stair climbs x[-100,300] z[-700,-300] (lower flight, y 0 to
   -150), across the landing x[300,700] z[-700,-300] (y=-150), then x[300,700]
   z[-300,500] (upper flight, y -150 to -600) onto the band's south edge.

   The two ANCHORS below are literals for HAD_WC_ANCHOR's reason — world.c
   includes this header and not collision.h — and each is (floor - 149).

     CEIL     in front of the second level's north-west door (reception.c's
              NDOOR, x=-1435 z=964), which is the room's only way on. x=-1200
              is the west strip's centre line and the only x a 600-wide body
              fits on: the strip is x[-1500,-900], exactly 600 across, so he
              plugs it the way he plugs the West Corridor. The CEILING there is
              y=-1200 (Reception v2.smx), and HAD_RC_CEIL_ANCHOR puts his FEET
              on that plane — feet sit at anchor + 150 — so he starts flush in
              the plaster and falls 601 units onto the floor below.

     STAIRTOP the second level's floor 120 north of the stair head, on the
              flight's own centre line (x[300,700] -> 500). The one leg from
              CEIL to here is a single diagonal and stays on walkable floor the
              whole way: every point on it has z >= 620, and the strip and the
              band are the same height with no wall between them north of
              z=500.

     LANDING  scenario 1's turn, at the foot of the upper flight and the middle
              of the landing. x=500 is both flights' shared column and z=-500 is
              the landing's centre, so the leg STAIRTOP->LANDING runs straight
              down the upper flight and the next one straight down the lower.

     FOLLOW   scenario 2's first leg: out toward the East Hall's double door
              (x=1435 z=1071), which is where the player has just backed off to.
              He stops short of it and stands for HAD_RC_EDGE_PAUSE.

     EDGE     the step-off, and z=300 is deliberately PAST the edge at z=500:
              the leg is a walk-off, not a hover, so its goal has to sit on the
              ground floor below. x=1100 is the middle of the only unguarded
              stretch of that edge — see the note in the header block above.

     CORNER   the room's south-east corner, a body radius clear of both walls,
              reached by walking south past the Kitchen Dining door (x=1450
              z=-414). Its Z is shared with SOUTH so the turn there is a square
              90 degrees, which is what the brief asks for.

     SOUTH    directly south of the stair: x=100 is the lower flight's own
              centre (x[-100,300]).

     FOOT     in front of the stair, on the ground floor just west of the bottom
              step. BOTH SCENARIOS END HERE. The bottom step is at x=-100 and a
              300-radius body clears it at -400, so -500 stands him a clear 100
              off it; z=-500 is the flight's centre line.

     RDOOR    in front of the Kitchen Dining double door (its leaf is at x=1450
              z=-414), a body radius plus a margin off it. It does not open —
              it is rubble while the room is sealed (reception.h) — which is
              the point of walking the player's eye to it. It is also where the
              LEAP starts.

     JUMP     where the leap lands: the second level, straight through the same
              unguarded stretch of its edge that scenario 2 stepped off
              (x[966,1233] — see the header block). 1100 is that stretch's
              middle and 560 is 60 clear of the edge, so he comes down on floor
              rather than on the lip. */
#define HAD_RC_CEIL_X       (-1200)
#define HAD_RC_CEIL_Z          964
#define HAD_RC_CEIL_ANCHOR  (-1350)   /* feet on the -1200 ceiling plane   */
#define HAD_RC_UPPER_ANCHOR  (-749)   /* second level y=-600               */
#define HAD_RC_ANCHOR        (-149)   /* ground floor y=0                  */
#define HAD_RC_STAIRTOP_X      500
#define HAD_RC_STAIRTOP_Z      620
#define HAD_RC_LANDING_X       500
#define HAD_RC_LANDING_Z     (-500)
#define HAD_RC_FOLLOW_X       1100
#define HAD_RC_FOLLOW_Z        900
#define HAD_RC_EDGE_X         1100
#define HAD_RC_EDGE_Z          300
#define HAD_RC_CORNER_X       1150
#define HAD_RC_CORNER_Z     (-1150)
#define HAD_RC_SOUTH_X         100
#define HAD_RC_SOUTH_Z      (-1150)
#define HAD_RC_FOOT_X        (-500)
#define HAD_RC_FOOT_Z        (-500)
#define HAD_RC_RDOOR_X        1150
#define HAD_RC_RDOOR_Z       (-414)
#define HAD_RC_JUMP_X         1100
#define HAD_RC_JUMP_Z          560

/* >>> HE STANDS FOR A SECOND BEFORE HE STEPS OFF. <<< Scenario 2's one pause,
   and the only thing in this role that is timed rather than positional. It is
   served through `pause_timer`, the same field HAD_WAKE_PAUSE uses, so he holds
   the IDLE pose through it exactly as an arriving Hadad does — which is what
   makes it read as him giving up on the chase rather than as a stall. */
#define HAD_RC_EDGE_PAUSE       60   /* 1 s at 60 fps */

/* The role's ONE run-time decision, read ONCE by had_leg_entered when he
   reaches the head of the stair and never revisited — the Library Destroyed's
   rule, and for its reason: re-solved every frame the goal would swing from one
   end of the room to the other as the player crossed the line, and he would
   pivot on the waypoint instead of committing.

   "The player has gone down the stair", measured on the player's EYE
   (player_y()) and not on a floor zone, because the stair is a FLOOR_RAMP and
   the mid-landing is a FLOOR_UPPER — so player_on_upper_floor is TRUE on the
   landing and useless here. The second level stands the eye at -789 and the
   ground floor at -189; -700 is a shade below the top of the flight, so it
   takes a real step or two down before it reads as a descent. */
#define HAD_RC_DESCEND_Y     (-700)

/* ---- THE LEAP ---------------------------------------------------------------
   HAD_RC_RDOOR (1150, -414, ground, anchor -149) to HAD_RC_JUMP (1100, 560,
   second level, anchor -749): 975 units of ground and 600 of height. `vy` is
   kicked upward as the leg is taken up and apply_ddog_height's own gravity
   (GRAVITY 1 a frame) does the rest, so the arc is the SAME physics that drops
   him out of the ceiling — there is no second integrator.

   >>> HE HAS TO BE ABOVE THE SECOND LEVEL WHEN HE CROSSES ITS EDGE, OR HE FALLS
   STRAIGHT BACK TO THE GROUND. <<< apply_ddog_height skips any floor zone that
   is ABOVE the body (`zone_target < *py - 2`), so an arc that reaches z=500
   still climbing past -749 lands on the balcony and one that reaches it at -700
   sails over the top of the zone and drops to y=0 on the far side. That single
   inequality is what sets both numbers below.

     RISE     with a kick of -m the rise after n frames is m*n - n(n+1)/2, so
              the peak is m(m-1)/2 at n = m. At m=41 that is 820, reached
              around frame 40, and the body is at or past the needed 600
              between frames 18 and 61 — a 43-frame window.

     SPEED    the leg is 975 long and the edge at z=500 is 914 of it, so at 16 a
              frame he crosses the edge on frame 57 (rise 684, i.e. 84 clear of
              the floor he is about to land on) and arrives over the landing
              point on frame 60 with vy already back to +19. He touches down a
              frame or two later, hard — which is what the landing rumble is
              for. Retune either number and re-check frame 57 against the 600.

     CEILING  the peak puts his crown at -1419 against a drawn vault of -1200,
              so the top of him passes THROUGH the ceiling for about twenty
              frames. That is invisible and not a bug: the camera is underneath
              it, and the ceiling polys occlude him exactly as the floor does.

     STRIDE   16 x 10 = 160 against the enemy's standing 4 x 39 = 156, which is
              the product HAD_STEP_FRAMES exists to hold. He is airborne for
              nearly all of it, so the cycle barely shows; it is matched anyway
              so the pair can never be picked apart later. */
#define HAD_RC_JUMP_VY        (-41)
#define HAD_RC_JUMP_SPEED       16
#define HAD_RC_JUMP_STEP_FRAMES 10

/* A LANDING SOUNDS. The downward `vy` that counts as "he fell" rather than "he
   walked down a stair tread": a real drop is capped at MAX_FALL_VEL (20) by the
   time it lands, while walking down either flight of the stair holds `vy` at 1
   to 3 — the ramp's surface only falls about 1.5 units a frame under him. 8
   separates the two with room either side and is not a tuning knob. */
#define HAD_RC_LAND_VY           8

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
    HAD_ROLE_WEST_CORR_RET, /* West Corridor again, under FLAG_HADAD_TWO:
                             the same two legs walked the other way round,
                             blocking the double door instead of the single
                             one. A SEPARATE instance from the ambush — see
                             THE WEST CORRIDOR RETURN above               */
    HAD_ROLE_LIBRARY,     /* Library Destroyed: two walks round a U, with
                             the crawl-gap escape in between — the first
                             role with a director (src/hadad_library.c)    */
    HAD_ROLE_RECEPTION,   /* Reception: a drop through the ceiling and a
                             four-stage walk down two storeys. Its trigger
                             AND its entry quake belong to its director
                             (src/reception_hadad.c), so this role has no
                             self-trigger of its own at all               */
} HadadRole;

typedef enum {
    HAD_IDLE,     /* on the plinth: inert, solid, drawn, INVULNERABLE.
                     HAD_ROLE_PLINTH only — a West Corridor Hadad is never
                     in this state, because he has no plinth to stand on  */
    HAD_ABSENT,   /* not in the room: not drawn, not solid, not damageable.
                     PLINTH — flag three, before the ramp trigger.
                     WEST_CORR and WEST_CORR_RET — the resting state, before
                     the corner trigger and outside the flag window entirely */
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
    /* >>> WRITTEN BY A DIRECTOR, NOT BY THE AI. <<< The one posing field this
       body has, in the sense ADDING_A_BOSS_ENCOUNTER.txt STEP 5 means it: the
       Rear Gate's grinder death (src/hadad_grinder.c) drives it and nothing
       else in the game touches it.

       0..256, how hard the sprite is being SQUEEZED. 0 is the normal 600x600
       body; 256 is flattened to nothing. The draw narrows the quad by
       (256 - squash)/256 and stretches it TALLER by half as much again, with
       the FEET PINNED — a body being crushed between two plates rises out of
       them, it does not sink into the floor. Transient and per-visit, so it is
       not saved; had_reseat's zero-fill is what puts it back, which is why 0
       and not 256 is the "normal" value. */
    int32_t     squash;
    /* ...and its companion: 1 = A DIRECTOR OWNS THIS BODY, and update_hadads
       must not tick it at all. Set alongside `squash` by the Rear Gate's death
       scene and by nothing else.

       >>> IT IS NOT JUST THE AI THAT IT SWITCHES OFF, AND THAT IS THE POINT.
       <<< A Hadad in HAD_WALK or HAD_ROOTED sets `music_wanted` every frame,
       and the reconciliation at the foot of update_hadads acts on that. A
       director that stopped the stalker track by hand would have it turned
       straight back on by the same frame's reconciliation, on the one frame
       update_hadads still runs after the trigger fires — which is precisely
       the bug the door_anim_active() guard at the top of that function exists
       for, written out in full there. Skipping him instead means the track
       goes quiet through the ONE mechanism that owns it, on the frame the
       plates take him, and stays quiet because the cutscene branch in main.c
       stops calling update_hadads from the next frame on.

       He is still DRAWN, still solid to a weapon and still damageable — the
       director has to be able to kill him at the end of the sequence, so this
       deliberately does not go anywhere near had_vulnerable. Transient and
       per-visit; had_reseat's zero-fill clears it. */
    /* >>> HE JUMPS THE GRINDERS IF THE LEVER WAS THROWN TOO EARLY. <<< The Rear
       Gate's flag-three march, and nothing else in the game. 0 = no leap due,
       HAD_VAULT_PENDING = one is owed and he takes off when he reaches
       HAD_VAULT_Z, HAD_VAULT_AIRBORNE = he is in the air over the plates right
       now. Latched by hadads_grinder_vault() on the frame the lever is thrown
       and cleared by his own landing; see THE THREE ANSWERS TO THE LEVER in the
       flag-three block above. Transient and per-visit, like `squash` — the
       encounter is decided inside one visit and had_reseat's zero-fill is what
       puts it back. */
    int         vault;
    int         frozen;
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
   in HAD_IDLE (a statue, on show from the first frame), every other role in
   HAD_ABSENT (nothing in the room until its trigger fires). Get that pair the
   wrong way round and a West Corridor Hadad stands in the middle of the passage
   as inert, invulnerable masonry. */
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

/* THE LEVER HAS JUST BEEN THROWN. Returns the Hadad standing in the band
   x[-x_half,x_half], z[z_min,z_max] who can be hurt, or NULL. The band is passed
   in rather than defined here because it is the GRINDERS' geometry and
   grinder_puzzle.c is where that lives.

   >>> IT IS ASKED ONCE, ON THE THROW, AND NOT EVERY FRAME OF THE TRAVEL. <<<
   That is the difference between "the player caught him with the plates" and
   "the plates were shut and he walked into them", and the second one was
   happening: `shut` stays true after the travel ends, so a lever thrown long
   before he arrived still killed him on contact, minutes later, with nobody
   watching. The two OTHER answers the same throw can give are
   hadads_grinder_vault() and doing nothing at all.

   >>> AND IT USED TO BE THE KILL ITSELF. <<< It emptied his health on the spot
   through hadad_damage, which is what "the lever kills him" meant before the
   death got a scene. It now only ANSWERS, and src/hadad_grinder.c does the
   killing at the end of that scene — through the same hadad_damage call, so
   the grey burst, the cue and the music stop are still the hundredth-axe-swing
   ones. Nothing else calls this. */
Hadad *hadads_grinder_caught(int32_t x_half, int32_t z_min, int32_t z_max);

/* THE LEVER WAS THROWN AND THE PLATES DID NOT HAVE HIM: he was still SOUTH of
   the kill band, i.e. `z_south` is that band's south edge. Latches the LEAP on
   any Hadad this side of it who is walking his authored path — he carries on
   marching, jumps the closed machine when he reaches HAD_VAULT_Z, lands north
   of it and finishes the climb. Returns 1 if a leap was latched.

   Call it ONLY after hadads_grinder_caught has come back NULL, and only from
   the throw: the two are the second and first answers of one verdict taken on
   one frame (see THE THREE ANSWERS TO THE LEVER in the flag-three block above).
   A Hadad north of the band is the third answer and needs no call at all — he
   simply walks on.

   The band edge is passed in for the reason the crush strip is: it is the
   GRINDERS' geometry and grinder_puzzle.c owns it. Ignores a Hadad who is
   already following the player — the leap belongs to the march, and a pursuit
   steers round the world rather than marching through it. */
int hadads_grinder_vault(int32_t z_south);

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

/* ---------------------------------------------------------------------------
 * THE RECEPTION DIRECTOR API   (src/reception_hadad.c and nothing else)
 * ---------------------------------------------------------------------------
 * Two functions. That role's HAD_ABSENT branch does nothing on its own — the
 * gate is two saved flags and the trigger is a place on a balcony, and neither
 * is a property of this enemy — so BEGINNING him is the whole conversation.
 */

/* The live Reception Hadad, or NULL — not placed, not in this area, or dead.
 * For ARMING only: latch the answer, do not re-ask it every frame
 * (ADDING_A_BOSS_ENCOUNTER.txt STEP 4). */
Hadad *hadad_reception_instance(void);

/* Put him IN THE CEILING in front of the north-west door and let go. He falls
 * to the second level under ordinary gravity and then walks his four stages;
 * nothing else is owed and there is no counterpart to stop him. Safe to call
 * with NULL (a debug jump can empty the slot). */
void  hadad_reception_begin(Hadad *h);

/* ---------------------------------------------------------------------------
 * PUT THE WEST CORRIDOR RETURN BACK ON THE TABLE   (src/main.c and nothing else)
 * ---------------------------------------------------------------------------
 * Reseat the HAD_ROLE_WEST_CORR_RET instance on its appearance point in front
 * of the single door, at full health, in HAD_ABSENT — ready to run again the
 * next time the player walks into the corner.
 *
 * >>> THIS IS THE ONE PLACE IN THE GAME A DEAD HADAD COMES BACK, AND THAT IS
 * WHY THE FUNCTION EXISTS. <<< The return already re-arms on every visit by
 * itself (hadads_rest reseats every non-plinth role, and its gate — FLAG_HADAD_
 * TWO alone — never closes), so for a living instance this call changes
 * nothing. What it covers is the instance the player spent a hundred crucifaxe
 * swings KILLING: `dead` is per instance and saved, hadads_rest skips the dead,
 * and a corpse cannot herd anybody anywhere.
 *
 * It is called when the player retreats from the Rear Gate into the West
 * Corridor with the third encounter unresolved (src/main.c's STATE_REAR_GATE
 * branch). The corridor behind them is about to be closed for good, so the
 * house has to push them onward whether or not they once cleared this room —
 * which is exactly what the return does: he blocks the double door back out and
 * leaves the single one into the Reception as the only way on.
 *
 * Callable from ANY room: the instance is area-tagged and lives in the same
 * global array as every other, so this does not care where the player is
 * standing. A no-op if the West Corridor has never been seeded.
 */
void hadad_wc_return_rearm(void);

#endif
