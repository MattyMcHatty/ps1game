#ifndef DEBUG_OPTS_H
#define DEBUG_OPTS_H

/*
 * Debug cheat toggles, set from the right-hand column of the title-screen debug
 * menu (Select opens it; rooms are the left column). All default OFF and stay
 * put for the whole session, so backing out to the title keeps your choices.
 *
 * Two flavours:
 *   - Continuous (INFINITE LIFE / STAMINA): the systems that would spend the
 *     resource read the flag every frame, so they can be flipped between runs
 *     and take effect on the next jump with no extra plumbing.
 *   - One-shot grants (GRAVE-OLVER / HELLUMINATOR / WAX AND POT / PIANO KEY /
 *     KEY STONES / EXIT DOOR SOLVED / the three HADAD flags): they hand the
 *     player things, so they must fire exactly once, AFTER the destination room
 *     has finished initialising (a room init can reset the inventory, and
 *     item_pickups_reset clears the piano key's bit). The debug menu arms
 *     them on the jump; main.c consumes the latch alongside savegame_apply_
 *     pending(), which has the same "after the room is up" requirement.
 */

typedef enum {
    DBG_HAS_GRAVEOLVER = 0,  /* own the Grave-olver, loaded, with a deep reserve */
    /* Own the Helluminator, tank full.
       >>> IT USED TO COME FREE WITH THE GRAVE-OLVER, AND NO LONGER DOES. <<<
       The two ranged weapons were one tick on the argument that "the ranged
       weapons" is a single decision; they are two now because they are not
       interchangeable any more. The lantern is the ONLY thing that can burn a
       stalking Living Statue and it triples against zombies (see damage.h), so
       "jump into the maze with the lantern and nothing else" is a real test of
       an encounter, and it was impossible to set up while the gun carried it.
       Tick both for what the old single option gave. */
    DBG_HAS_HELLUMINATOR,
    DBG_HAS_WAX_AND_POT,     /* both non-key inventory items from the start      */
    DBG_HAS_PIANO_KEY,       /* the Attic Stairwell altar's piano key            */
    DBG_HAS_KEY_STONES,      /* blue + yellow + green: the exit door's inputs    */
    /* The Valve Handle, off the Greenhouse's standing pipe, so the garden's
       three-room Valve Puzzle (src/valve_puzzle.h) can be jumped straight into.
       Without it that puzzle is unreachable from a level jump: the handle is the
       only key to all three pipes and the only place in the game it exists is a
       room at the far end of the garden chain.

       >>> IT IS A ONE-SHOT GRANT AND IT FLOODS THE GREENHOUSE. <<< Taking the
       handle IS what opens that room's roof sprinklers, so "holding the handle"
       and "the Greenhouse is dry" is not a state the game can reach and this
       option does not fabricate it — it sets FLAG_GREENHOUSE_FLOOD and clears
       the Greenhouse mount's wheel, which is what the pipe looks like after the
       player has been there. The same reasoning DBG_HAS_WAX_AND_POT gives for
       setting the flags that say where ITS two items came from.

       KNOWN GAP, and it is the one greenhouse_flood.c's own init note describes:
       jumping DIRECTLY INTO the Greenhouse with this ticked arrives with the
       flag set but the flowers, mushrooms and vine curtains not placed, because
       greenhouse_flood_init() ran in the area init a moment before the grant
       landed. Every other room is unaffected, and walking back into the
       Greenhouse through its door puts it right. */
    DBG_HAS_VALVE_HANDLE,    /* the Greenhouse's wheel; floods that room too     */
    /* BOTH HATCH KEYS, so The Hatch's two keyholes (src/hatch_puzzle.h) and the
       drop into the pit behind them can be jumped straight into. Without it that
       puzzle is unreachable from a level jump by a very long way: the two keys
       are the payoffs of the two LONGEST chains in the garden, and neither is a
       pickup lying anywhere a jump can land.

       >>> IT THEREFORE SOLVES BOTH OF THOSE CHAINS, AND THAT IS THE POINT. <<<
       "Holding two hatch keys" is not a state the game can reach without them,
       so the grant does not fabricate it — it puts the whole of both puzzles
       where a player who had earned the keys would have left them:

         THE KEYSTONE MAZE   all four stones are IN the four alcove plinths (the
                             flags set, the stones cleared from the inventory)
                             and the keystone's reward has been taken.
         THE VALVE PUZZLE    all three garden pipes turned, the Greenhouse
                             flooded and its wheel gone, the handle SPENT out of
                             the inventory (it is consumed on the third pipe),
                             and Maze One's bird cage opened and washed out.

       It SUPERSEDES DBG_HAS_VALVE_HANDLE rather than needing it: that option is
       the handle in hand with the pipes untouched, this one is the same run
       finished. Ticking both is legal and this one wins, because it runs later.

       KNOWN GAP, the same class as the blue key stone's under DBG_HAS_KEY_STONES
       and for the same reason: a first entry into the Rear Gate still lays a
       washed-down Hatch Key out on the path. Nothing records that a key in a
       room the player has never opened was already collected. player_hatch_keys
       caps at HATCH_KEYS_MAX, so it cannot be farmed past two. */
    DBG_HAS_HATCH_KEYS,      /* both keys; the maze and the valves solved with them */
    /* The Attic Exit's exit-door puzzle, already solved: the door stands open,
       carries the xt_dr_cmplt art and its prompt is the way OUT, so the room
       behind it can be walked into without placing a stone.

       It is the SAME grant DBG_HADAD_FLAG_ONE carries below — one helper, two
       callers — because that flag cannot describe a real game state without it
       (his plinth is on the far side of this door). Ticking both is therefore
       legal and idempotent; this one exists so the door can be had on its own,
       with Hadad still unplaced.

       A one-shot grant like the item ones above, and for the stronger reason:
       it sets persistent GameFlags that the Attic Exit DERIVES its door art
       from on entry (attic_exit.c's door_tex_id), so it must land before the
       room reads them. main.c consumes the latch in the right place already.

       >>> IT SPENDS THE THREE STONES AND RETIRES TWO PUZZLES. <<< That is what
       "solved" means — the stones are IN the door, and the stove and the Anzu
       tablets that minted two of them are done. debug_opts.c has the full
       reasoning; the short of it is that granting the door without them leaves
       the burner and the tablets live, ready to mint a second set. */
    DBG_EXIT_DOOR_SOLVED,    /* Attic Exit door open; its three stones spent     */
    DBG_INFINITE_LIFE,       /* enemy damage does not reduce health              */
    DBG_INFINITE_STAMINA,    /* sprinting never drains the bar                   */
    /* Hadad's three encounter flags, so the Rear Gate can be jumped into with
       any of them already armed instead of having to be played up to.
       One-shot grants like the item ones above and for the same reason: they
       set persistent GameFlags, and a room must not be able to init before the
       flag is there — update_hadads reads them on its FIRST frame to decide
       where he is standing (see hadad.h).

       They are NOT mutually exclusive in code, and that is deliberate: flag
       three ignores flag one wherever the two disagree, so ticking both is a
       legal state and the honest way to test that rule.

       >>> THEY ALSO CARRY THE ATTIC EXIT'S EXIT DOOR. <<< Every one of his
       encounters is out in the garden, behind that door, so none of them is
       reachable with it shut — a flag granted without the door would describe a
       game state that cannot exist. Flag ONE therefore also solves the door
       puzzle — the very same grant DBG_EXIT_DOOR_SOLVED above makes, through
       the one helper both call (stones spent, door open, and the two puzzles
       that minted two of those stones retired) — and flag TWO hands all four
       stones back over, which is what that flag means. debug_opts.c has the
       full reasoning.

       FLAG_HADAD_TWO is the odd one in another way too: unlike the other two it
       is not a placement at all, it is the state of that door — both faces
       sealed, all four key stones in the inventory (exit_door_puzzle.h). */
    DBG_HADAD_FLAG_ONE,      /* posted at the corridor foot; door puzzle solved  */
    DBG_HADAD_FLAG_TWO,      /* the exit door is sealed; all four stones held    */
    DBG_HADAD_FLAG_THREE,    /* the third encounter: absent until the ramp       */
    /* ASAG IS DEAD AND HIS DOOR SCENE HAS RUN, so Chapter 3 can be walked into
       from the Outside Catacombs instead of only being jumped to by name.

       It is one bit, FLAG_ASAG_DEAD, and everything the option is FOR is
       somebody else reading it (the list is in src/catacomb_open.h): the two
       leaves stand apart, the fifteen backing polys behind the doorway stay
       lit, and the mouth offers Circle — which is the way through to the
       CATACOMBS ENTRY. The row of that name in the level column jumps straight
       INTO that room; this option is the other half of it, the APPROACH, and
       the only way to exercise the doorway's own prompt and the transition it
       starts without playing the boss.

       A one-shot grant like the items above, and it also POSES THE LEAVES
       itself. catacomb_doors_init() derives the pose from the flag on room
       entry, and on a direct jump INTO the Outside Catacombs that init has
       already run by the time the latch is consumed — so the flag on its own
       would arrive at a lit doorway with two shut doors across it. Setting the
       slide here closes that gap; it is the same call the scene makes a frame
       at a time, and the next entry to the room re-derives it from the flag.

       >>> IT DOES NOT CLAIM THE TWO HATCH KEYS, though strictly it implies
       them. <<< The only route into Asag's arena is the drop in The Hatch,
       behind both keys — but that is DBG_HAS_HATCH_KEYS' whole pair of garden
       chains, which is a great deal to smuggle into a bit about a door, and it
       is the same line that option draws for itself about the Hadad flags. Tick
       both for the honest full run; they are additive and do not fight.

       What the bit does elsewhere is wanted rather than tolerated: The Hatch
       retires its drop (src/hatch_puzzle.c) — no sign over the lip, and Circle
       there does nothing — because the arena is a one-way pocket with nothing
       left at the bottom of it. */
    DBG_ASAG_DEFEATED,       /* Asag dead: the catacomb doors stand open        */
    DEBUG_OPT_COUNT
} DebugOpt;

/* 0 = off, 1 = on. Indexed by DebugOpt. */
extern int debug_opts[DEBUG_OPT_COUNT];

/* Menu labels, indexed by DebugOpt. Max 16 chars — the menu draws them as
   "*[X] NAME" in a 21-character column (see title_init). */
extern const char *const debug_opt_names[DEBUG_OPT_COUNT];

#define DEBUG_GRAVEOLVER_ROUNDS 999   /* reserve granted by DBG_HAS_GRAVEOLVER */

void debug_opts_arm_grants(void);    /* debug menu: a jump is starting */
void debug_opts_apply_grants(void);  /* main.c: room is up — consume the latch */

#endif
