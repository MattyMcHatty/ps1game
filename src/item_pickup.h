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
    PICKUP_GRAVEOLVER = 0,   /* the Grave-olver gun -> WEAPON_GRAVEOLVER      */
    PICKUP_ROUNDS,           /* standard rounds -> player_ammo[AMMO_STANDARD] */
    PICKUP_FLAME_ROUNDS,     /* flame rounds    -> player_ammo[AMMO_FLAME]    */
    PICKUP_PIANO_KEY,        /* piano key       -> ITEM_PIANO_KEY bit         */
    PICKUP_BLUE_KEY_STONE,   /* blue key stone  -> ITEM_BLUE_KEY_STONE bit    */
    PICKUP_YELLOW_KEY_STONE, /* yellow key stone-> ITEM_YELLOW_KEY_STONE bit  */
    PICKUP_MAGENTA_KEY_STONE,/* magenta key stone->ITEM_MAGENTA_KEY_STONE bit */
    PICKUP_HATCH_KEY,        /* hatch key       -> player_hatch_keys (stacks) */
    PICKUP_HELLUMINATOR,     /* the lantern -> WEAPON_HELLUMINATOR, and a full
                                tank of oil. The second WEAPON pickup, and the
                                only one that grants its own ammunition: there is
                                no oil pickup and there is not meant to be (see
                                player.h), so if collecting it did not fill the
                                lantern the player would find a weapon that could
                                not be used until a refill point they have not
                                reached yet.                                    */
    PICKUP_KIND_COUNT
} PickupKind;

#define MAX_ITEM_PICKUPS      8
#define ITEM_PICKUP_RADIUS  200   /* horizontal (X/Z) reach */
#define ITEM_PICKUP_HEIGHT  150   /* vertical reach: must be less than a floor's
                                     height (>=150) so a pickup can't be grabbed
                                     from the floor above/below it */

/* Per-pickup horizontal reach. Every ItemPickup carries a `radius`: leave it
   0 and the pickup uses ITEM_PICKUP_RADIUS above, exactly as before; set it to
   a number and THAT is the Manhattan X/Z reach for this one collectible.
   It is what lets a pickup sit in the middle of a counter top or a plinth the
   player is held well clear of, instead of having to be nudged onto the lip
   nearest wherever the collision happens to stop them. The vertical test
   (ITEM_PICKUP_HEIGHT) is NOT widened by it — a bigger reach must never let a
   pickup be taken through the floor above or below. */

/* ---- Per-pickup DISPLAY overrides ------------------------------------------
   Both default to 0, which is the behaviour every pickup had before they
   existed, and both are set through item_pickup_set_display() after a spawn.
   They are what a pickup needs when it sits INSIDE or ON a piece of room
   geometry rather than floating in clear air:

   world_half — half-size in world units, i.e. ITEM_PICKUP_WORLD_HALF for this
     one sprite. A pickup shut inside a small prop has to be smaller than the
     prop or it pokes out of it however well it sorts (Maze One's bird cage is
     110 x 134 across the inside, against the default sprite's 140).

   otz_bias — added to the sprite's OT index, exactly as every room mesh adds
     ITEM_PICKUP_ROOM_BIAS to its own prims. Leaving it 0 is what makes a pickup
     win a tie against the floor under it; setting it to the room bias restores
     TRUE depth order against the room, so geometry genuinely in front of the
     sprite occludes it instead of the sprite showing through. rabisu.c does the
     same thing for the same reason. Use it for a pickup behind bars, in a
     recess, or overlapping a surface it stands on. */
#define ITEM_PICKUP_WORLD_HALF  70   /* default sprite half-size, world units */
#define ITEM_PICKUP_ROOM_BIAS   40   /* what every room mesh adds to its prims */

typedef struct {
    int32_t    x, y, z;
    int32_t    bob_angle;
    int32_t    active;
    PickupKind kind;
    int32_t    amount;   /* rounds granted (PICKUP_ROUNDS only; unused otherwise) */
    int32_t    radius;   /* 0 = use ITEM_PICKUP_RADIUS; else this pickup's reach */
    int32_t    world_half; /* 0 = ITEM_PICKUP_WORLD_HALF; else this one's size  */
    int32_t    otz_bias;   /* 0 = sort in front of ties; else added to its OTZ  */
} ItemPickup;

extern ItemPickup item_pickups[MAX_ITEM_PICKUPS];
extern int        item_pickup_count;

/* Load every kind's sprite TIM into VRAM. Call ONCE at startup (LoadImage is
   only safe before the main render loop begins). */
void item_pickups_load_textures(void);

/* Place a collectible into the live array. Returns its index, or -1 if full.
   The plain form grants the kind's default (ROUNDS_PER_PICKUP for rounds); the
   _amount form sets how many rounds this particular box carries, so a room can
   hand out a token pickup (a single reload) without changing every other one. */
int  item_pickup_spawn(int32_t x, int32_t y, int32_t z, PickupKind kind);
int  item_pickup_spawn_amount(int32_t x, int32_t y, int32_t z, PickupKind kind,
                              int32_t amount);
/* As _amount, but with this pickup's own collect range (Manhattan, X/Z). Pass
   0 for radius to fall back to ITEM_PICKUP_RADIUS. */
int  item_pickup_spawn_range(int32_t x, int32_t y, int32_t z, PickupKind kind,
                             int32_t amount, int32_t radius);

/* Set one pickup's display overrides (see the notes on the struct above). Pass
   the index a spawn returned; a negative index is a no-op, so the return value
   of a spawn into a full array can be handed straight in. 0 for either field
   means "leave it at the default". */
void item_pickup_set_display(int index, int32_t world_half, int32_t otz_bias);

void item_pickups_update(void);
void item_pickups_draw(RenderContext *ctx);
void item_pickups_reset(void);   /* also resets the weapon/rounds inventory */

#endif
