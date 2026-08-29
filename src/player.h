#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include "render.h"
#include "damage.h"

#define MAX_HEALTH 100

extern int32_t player_health;
extern int     game_over;
extern int     flash_timer;
extern int     damage_timer;
extern int     player_keys;    /* bitmask — bit N set means KeyType N is held */

/* Non-key collectible inventory items the player carries between rooms (kept in
   a bitmask, saved alongside player_keys). Menu ITEMS column reads this. */
typedef enum {
    ITEM_COPPER_POT = 0,
    ITEM_WAX_CUBE,          /* awarded by solving the 2F hall trick-drawers puzzle */
    ITEM_GREEN_KEY_STONE,   /* awarded by cooking the pot + cube on the kitchen stove */
    ITEM_PIANO_KEY,         /* found on the Attic Stairwell altar; puzzle input, TBD */
    ITEM_BLUE_KEY_STONE,    /* found on the Attic Stairwell altar; puzzle input, TBD */
    ITEM_YELLOW_KEY_STONE,  /* awarded by solving the piano room's Anzu Tablet puzzle */
    ITEM_MAGENTA_KEY_STONE, /* the fourth stone. Its art is the one already fixed into
                               the exit door's bottom socket (exit_door_puzzle.c draws
                               that from its own copy of the same TIM); nothing places
                               a magenta pickup yet, so the bit is only reachable from
                               a PICKUP_MAGENTA_KEY_STONE spawn added later */
    ITEM_VALVE_HANDLE,      /* the wheel taken off the Greenhouse's standing pipe.
                               Not a PickupKind: there is no sprite to walk into,
                               the player unbolts the 3D prop itself
                               (src/greenhouse_flood.c) and the mount's `present`
                               bit is what the world remembers */
    MAX_ITEM_TYPES
} ItemType;
extern int     player_items;   /* bitmask — bit ItemType set means it is held */

/* --- Persistent world/puzzle flags ------------------------------------------
   One saved bitmask for "this has happened and must stay happened", for events
   that award no item to hang the flag on, or whose award can later be spent.
   The piano puzzle needs one because it consumes the Piano Key and leaves
   nothing behind, so without a flag a reloaded save would put the bookcase back.
   The stove puzzle used to hang off simply owning the Green Key Stone, but the
   Attic Exit's door puzzle now CONSUMES that stone, so it needs a flag too.

   ADDING A FLAG: add a GameFlag before MAX_GAME_FLAGS. The bits ride in
   SaveData.flags, so the word is saved and restored wholesale — no save-format
   change is needed for a new bit (only for a 33rd one). */
