#include "debug_opts.h"
#include "player.h"
#include "copper_pot.h"
#include "valve_handle.h"   /* DBG_HAS_VALVE_HANDLE takes the wheel off its pipe */
#include "birdcage.h"       /* DBG_HAS_HATCH_KEYS washes Maze One's cage out     */

int debug_opts[DEBUG_OPT_COUNT] = { 0 };   /* all cheats off by default; the
                                              rest zero-initialise with it */

const char *const debug_opt_names[DEBUG_OPT_COUNT] = {
    "HAS GRAVE-OLVER",
    "HAS HELLUMINATOR",   /* 16 chars — the label limit exactly (debug_opts.h) */
    "HAS WAX AND POT",
    "HAS PIANO KEY",
    "HAS KEY STONES",
    "HAS VALVE HANDLE",   /* 16 chars — the label limit exactly */
    "HAS HATCH KEYS",
    "EXIT DOOR SOLVED",
    "INFINITE LIFE",
    "INFINITE STAMINA",
    "HADAD FLAG ONE",
    "HADAD FLAG TWO",
    "HADAD FLAG THREE",
};

static int grants_pending = 0;

void debug_opts_arm_grants(void) { grants_pending = 1; }

/* The Attic Exit's exit-door puzzle, put where a player who had solved it would
   have left it. Two options grant it — DBG_EXIT_DOOR_SOLVED, which is nothing
   but this, and DBG_HADAD_FLAG_ONE, which cannot describe a reachable game
   state without it (his plinth is out in the garden and this door is the only
   way there; granting the flag without the door left the two disagreeing, the
   door locked while its own prompt offered "Press O to remove the keystones",
   which is the one thing a locked door cannot do). It is idempotent, so ticking
   both is legal and costs nothing.

   What "solved" means here, piece by piece:

     - FLAG_EXIT_DOOR_UNLOCKED, so the door is open and carries the xt_dr_cmplt
       art (attic_exit.c's door_tex_id reads it on entry, and the room's prop
       placement re-uploads from it after these grants land);
     - the three collected stones are SPENT — cleared from the inventory,
       because they are in the door. Note this runs AFTER DBG_HAS_KEY_STONES
       and deliberately undoes it: ticking both means "collected them and then
       used them", which is the true story, not "holding a second set";
     - FLAG_STOVE_SOLVED and FLAG_ANZU_SOLVED, the two puzzles that MADE two of
       those stones. Without them the burner and the tablets are still live with
       the stones already spent, and each would happily mint a replacement. Same
       trap DBG_HAS_KEY_STONES documents, one step further along.

   KNOWN GAP, shared with DBG_HAS_KEY_STONES and not introduced here: the BLUE
   stone is an unconditional first-entry spawn on the Attic Stairwell altar
   (world.c) with no flag behind it, so walking into that room on a granted save
   still lays a second one out. Retiring it would take a new GameFlag; nothing
   downstream can be farmed with it. */
