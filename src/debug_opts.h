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
 *   - One-shot grants (GRAVE-OLVER / WAX AND POT / PIANO KEY / KEY STONES /
 *     EXIT DOOR SOLVED / the three HADAD flags): they hand the
 *     player things, so they must fire exactly once, AFTER the destination room
 *     has finished initialising (a room init can reset the inventory, and
 *     item_pickups_reset clears the piano key's bit). The debug menu arms
 *     them on the jump; main.c consumes the latch alongside savegame_apply_
 *     pending(), which has the same "after the room is up" requirement.
 */

typedef enum {
    DBG_HAS_GRAVEOLVER = 0,  /* own the Grave-olver, loaded, with a deep reserve */
    DBG_HAS_WAX_AND_POT,     /* both non-key inventory items from the start      */
    DBG_HAS_PIANO_KEY,       /* the Attic Stairwell altar's piano key            */
    DBG_HAS_KEY_STONES,      /* blue + yellow + green: the exit door's inputs    */
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
