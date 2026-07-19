#include <string.h>
#include "world.h"
#include "demondog.h"
#include "zombie.h"
#include "crate.h"
#include "key.h"
#include "sml_med.h"
#include "item_pickup.h"
#include "door.h"
#include "fatdoor.h"

#define WORLD_NUM_ROOMS 4   /* delivery_area, kitchen_dining, reception, piano_room */

/* A saved snapshot of one room's entities. Mirrors the live arrays below; this
   is the per-room unit a save file would store. */
typedef struct {
    int       visited;

    DemonDog  dogs[MAX_DEMON_DOGS];   int dog_count;
    Zombie    zombs[MAX_ZOMBIES];     int zomb_count;
    Crate      crates[MAX_CRATES];         int crate_count;
    KeyPickup  keys[MAX_KEYS];             int key_count;
    SmlMed     meds[MAX_SML_MEDS];         int med_count;
    ItemPickup items[MAX_ITEM_PICKUPS];    int item_count;
    DoorState  door_state;
} RoomState;

/* Everything the save system persists in one contiguous blob: the per-room
   snapshots plus global (non-room-swapped) state. The fatdoors are one global
   array tagged by area — they never pass through world_leave/enter's swapping,
   so they get their own section, mirrored on every leave and restored wholesale
   on install. */
typedef struct {
    RoomState rooms[WORLD_NUM_ROOMS];
    FatDoor   fatdoors[MAX_FATDOORS];
    int       fatdoor_count;
} WorldState;

static WorldState world;

/* Live entity arrays (owned by their modules) — the "working set" for the
   room the player is currently in. */
extern DemonDog  demon_dogs[MAX_DEMON_DOGS];   extern int demon_dog_count;
extern Zombie    zombies[MAX_ZOMBIES];         extern int zombie_count;
extern Crate     crates[MAX_CRATES];           extern int crate_count;
extern KeyPickup keys[MAX_KEYS];               extern int key_count;
extern SmlMed    sml_meds[MAX_SML_MEDS];       extern int sml_med_count;
extern ItemPickup item_pickups[MAX_ITEM_PICKUPS]; extern int item_pickup_count;
extern DoorState door_state;

static int room_index(GameState area) {
    switch (area) {
        case STATE_DELIVERY_AREA:  return 0;
        case STATE_KITCHEN_DINING: return 1;
        case STATE_RECEPTION:      return 2;
        case STATE_PIANO_ROOM:     return 3;
        default:                   return 0;
    }
}

/* live arrays -> room slot */
static void snapshot(RoomState *r) {
    memcpy(r->dogs,   demon_dogs, sizeof demon_dogs); r->dog_count   = demon_dog_count;
    memcpy(r->zombs,  zombies,    sizeof zombies);    r->zomb_count  = zombie_count;
    memcpy(r->crates, crates,     sizeof crates);     r->crate_count = crate_count;
    memcpy(r->keys,   keys,       sizeof keys);       r->key_count   = key_count;
    memcpy(r->meds,   sml_meds,   sizeof sml_meds);   r->med_count   = sml_med_count;
    memcpy(r->items,  item_pickups, sizeof item_pickups); r->item_count = item_pickup_count;
    r->door_state = door_state;
}

/* room slot -> live arrays */
static void restore(const RoomState *r) {
    memcpy(demon_dogs, r->dogs,   sizeof demon_dogs); demon_dog_count = r->dog_count;
    memcpy(zombies,    r->zombs,  sizeof zombies);    zombie_count    = r->zomb_count;
    memcpy(crates,     r->crates, sizeof crates);     crate_count     = r->crate_count;
    memcpy(keys,       r->keys,   sizeof keys);       key_count       = r->key_count;
    memcpy(sml_meds,   r->meds,   sizeof sml_meds);   sml_med_count   = r->med_count;
    memcpy(item_pickups, r->items, sizeof item_pickups); item_pickup_count = r->item_count;
    door_state = r->door_state;
}

void *world_blob(void)      { return &world; }
int   world_blob_size(void) { return (int)sizeof world; }

void world_install(const void *blob) {
    memcpy(&world, blob, sizeof world);
    /* The fatdoor section is global (not per-room-swapped), so restore the live
       array immediately rather than waiting for a world_enter. */
    memcpy(fatdoors, world.fatdoors, sizeof fatdoors);
    fatdoor_count = world.fatdoor_count;
}

/* Mirror the live (global) fatdoor array into the blob's section. */
static void snapshot_fatdoors(void) {
    memcpy(world.fatdoors, fatdoors, sizeof fatdoors);
    world.fatdoor_count = fatdoor_count;
}

void world_new_game(void) {
    memset(&world, 0, sizeof world);
    /* The starting room (delivery) already has its entities set up by the
       startup inits + reset_game, so capture that as its initial state. */
    int d = room_index(STATE_DELIVERY_AREA);
    snapshot(&world.rooms[d]);
    world.rooms[d].visited = 1;
    snapshot_fatdoors();
}

void world_leave(GameState area) {
    /* Enemy wake/chase state never persists: leaving a room — and saving, which
       also snapshots via this function — puts every still-living enemy back at
       its spawn point, asleep, at full health. Deaths stick. */
    zombies_rest();
    demon_dogs_rest();
    snapshot(&world.rooms[room_index(area)]);
    snapshot_fatdoors();
}

void world_enter(GameState area) {
    RoomState *r = &world.rooms[room_index(area)];
    if (r->visited) {
        restore(r);
    } else {
        /* First visit: start empty. A room with its own enemies/crates/pickups
           would populate the live arrays here (e.g. call its *_init), then they
           get snapshotted on the next world_leave(). */
        memset(demon_dogs, 0, sizeof demon_dogs); demon_dog_count = 0;
        memset(zombies,    0, sizeof zombies);    zombie_count    = 0;
        memset(crates,     0, sizeof crates);     crate_count     = 0;
        memset(keys,       0, sizeof keys);       key_count       = 0;
        memset(sml_meds,   0, sizeof sml_meds);   sml_med_count   = 0;
        memset(item_pickups, 0, sizeof item_pickups); item_pickup_count = 0;
        door_state = DOOR_LOCKED;

        /* Seed each room's resident enemies on first entry; they are then
           snapshotted on the next world_leave() and persist (deaths stick). */
        if (area == STATE_KITCHEN_DINING) {
            zombie_add(-309, -149, -1275);
            zombie_add(-892, -149,  -384);
            zombie_add(-2064, -149, -663);
            zombie_add(-2969, -149,  -13);
            zombie_add(-1997, -149,  830);

            /* Floating small medipac. sml_med_spawn adds SML_MED_FLOAT_Y, so
               this floats just above the floor at the requested spot. */
            sml_med_spawn(293, -149, -1399);
        }

        /* Reception: the Grave-olver gun and a box of Standard Rounds sit on top
           of the dresser (at x=580, z=958, rotated 90° so its long axis runs
           along Z), spaced 120 apart along that length and centred on it. */
        if (area == STATE_RECEPTION) {
            item_pickup_spawn(580, -149, 898,  PICKUP_GRAVEOLVER);
            item_pickup_spawn(580, -149, 1018, PICKUP_ROUNDS);
        }

        r->visited = 1;
    }
}
