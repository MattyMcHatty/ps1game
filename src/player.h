#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include "render.h"

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
    MAX_ITEM_TYPES
} ItemType;
extern int     player_items;   /* bitmask — bit ItemType set means it is held */

/* Weapons the player owns. The crucifaxe is always present (bit 0); other
   weapons are found in the world. Menu WEAPONS column reads this bitmask. */
typedef enum {
    WEAPON_CRUCIFAXE = 0,
    WEAPON_GRAVEOLVER,
    MAX_WEAPON_TYPES
} WeaponType;

extern int player_weapons;     /* bitmask — bit WEAPON_* set means it is owned */
extern int player_rounds;      /* reserve rounds held (used to reload the cylinder) */
extern int graveolver_loaded;  /* rounds currently in the Grave-olver cylinder (0..6) */
extern int player_save_count;  /* total successful saves this playthrough (any slot/card) */
extern WeaponType current_weapon;  /* the equipped weapon; L2 cycles owned ones */

#define ROUNDS_PER_PICKUP    6  /* rounds granted by one Standard Rounds pickup —
                                   one cylinder's worth (see GRAVEOLVER_CAPACITY) */
#define GRAVEOLVER_CAPACITY  6  /* rounds the Grave-olver cylinder holds at once */

#define PICKUP_MSG_DURATION 120  /* frames (~2 seconds at 60fps) */
#define PICKUP_MSG_COUNT    3

typedef struct {
    char msg[64];
    int  timer;
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
   call it from the player-systems draw pass before draw_hud. */
void player_draw_status_overlay(RenderContext *ctx);

void show_pickup_msg(const char *item_name);
void show_pickup_msg_raw(const char *text);   /* verbatim log line, no prefix */
void draw_hud(RenderContext *ctx);

#endif