typedef enum {
    FLAG_PIANO_SOLVED = 0,   /* Piano Key placed: keys repaired, bookcase sunk */
    FLAG_ANZU_SOLVED,        /* Anzu Tablet assembled: tablets gone, room music changed */
    FLAG_LIGHTS_SOLVED,      /* Attic Exit lightswitches set: the cage gate is winched away */
    FLAG_EXIT_DOOR_UNLOCKED, /* Attic Exit keystones placed: the exit door is open */
    FLAG_STOVE_SOLVED,       /* Kitchen stove cooked: the Green Key Stone was awarded */
    /* The stove CONSUMES its two ingredients, so neither of the facts below can
       be read off the inventory any more — the same trap FLAG_STOVE_SOLVED was
       added for when the exit door started consuming the green stone. Without
       these, cooking put the pot back in the conservatory and re-armed the
       trick drawers, and each could then be farmed indefinitely. */
    FLAG_POT_TAKEN,          /* Copper Pot collected: its world sprite is gone for good */
    FLAG_DRAWERS_SOLVED,     /* 2F trick drawers beaten: the Wax Cube was awarded */
    /* Hadad, the thing on the Rear Gate's plinth. Two flags rather than one
       because the encounter has two halves that run in the SAME room and want
       opposite behaviour out of it, and neither can be read off anything else —
       he awards no item and consumes none. See src/hadad.h for the whole state
       table; the short version is:

         FLAG_HADAD_ONE    he has come off the plinth once. He is then
                           permanently posted at the foot of the ramp on every
                           return visit, and the corridor lever is dead for as
                           long as this is the ONLY one of the two that is set.
                           It also opens the West Corridor's corner ambush —
                           see west_corridor.h.
         FLAG_HADAD_TWO    the player has pulled the four stones back out of
                           the Attic Exit's exit door. Both faces of that door
                           are then dead — it reads "Locked" from the Attic
                           Exit and from the Garden Stairs — so the whole
                           garden half of the map is sealed off behind it. Set
                           by exit_door_puzzle.c, which also runs the quake
                           that follows. It is DECLARED at the end of this
                           enum, not here: see the warning below.
         FLAG_HADAD_THREE  the third encounter is armed. It IGNORES flag one
                           entirely — the lever works again, Hadad is not in
                           the room until the player comes back up off the
                           ramp, and the West Corridor ambush is closed for
                           good.

       >>> THE STORY ORDER IS ONE, TWO, THREE; THE DECLARATION ORDER IS NOT. <<<
       These are bit positions in `game_flags`, which rides in SaveData.flags
       wholesale, so inserting a member mid-enum shifts everything below it and
       every existing save then reads the wrong bits. FLAG_HADAD_THREE was
       declared here while two was still undesigned; when two finally arrived it
       took the cheap route this note always pointed at and went on the END of
       the enum instead of into the gap. Nothing reads the number — it is a
       name, and the bit index is positional — so the two orders may differ.
       Do the same with the next one.

       Flag three IS set by the game now — it was undesigned for a long time and
       reachable only from the title screen's debug menu. Its one trigger is
       LEAVING THE WEST CORRIDOR BY THE NORTH DOUBLE DOOR, the one back out to
       the top of the Rear Gate's ramp, WITH FLAGS ONE AND TWO BOTH ALREADY SET
       (src/main.c, the STATE_WEST_CORRIDOR ndoor branch). The debug option
       remains, as it does for the other two. */
    FLAG_HADAD_ONE,
    FLAG_HADAD_THREE,
    /* The two doors that are unlocked from one side and read from the other.
       Neither awards or consumes anything, so there is nothing in the inventory
       to read them off — the same reason the puzzle flags above exist. They were
       plain globals reset on a new game until the user asked for them to survive
       a save/load; being bits in this word is all that takes.

         FLAG_HALL_2F_DOOR      the 2F Hall's east door. Unlocked from the hall;
                                Reception's HDOOR reads it (reception.c).
         FLAG_WEST_CORR_DOOR    the West Corridor's east door. Unlocked from the
                                corridor; Reception's NDOOR reads it. Same
                                mechanic, deliberately — see west_corridor.h. */
    FLAG_HALL_2F_DOOR,
    FLAG_WEST_CORR_DOOR,
    /* Hadad's second encounter, declared out of story order — see the block on
       FLAG_HADAD_ONE above for what it means and why it is down here. */
    FLAG_HADAD_TWO,
    /* The East Hall's east double door, the one that leads to the Library and
       then to the Library Destroyed, is buried on the far side. Set by the quake
       that runs the first time the player comes back OUT of the wrecked Library
       into the hall (east_hall_quake.h) — the shake IS the ceiling coming down
       behind them. From then on the door keeps its "Press O to enter" sign but
       Circle only posts a line: the way back east is through the East Stairwell.

       A saved bit rather than a global because it is a one-way world change, and
       because the East Stairwell's own door into the library is still open — a
       reload that forgot this would hand the player a route the story has
       closed. Declared at the END of the enum, as the note above instructs. */
    FLAG_EAST_HALL_RUBBLE,
    /* The Reception's Hadad encounter has been used up. Set at the ARM, on the
       first entry into the room once FLAG_HADAD_TWO has sealed it — not when
       the beat finishes — for the reason FLAG_EAST_HALL_RUBBLE is: the entry
       quake and the walk that follows take the better part of a minute, and a
       reset, a debug jump or a death inside that window must not leave a world
       where the encounter is still waiting to happen. It gates BOTH halves: the
       quake on entry and Hadad himself (src/reception_hadad.h).

       Saved, because it is a one-shot world beat; a reload that forgot it would
       replay the collapse and drop a second Hadad through the ceiling. Declared
       at the END of the enum, as the note above instructs. */
    FLAG_RECEPTION_HADAD,
    /* >>> THE REAR GATE'S CORRIDOR IS SHUT AND THE LEVER IS DEAD. <<< The end
       of Hadad's story, whichever way it goes, and the one flag in this word
       that CLOSES A ROUTE FOR GOOD: the grinders are seated shut by
       grinder_puzzle_place() on every entry from here on, the lever answers a
       press with "The mechanism is broken" and nothing ever re-opens it. The
       hedged corridor is the only way from the Rear Gate's lawn to the ramp and
       the double door at the top of it, so this permanently seals the house off
       from the garden.

       It is set by EVERY ending of the third encounter, which is the point of
       having it rather than reading it off FLAG_HADAD_THREE:
         - THE LEVER IS THROWN AT ALL (src/grinder_puzzle.c). That is the first
           and the usual one: under flag three the throw is a single decision —
           it either catches him between the plates or it does not — and the
           mechanism is spent the instant it is made, so the encounter cannot be
           re-rolled by re-opening the corridor and waiting for him to come round
           again;
         - he is CAUGHT in the plates and the death scene kills him
           (src/hadad_grinder.c) — which now always follows a throw, so this one
           is a second, idempotent set that stands as its own guarantee;
         - or the lever is never touched, and the player leaves the Rear Gate by
           any route except the south door into the West Corridor (src/main.c's
           STATE_REAR_GATE branch). Leaving by THAT door instead re-arms the West
           Corridor return so the house pushes them back out again — see
           hadad_wc_return_rearm() in src/hadad.h.
       Either way the machinery has been used up and the corridor is closed.

       Saved, because it is a one-way world change: a reload that forgot it
       would hand the player back a route the story has taken away, standing in
       a corridor whose plates are open again. Declared at the END of the enum,
       as the note above instructs. */
    FLAG_GRINDER_BROKEN,
    /* ---- The Keystone Maze's four plinths --------------------------------
       One bit per hedge-alcove plinth, set the moment the right key stone is
       placed in it. Five bits rather than one because the puzzle is solved a
       quarter at a time and each quarter is a separate, permanent world change:
       the plinth's own light goes out, the matching face of the central
       keystone lights, and the STONE IS CONSUMED. Nothing in the inventory can
       be read back afterwards to tell which of the four were spent — the same
       trap FLAG_STOVE_SOLVED exists for — so the bits have to carry it.

       The order here is the order the puzzle is stated in, not a compass walk:
       NW/green, W/magenta, NE/yellow, SE/blue. See src/keystone_plinths.h for
       the plinth <-> colour <-> face table.

         FLAG_KEYSTONE_REWARD  all four are in and the payoff has RUN: the top
                               face burns white and the flame rounds have been
                               spawned onto the keystone's west lip. Separate
                               from the four above because the reward is
                               spawned by a cutscene that can be cut short by a
                               quit or a death between the fourth placement and
                               the spawn; without this bit that player would
                               come back to a solved puzzle and no rounds, and
                               with it the room hands them over on entry
                               instead. It is also what stops the payoff
                               replaying on every later visit.

       Declared at the END of the enum, as the note above instructs. */
    FLAG_KEYSTONE_NW,
    FLAG_KEYSTONE_W,
    FLAG_KEYSTONE_NE,
    FLAG_KEYSTONE_SE,
    FLAG_KEYSTONE_REWARD,
    /* The GREENHOUSE'S TEN PIPE BUTTONS have been set to 3/4/5/6 and the vine
       curtain over the west annexe has wound up into the roof
       (src/greenhouse_puzzle.c). A flag rather than a reading of the world for
       the usual reason: the puzzle awards no item and consumes none, and the one
       world change it does make — the cleared curtain — is not enough on its
       own. Without this bit a returning player would find the annexe open and
       the board dark, and could press the four again; with it the room installs
       the lit set on entry and the buttons go inert.

       It does NOT gate the Helluminator. That sits in the annexe from the
       Greenhouse's first visit, unreachable rather than unspawned, so the
       ordinary per-room pickup persistence is what remembers whether it has been
       taken — see world_seed_room(). Declared at the END of the enum, as the
       note above instructs. */
    FLAG_GREENHOUSE_BUTTONS,
    /* MAZE ONE'S BIRD CAGE (src/birdcage.c). The cage hangs 328 above the
       player's eye with a Hatch Key locked inside it, and the prompt under it
       reads one of three lines depending how far the puzzle has got. TWO bits
       for THREE states because the progression is one-way and each bit names a
       world fact of its own:

         neither          the key is in the cage and out of reach
         _OPEN            the cage has been opened and the key has dropped into
                          the drain that crosses the path below it
         _OPEN | _WASHED  the drain has run and the key has gone with it

       _WASHED is only ever set on top of _OPEN, so birdcage_state() reads them
       in that order and a save that somehow carried _WASHED alone still shows
       the last line rather than an impossible fourth state. Neither can be read
       off the world instead: the key pickup's own taken-bit says nothing here
       (it is never collected from the cage, which is the whole point), and the
       cage art does not change. Declared at the END of the enum, as the note at
       the top instructs. */
    FLAG_BIRDCAGE_OPEN,
    FLAG_BIRDCAGE_WASHED,
    /* THE GREENHOUSE HAS FLOODED (src/greenhouse_flood.c). Taking the Valve
       Handle off the standing pipe in the south bay opens the roof sprinklers,
       and what comes down with the water stays: six Rafflesias on the room's
       poison_flower_base beds, four Mushroom Heads, and five vine curtains
       across the aisles between the planting beds.

       A flag rather than a reading of the world, for a reason none of the three
       populations can cover on its own. The mount's `present` bit says the
       handle was TAKEN, but nothing says the scene has RUN — a debug grant of
       the item would leave a room that should be overgrown standing empty. The
       flowers have no save state at all by design (rafflesia.h), the curtains'
       health bytes cannot distinguish "never dropped" from "cleared", and the
       mushrooms are seeded by world_seed_room(), which needs to know on a save
       rebuild whether this room's four exist. This bit is what all three read.
       Declared at the END of the enum, as the note at the top instructs. */
    FLAG_GREENHOUSE_FLOOD,
    /* THE VALVE PUZZLE (src/valve_puzzle.c). One bit per pipe, and each one
       means "this pipe has been turned and is now inert". Three pipes carry the
       pipe texture — Maze One's standpipe, Maze Two's and the Chain Room's — and
       the Valve Handle taken off the Greenhouse fits all three in any order.

       A flag per pipe rather than a reading of the world, because what each one
       leaves behind is either invisible or shared:

         _MAZE_ONE    opens the drain. What it produces is a SOUND (SFX_WATER,
                      looping in this room, Fountain Square and the Rear Gate)
                      and nothing else — there is no world state at all to read
                      it back off, which is why the bit is the only record.
         _MAZE_TWO    unlocks BOTH of the Chain Room's gates, from all four
                      sides. The gates have no state of their own: the leaves
                      are drawn shut either way and their collision walls never
                      move, so this bit IS the lock (see chain_room.c,
                      maze_two.c and keystone_maze.c, which all read it).
         _CHAIN_ROOM  winds the chain in over Maze One's bird cage. It advances
                      FLAG_BIRDCAGE_OPEN, but that bit cannot stand in for this
                      one: the cage advances to _WASHED by two different routes
                      depending which pipe was turned first, so the cage state
                      says nothing about whether THIS pipe is spent.

       All three set means the handle is CONSUMED — see valve_puzzle.c's
       retire_handle(). That is deliberately not readable off the inventory
       either: the item is also gone before the player ever finds it, and a
       debug grant would put it back. Declared at the END of the enum, as the
       note at the top instructs. */
    FLAG_VALVE_MAZE_ONE,
    FLAG_VALVE_MAZE_TWO,
    FLAG_VALVE_CHAIN_ROOM,
    /* THE REAR GATE'S HATCH KEY HAS BEEN PLACED. The drain that Maze One's valve
       opens washes the bird cage's key out of the maze and drops it at the far
       end of the channel, on the Rear Gate's path just north of the grinders
       (valve_puzzle_apply_flags()).

       IT IS A "PLACED" BIT, NOT A "TAKEN" ONE, and that distinction is the
       whole reason it exists. The key appears in a room the player has almost
       certainly already visited, so it cannot be seeded: world_enter() restores
       that room from its RoomState snapshot and never runs the seed again. It is
       therefore spawned on the first entry after FLAG_BIRDCAGE_WASHED — and
       without a record of having done so, every later entry would spawn another,
       and the player could farm hatch keys off it.

       Once set, the ordinary per-room persistence owns the pickup: it rides in
       the Rear Gate's snapshot, and world_seed_room() re-places it under THIS
       flag on a save rebuild so WorldDelta.items_gone still clears it if it has
       been collected. Declared at the END of the enum, as the note at the top
       instructs. */
    FLAG_DRAIN_KEY_PLACED,

    /* THE HATCH'S TWO LEAVES HAVE BEEN THROWN OPEN. The pair of doors over the
       pit in the middle of The Hatch's yard (src/hatch_doors.c), which is NOT
       the lid on the brick well in that room's north chamber — that one is part
       of the room mesh and does not move.

       Set on the PRESS rather than on the last frame of the swing, so a save
       taken while the doors are still moving comes back with them open instead
       of shut. There is deliberately no second bit for "halfway": the whole
       animation is under a second and hatch_doors_init() poses the pair straight
       onto its last frame off this one bit.

       Declared at the END of the enum, as the note at the top instructs — these
       are bit positions in a saved word and inserting one renumbers every flag
       after it. This is bit 28 of 32. */
    FLAG_HATCH_DOORS_OPEN,
    MAX_GAME_FLAGS
} GameFlag;
extern int     game_flags;     /* bitmask — bit GameFlag set means it happened */

