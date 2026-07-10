#ifndef ITEM_PICKUP_H
#define ITEM_PICKUP_H

#include <stdint.h>
#include "render.h"

/*
 * Generic floating collectibles (the Grave-olver weapon and its Standard Rounds
 * for now). One array holds every kind; each kind carries its own sprite TIM and
 * decides what collecting it grants. Modelled on key.c / sml_med.c but unified so
 * new findable weapons/items are a one-line addition (add a PickupKind + its TIM)
 * rather than a whole new module. Persisted per-room via world.c, exactly like
 * keys and meds.
 */

typedef enum {
    PICKUP_GRAVEOLVER = 0,   /* the Grave-olver gun -> WEAPON_GRAVEOLVER */
    PICKUP_ROUNDS,           /* standard rounds     -> player_rounds     */
    PICKUP_KIND_COUNT
} PickupKind;

#define MAX_ITEM_PICKUPS      8
#define ITEM_PICKUP_RADIUS  200   /* horizontal (X/Z) reach */
#define ITEM_PICKUP_HEIGHT  150   /* vertical reach: must be less than a floor's
                                     height (>=150) so a pickup can't be grabbed
                                     from the floor above/below it */

typedef struct {
    int32_t    x, y, z;
    int32_t    bob_angle;
    int32_t    active;
    PickupKind kind;
} ItemPickup;

extern ItemPickup item_pickups[MAX_ITEM_PICKUPS];
extern int        item_pickup_count;

/* Load every kind's sprite TIM into VRAM. Call ONCE at startup (LoadImage is
   only safe before the main render loop begins). */
void item_pickups_load_textures(void);

/* Place a collectible into the live array. Returns its index, or -1 if full. */
int  item_pickup_spawn(int32_t x, int32_t y, int32_t z, PickupKind kind);

void item_pickups_update(void);
void item_pickups_draw(RenderContext *ctx);
void item_pickups_reset(void);   /* also resets the weapon/rounds inventory */

#endif
