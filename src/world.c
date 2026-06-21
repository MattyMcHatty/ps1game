#include <string.h>
#include "world.h"
#include "demondog.h"
#include "zombie.h"
#include "crate.h"
#include "key.h"
#include "sml_med.h"
#include "door.h"

#define WORLD_NUM_ROOMS 2   /* delivery_area, kitchen_dining */

/* A saved snapshot of one room's entities. Mirrors the live arrays below; this
   is the per-room unit a save file would store. */
typedef struct {
    int       visited;

    DemonDog  dogs[MAX_DEMON_DOGS];   int dog_count;
    Zombie    zombs[MAX_ZOMBIES];     int zomb_count;
    Crate     crates[MAX_CRATES];     int crate_count;
    KeyPickup keys[MAX_KEYS];         int key_count;
    SmlMed    meds[MAX_SML_MEDS];     int med_count;
    DoorState door_state;
} RoomState;

static RoomState rooms[WORLD_NUM_ROOMS];

/* Live entity arrays (owned by their modules) — the "working set" for the
   room the player is currently in. */
extern DemonDog  demon_dogs[MAX_DEMON_DOGS];   extern int demon_dog_count;
extern Zombie    zombies[MAX_ZOMBIES];         extern int zombie_count;
extern Crate     crates[MAX_CRATES];           extern int crate_count;
extern KeyPickup keys[MAX_KEYS];               extern int key_count;
extern SmlMed    sml_meds[MAX_SML_MEDS];       extern int sml_med_count;
extern DoorState door_state;

static int room_index(GameState area) {
    switch (area) {
        case STATE_DELIVERY_AREA:  return 0;
        case STATE_KITCHEN_DINING: return 1;
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
    r->door_state = door_state;
}

/* room slot -> live arrays */
static void restore(const RoomState *r) {
    memcpy(demon_dogs, r->dogs,   sizeof demon_dogs); demon_dog_count = r->dog_count;
    memcpy(zombies,    r->zombs,  sizeof zombies);    zombie_count    = r->zomb_count;
    memcpy(crates,     r->crates, sizeof crates);     crate_count     = r->crate_count;
    memcpy(keys,       r->keys,   sizeof keys);       key_count       = r->key_count;
    memcpy(sml_meds,   r->meds,   sizeof sml_meds);   sml_med_count   = r->med_count;
    door_state = r->door_state;
}

void world_new_game(void) {
    memset(rooms, 0, sizeof rooms);
    /* The starting room (delivery) already has its entities set up by the
       startup inits + reset_game, so capture that as its initial state. */
    int d = room_index(STATE_DELIVERY_AREA);
    snapshot(&rooms[d]);
    rooms[d].visited = 1;
}

void world_leave(GameState area) {
    snapshot(&rooms[room_index(area)]);
}

void world_enter(GameState area) {
    RoomState *r = &rooms[room_index(area)];
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

        r->visited = 1;
    }
}