/* game_flags is a 32-bit word in the save blob (SaveData.flags), so the enum
   cannot outgrow it without widening that field and versioning every save. */
_Static_assert(MAX_GAME_FLAGS <= 32, "game_flags is 32 bits wide");

static inline int game_flag(GameFlag f) { return (game_flags & (1 << f)) != 0; }
static inline void game_flag_set(GameFlag f) { game_flags |= (1 << f); }

/* Weapons the player owns. The crucifaxe is always present (bit 0); other
   weapons are found in the world. Menu WEAPONS column reads this bitmask. */
typedef enum {
    WEAPON_CRUCIFAXE = 0,
    WEAPON_GRAVEOLVER,
    WEAPON_HELLUMINATOR,   /* the lantern, found in the Greenhouse's west annexe */
    MAX_WEAPON_TYPES
} WeaponType;

extern int player_weapons;     /* bitmask — bit WEAPON_* set means it is owned */

/* --- Grave-olver ammunition -------------------------------------------------
   The cylinder holds ONE type at a time; the reserve is counted per type. R2
   swaps the chambered type, which costs a full reload (see graveolver.c).

   ADDING AN AMMO TYPE: add an AmmoType before MAX_AMMO_TYPES, add its row to
   ammo_info[] in player.c, add a PickupKind + TIM in item_pickup.c, and give it
   a DamageType that enemy weakness tables can key on (see damage.h). */
