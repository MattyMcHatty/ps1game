#include "debug_opts.h"
#include "player.h"
#include "copper_pot.h"

int debug_opts[DEBUG_OPT_COUNT] = { 0, 0, 0, 0, 0, 0 };   /* all cheats off by default */

const char *const debug_opt_names[DEBUG_OPT_COUNT] = {
    "HAS GRAVE-OLVER",
    "HAS WAX AND POT",
    "HAS PIANO KEY",
    "HAS KEY STONES",
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
        /* Every ammo type, so R2 switching is testable straight off a level
           jump without hunting for the Flame Rounds pickup first. */
        {
            int a;
            for (a = 0; a < MAX_AMMO_TYPES; a++)
                player_ammo[a] = DEBUG_GRAVEOLVER_ROUNDS;
        }
        graveolver_ammo   = AMMO_STANDARD;
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

    if (debug_opts[DBG_HAS_PIANO_KEY]) {
        /* Same end state as walking over the pickup on the Attic Stairwell
           altar. No texture upload to match the pot's above: pno_key.tim owns
           its VRAM outright at (608,256) — item_pickups_load_textures and
           menu_init both LoadImage it once at startup and no room streams over
           it — so the menu icon is already correct on a direct jump. */
        player_items |= (1 << ITEM_PIANO_KEY);
    }

    if (debug_opts[DBG_HAS_KEY_STONES]) {
        /* All three stones the Attic Exit's door puzzle asks for, so a direct
           jump into that room can be played end to end without first cooking the
           green one on the kitchen stove, assembling the Anzu tablet for the
           yellow, or collecting the blue off the Attic Stairwell altar.

           Side effect worth knowing: this also retires the stove puzzle for the
           session, and says so explicitly via FLAG_STOVE_SOLVED. Owning the
           green stone alone would do it (stove_puzzle_solved still accepts the
           stone as proof), but the exit-door puzzle SPENDS the stone — without
           the flag the burner would come back to life the moment it did, ready
           to cook a second one.

           No texture upload to match the pot's above — for the same reason as
           the piano key. grn_ky_stn (552,256), ylw_ky_stn (560,256) and
           bl_ky_stn (624,256) each own their VRAM outright: nothing streams over
           them, and menu_init LoadImages all three once at startup, so the menu
           icons are already correct on a direct jump. */
        player_items |= (1 << ITEM_GREEN_KEY_STONE)  |
                        (1 << ITEM_YELLOW_KEY_STONE) |
                        (1 << ITEM_BLUE_KEY_STONE);
        game_flag_set(FLAG_STOVE_SOLVED);
    }
}
