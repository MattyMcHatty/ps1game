#include "debug_opts.h"
#include "player.h"
#include "copper_pot.h"

int debug_opts[DEBUG_OPT_COUNT] = { 0, 0, 0, 0 };   /* all cheats off by default */

const char *const debug_opt_names[DEBUG_OPT_COUNT] = {
    "HAS GRAVE-OLVER",
    "HAS WAX AND POT",
    "INFINITE LIFE",
    "INFINITE STAMINA",
};

static int grants_pending = 0;

void debug_opts_arm_grants(void) { grants_pending = 1; }

void debug_opts_apply_grants(void) {
    if (!grants_pending) return;
    grants_pending = 0;

    if (debug_opts[DBG_HAS_GRAVEOLVER]) {
        /* Same end state as walking over the reception pickup, but with a
           reserve deep enough not to think about (the menu's ammo counter
           scales to any digit count). */
        player_weapons   |= (1 << WEAPON_GRAVEOLVER);
        current_weapon    = WEAPON_GRAVEOLVER;
        player_rounds     = DEBUG_GRAVEOLVER_ROUNDS;
        graveolver_loaded = GRAVEOLVER_CAPACITY;
    }

    if (debug_opts[DBG_HAS_WAX_AND_POT]) {
        player_items |= (1 << ITEM_COPPER_POT) | (1 << ITEM_WAX_CUBE);
        /* The pot's texture time-shares the key slot and is only uploaded on
           conservatory entry or on a room load once it is owned — neither has
           happened yet on a direct jump, so upload it here or the menu icon is
           whatever the key left behind. Pure LoadImage from a resident RAM copy;
           our callers have already idled the GPU. */
        copper_pot_upload_texture();
    }
}