typedef enum {
    AMMO_STANDARD = 0,
    AMMO_FLAME,
    MAX_AMMO_TYPES
} AmmoType;

typedef struct {
    const char *name;       /* HUD label beside the count, e.g. "Flame Rounds" */
    const char *load_msg;   /* pickup-log line posted when a swap completes    */
    DamageType  damage;     /* what enemy weakness tables key on               */
    uint8_t     flash_r, flash_g, flash_b;   /* muzzle-flash colour when fired */
} AmmoInfo;

extern const AmmoInfo ammo_info[MAX_AMMO_TYPES];

extern int       player_ammo[MAX_AMMO_TYPES];  /* reserve rounds held, per type */
extern AmmoType  graveolver_ammo;              /* type currently in the cylinder */
extern int       graveolver_loaded;  /* rounds currently in the cylinder (0..6) */
/* Hatch keys held. A COUNTER rather than a bit in player_items: the garden has
   two hatches and both keys stack into one inventory slot, so what matters is
   how many are carried, not merely that one is. */
#define HATCH_KEYS_MAX  2
extern int player_hatch_keys;

/* --- The Helluminator's oil -------------------------------------------------
   >>> IT IS NOT AN AmmoType, AND THAT IS THE POINT. <<< The cylinder swap (R2)
   walks ammo_info[] and will chamber anything the player holds reserve of, so an
   AMMO_OIL would have had to be excluded by hand in next_available_ammo() — a
   conditional in exactly the place tools/ADDING_AN_ITEM.txt warns not to put
   one, and one that would need re-remembering for every later weapon. A separate
   scalar cannot be loaded into a revolver at all.

   It is also not COLLECTED. There is no oil pickup and there is not meant to be:
   the lantern arrives full and is topped up at refill points placed in the
   world. So the only two operations on it are "burn a unit" and "fill to
   HELL_OIL_MAX", which is what player_oil_refill() is for — the second call
   site, wherever the first refill point lands, then costs nothing. */