static void grant_exit_door_solved(void) {
    game_flag_set(FLAG_EXIT_DOOR_UNLOCKED);
    game_flag_set(FLAG_STOVE_SOLVED);
    game_flag_set(FLAG_ANZU_SOLVED);
    player_items &= ~((1 << ITEM_GREEN_KEY_STONE)  |
                      (1 << ITEM_YELLOW_KEY_STONE) |
                      (1 << ITEM_BLUE_KEY_STONE));
}

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

    /* The lantern, exactly as walking over the Greenhouse annexe pickup leaves
       it: owned, equipped, tank full.

       >>> THIS USED TO BE PART OF THE BLOCK ABOVE. <<< The gun handed the
       lantern over as well, on the argument that "the ranged weapons" is one
       decision for someone scrolling a menu. It is its own option now because
       the two stopped being interchangeable: the Helluminator is the only
       weapon that can touch a stalking Living Statue and it does triple to
       zombies, so "in the maze with the lantern and no gun" is a state worth
       testing and one the bundle made unreachable. Ticking both gives what the
       single option used to.

       AFTER the Grave-olver block and setting current_weapon second on purpose:
       with both ticked the lantern is what you are holding when the room comes
       up, because it is the more specific request of the two.

       player_oil_refill() rather than an assignment, so the one place that
       knows what "full" means stays player.c (player.h). A full tank matters
       more here than a deep ammo reserve does above — there is no oil pickup
       anywhere in the game, and no refill point is placed yet, so what is
       granted is all there will ever be.

       No texture upload: hellumin.tim owns its VRAM and menu_init LoadImages it
       once at startup, so the inventory icon is already right on a direct jump
       (the same reason the piano key needs none). */
    /* The Valve Handle, put where a player who had taken it would have left the
       world: in their inventory, OFF the Greenhouse's pipe, and with that room
       flooded. All three are one event in real play (greenhouse_flood.c's
       take()), so granting the item alone would leave the wheel still hanging on
       a pipe whose prompt still reads "Press O to remove" — the same kind of
       disagreement grant_exit_door_solved() exists to avoid.

       The mount is cleared through valve_handle_set_present() rather than by
       touching valve_mounts directly, so the one function that knows what
       "taken" means to that module stays the only one that does it (it also
       cancels any turn or fit in progress, which cannot be live here but is the
       contract). valve_mounts is a GLOBAL area-tagged array, not a room-swapped
       one, so this holds whichever room the jump lands in.

       No texture upload and no menu work: the handle's inventory icon is part of
       menu_init's startup LoadImage, the same reason the piano key needs none. */
    if (debug_opts[DBG_HAS_VALVE_HANDLE]) {
        player_items |= (1 << ITEM_VALVE_HANDLE);
        game_flag_set(FLAG_GREENHOUSE_FLOOD);
        {
            int m = valve_mount_in_area(STATE_GREENHOUSE);
            if (m >= 0) valve_handle_set_present(m, 0);
        }
    }

    if (debug_opts[DBG_HAS_HELLUMINATOR]) {
        player_weapons |= (1 << WEAPON_HELLUMINATOR);
        current_weapon  = WEAPON_HELLUMINATOR;
        player_oil_refill();
    }

    if (debug_opts[DBG_HAS_WAX_AND_POT]) {
        player_items |= (1 << ITEM_COPPER_POT) | (1 << ITEM_WAX_CUBE);
        /* Both of these are SPENT by the kitchen stove, so the grant has to say
           where they came from as well as hand them over — exactly the situation
           DBG_HAS_KEY_STONES documents below. Without the flags, cooking with
           granted ingredients would drop a second pot back on the conservatory
           floor and re-arm the 2F trick drawers for another Wax Cube. */
        game_flag_set(FLAG_POT_TAKEN);
        game_flag_set(FLAG_DRAWERS_SOLVED);
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

    /* AFTER the block above, which it undoes on purpose: the stones are in the
       door, not in the pocket. See grant_exit_door_solved. */
    if (debug_opts[DBG_EXIT_DOOR_SOLVED]) grant_exit_door_solved();

    /* Hadad. For his PLACEMENT these are nothing but the flag: hadads_rest() and
       update_hadads work the rest out between them, so setting the bit here and
       jumping to the Rear Gate puts him exactly where a player who had earned it
       would find him — posted at the bottom of the corridor for flag one, out of
       the room until the ramp trigger for flag three. See the three-way state
       table in hadad.h.

       What they DO carry beyond the bit is the state of the Attic Exit's exit
       door, because his encounters sit on the far side of it and cannot be
       reached with it shut. Each block below says what it grants and why.

       Set in story order so each lands on top of the last, which is the
       relationship the encounter itself has. Flag TWO is not a placement at
       all — it seals the Attic Exit's door from both faces (see
       exit_door_puzzle.h) — but it belongs to the same run and grants the same
       way. (The DECLARATION order in GameFlag is not this order; see the note
       on FLAG_HADAD_ONE in src/player.h.) */
    if (debug_opts[DBG_HADAD_FLAG_ONE]) {
        game_flag_set(FLAG_HADAD_ONE);
        /* >>> AND THE EXIT-DOOR PUZZLE WITH IT. <<< Hadad is on the Rear Gate's
           plinth, out in the garden, and the ONLY route there is through the
           Attic Exit's exit door — so a player who has had him off that plinth
           has necessarily solved it. That is the same grant DBG_EXIT_DOOR_SOLVED
           makes, so both go through the one helper above — and that is where the
           reasoning lives, down to why it spends the three stones and retires
           the two puzzles that minted them. */
        grant_exit_door_solved();
    }
    if (debug_opts[DBG_HADAD_FLAG_TWO]) {
        game_flag_set(FLAG_HADAD_TWO);
        /* This flag IS "the four stones have been pulled back out of the door",
           so it has to hand them over — all four, magenta included, exactly as
           remove_keystones does. It did not need to before, when nothing here
           emptied the inventory; now that flag one above spends the three, a
           one+two tick would otherwise leave the player with the door sealed and
           no stones to show for it. Runs after flag one for that reason. */
        player_items |= (1 << ITEM_GREEN_KEY_STONE)   |
                        (1 << ITEM_YELLOW_KEY_STONE)  |
                        (1 << ITEM_BLUE_KEY_STONE)    |
                        (1 << ITEM_MAGENTA_KEY_STONE);
    }
    if (debug_opts[DBG_HADAD_FLAG_THREE]) game_flag_set(FLAG_HADAD_THREE);

    /* BOTH HATCH KEYS, and the two long chains that mint them, finished.
       See the option's own note in debug_opts.h for why the chains come with the
       keys rather than the keys arriving on their own: two hatch keys in the
       pocket is not a state the game can reach with either puzzle still live,
       and a grant that left them live would let the player mint a second pair.

       LAST IN THE FUNCTION, on purpose and in two directions:

         it must run AFTER DBG_HADAD_FLAG_TWO, which hands all four key stones
         back over. These four are the SAME stones — a player who has lit the
         keystone has necessarily pulled them out of the exit door first — so
         with both ticked the honest end state is "and then placed them in the
         plinths", which is this block winning;

         and it must run after DBG_HAS_VALVE_HANDLE, which it supersedes: that
         option is the handle in hand with the three pipes untouched, this one is
         the same run finished and the handle spent.

       WHAT IT DOES NOT CLAIM: the Attic Exit's own door state. The stones reach
       the maze through FLAG_HADAD_TWO, so strictly this implies that flag and the
       whole encounter behind it — but that seals a door and changes two other
       rooms, which is a great deal to smuggle into a grant about hatch keys. Tick
       the Hadad options alongside for that; they are additive and this block does
       not fight them. */
    if (debug_opts[DBG_HAS_HATCH_KEYS]) {
        /* ---- The keys themselves ---- */
        player_hatch_keys = HATCH_KEYS_MAX;

        /* ---- KEY ONE: the Keystone Maze, solved ----
           All four stones are IN the alcove plinths, so the flags go on and the
           inventory bits come OFF — keystone_plinths_apply_flags() runs a moment
           after this and installs the lights, the four lit keystone faces and the
           white top from exactly these bits.

           FLAG_KEYSTONE_REWARD is what stops that same call spawning the Hatch
           Key on the keystone: its catch-up branch fires on "four placed, no
           reward recorded", which is precisely the state this block would leave
           without it — and the key it handed over would be a THIRD. */
        game_flag_set(FLAG_KEYSTONE_NW);
        game_flag_set(FLAG_KEYSTONE_W);
        game_flag_set(FLAG_KEYSTONE_NE);
        game_flag_set(FLAG_KEYSTONE_SE);
        game_flag_set(FLAG_KEYSTONE_REWARD);
        player_items &= ~((1 << ITEM_GREEN_KEY_STONE)   |
                          (1 << ITEM_MAGENTA_KEY_STONE) |
                          (1 << ITEM_YELLOW_KEY_STONE)  |
                          (1 << ITEM_BLUE_KEY_STONE));

        /* ---- KEY TWO: the Valve Puzzle, run to its end ----
           The handle came off the Greenhouse's pipe (which floods that room —
           the two are one event, see the DBG_HAS_VALVE_HANDLE block above), all
           three garden pipes were turned, and the handle was SPENT on the third:
           valve_puzzle.c retires it the moment the last flag goes on, so leaving
           it in the inventory here would contradict the three bits below.

           All four mounts are cleared through valve_handle_set_present() for the
           reason that block gives — one function knows what "taken" means. */
        game_flag_set(FLAG_GREENHOUSE_FLOOD);
        game_flag_set(FLAG_VALVE_MAZE_ONE);
        game_flag_set(FLAG_VALVE_MAZE_TWO);
        game_flag_set(FLAG_VALVE_CHAIN_ROOM);
        player_items &= ~(1 << ITEM_VALVE_HANDLE);
        {
            static const GameState VP_ROOMS[4] = {
                STATE_GREENHOUSE, STATE_MAZE_ONE, STATE_MAZE_TWO, STATE_CHAIN_ROOM
            };
            int i;
            for (i = 0; i < 4; i++) {
                int m = valve_mount_in_area(VP_ROOMS[i]);
                if (m >= 0) valve_handle_set_present(m, 0);
            }
        }
        /* ...and the bird cage that key came out of, at the END of its own three
           states: the chain wound in, the drain running, the key washed down it.
           birdcage_wash() sets both bits and is what makes birdcage_state() read
           WASHED, which is in turn what valve_puzzle_apply_flags() uses to take
           the caged Hatch Key out of Maze One on the next entry. */
        birdcage_wash();
        /* The "it has already been laid on the Rear Gate's path" bit, so the LIVE
           spawn in valve_puzzle_apply_flags() does not fire. See the KNOWN GAP in
           debug_opts.h: world_seed_room() still lays one out on a first entry to
           that room, and nothing short of a new flag could know otherwise. */
        game_flag_set(FLAG_DRAIN_KEY_PLACED);
    }
}
