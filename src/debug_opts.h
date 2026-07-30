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
 *   - One-shot grants (GRAVE-OLVER / WAX AND POT): they hand the player things,
 *     so they must fire exactly once, AFTER the destination room has finished
 *     initialising (a room init can reset the inventory). The debug menu arms
 *     them on the jump; main.c consumes the latch alongside savegame_apply_
 *     pending(), which has the same "after the room is up" requirement.
 */

typedef enum {
    DBG_HAS_GRAVEOLVER = 0,  /* own the Grave-olver, loaded, with a deep reserve */
    DBG_HAS_WAX_AND_POT,     /* both non-key inventory items from the start      */
    DBG_INFINITE_LIFE,       /* enemy damage does not reduce health              */
    DBG_INFINITE_STAMINA,    /* sprinting never drains the bar                   */
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