#define HELL_OIL_MAX  100
extern int player_oil;
void player_oil_refill(void);   /* top up to HELL_OIL_MAX; 1 if it took any */

extern int player_save_count;  /* total successful saves this playthrough (any slot/card) */
extern WeaponType current_weapon;  /* the equipped weapon; L2 cycles owned ones */

/* Total reserve across every type — for "does the player have any ammo at all"
   checks (the inventory menu's Rounds slot, mainly). */
int player_ammo_total(void);

#define ROUNDS_PER_PICKUP    6  /* rounds granted by one ammo pickup —
                                   one cylinder's worth (see GRAVEOLVER_CAPACITY) */
#define GRAVEOLVER_CAPACITY  6  /* rounds the Grave-olver cylinder holds at once */

#define PICKUP_MSG_COUNT    3

/* Log entries never time out: a posted line stays on screen until three newer
   lines have pushed it off the top (or a load/new game clears the log). `live`
   is just "this slot holds a message", not a countdown. */
typedef struct {
    char msg[64];
    int  live;
} PickupEntry;

extern PickupEntry pickup_log[PICKUP_MSG_COUNT];

/* Apply enemy damage. The single choke point for every attack in the game, so
   the INFINITE LIFE debug toggle only has to be honoured here. Attacks still
   land in every other respect — lunge, knockback, hurt cue — the health loss is
   what gets suppressed. Callers keep their own death handling; with the toggle
   on, health never drops, so it simply never triggers. */
