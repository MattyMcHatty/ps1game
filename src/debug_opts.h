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
 *   - One-shot grants (GRAVE-OLVER / WAX AND POT / PIANO KEY / KEY STONES): they hand the
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
    DBG_INFINITE_LIFE,       /* enemy damage does not reduce health              */
    DBG_INFINITE_STAMINA,    /* sprinting never drains the bar                   */
    /* Hadad's two encounter flags, so the Rear Gate can be jumped into with
       either half of him already armed instead of having to be played up to.
       One-shot grants like the item ones above and for the same reason: they
       set persistent GameFlags, and a room must not be able to init before the
       flag is there — update_hadads reads them on its FIRST frame to decide
       where he is standing (see hadad.h).

       They are NOT mutually exclusive in code, and that is deliberate: flag two
       ignores flag one wherever the two disagree, so ticking both is a legal
       state and the honest way to test that rule. */
    DBG_HADAD_FLAG_ONE,      /* he is posted at the bottom of the corridor       */
    DBG_HADAD_FLAG_TWO,      /* the second encounter: absent until the ramp      */
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
