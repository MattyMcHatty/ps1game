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

#define ROUNDS_PER_PICKUP  100  /* rounds granted by one Standard Rounds pickup */
#define GRAVEOLVER_CAPACITY  6  /* rounds the Grave-olver cylinder holds at once */

#define PICKUP_MSG_DURATION 120  /* frames (~2 seconds at 60fps) */
#define PICKUP_MSG_COUNT    3

typedef struct {
    char msg[64];
    int  timer;
} PickupEntry;

extern PickupEntry pickup_log[PICKUP_MSG_COUNT];

void show_pickup_msg(const char *item_name);
void draw_hud(RenderContext *ctx);

#endif