void player_hurt(int32_t amount);

/* --- Poison: the spider web's status effect -------------------------------
   While poisoned the player walks at half speed and cannot sprint at all, for
   POISON_DURATION frames. Feedback is the sprint bar turning red (as it does
   when the bar is merely exhausted — poison is the same "you cannot sprint"
   message) and a slight green wash over the scene.

   A second web REFRESHES the timer rather than stacking: two webs landing
   together should not lock the player down for ten seconds. The effect is
   transient and is NOT saved — it is cleared by reset_game and otherwise just
   runs out. */
#define POISON_DURATION 300   /* 5 s at 60 fps */
extern int player_poison_timer;
static inline int player_poisoned(void) { return player_poison_timer > 0; }

void player_poison(void);          /* apply/refresh the status  */
void player_status_update(void);   /* tick it; call once a frame per area */
/* Slight green wash while poisoned. Drawn over the scene but under the HUD, so
   call it from the player-systems draw pass before hud_draw. */
void player_draw_status_overlay(RenderContext *ctx);

/* Post a line to the HUD's log box (bottom right). show_pickup_msg prefixes
   "Picked up "; the _raw form is verbatim, for puzzle and status lines. The box
   itself is drawn by hud.c — see src/hud.h for when it is visible. */
void show_pickup_msg(const char *item_name);
void show_pickup_msg_raw(const char *text);   /* verbatim log line, no prefix */

#endif
