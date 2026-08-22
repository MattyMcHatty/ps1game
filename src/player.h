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

         FLAG_HADAD_ONE  he has come off the plinth once. He is then permanently
                         posted at the foot of the ramp on every return visit,
                         and the corridor lever is dead for as long as this is
                         the ONLY one of the two that is set.
         FLAG_HADAD_TWO  the second encounter is armed. It IGNORES flag one
                         entirely — the lever works again and Hadad is not in
                         the room until the player comes back up off the ramp.

       Flag two is not set by anything yet, by design: what turns it on has not
       been designed. Until then it is reachable only from the title screen's
       debug menu (DBG_HADAD_FLAG_TWO). */
    FLAG_HADAD_ONE,
    FLAG_HADAD_TWO,
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
    MAX_GAME_FLAGS
} GameFlag;
extern int     game_flags;     /* bitmask — bit GameFlag set means it happened */

static inline int game_flag(GameFlag f) { return (game_flags & (1 << f)) != 0; }
static inline void game_flag_set(GameFlag f) { game_flags |= (1 << f); }

/* Weapons the player owns. The crucifaxe is always present (bit 0); other
   weapons are found in the world. Menu WEAPONS column reads this bitmask. */
typedef enum {
    WEAPON_CRUCIFAXE = 0,
    WEAPON_GRAVEOLVER,
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
