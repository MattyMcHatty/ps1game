#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include "title.h"   /* GameState */

/*
 * Per-room persistent world state — the template for multi-room progress.
 *
 * The live entity arrays (demon_dogs[], crates[], keys[], sml_meds[]) and the
 * delivery door_state always hold the CURRENT room's entities. This module
 * keeps a saved copy of every room's state and swaps it in/out on area
 * transitions:
 *     world_leave(old)  snapshots the live arrays into the old room's slot
 *     world_enter(new)  restores the new room's slot into the live arrays
 * A room's first visit starts empty (the arrays are cleared); when a room gains
 * its own entities, populate them on first visit in world_enter().
 *
 * This is exactly the unit a future SAVE system serializes: the player's own
 * state (player_keys / player_health, kept separately) plus every room's slot.
 *
 * Adding a room: add it to room_index(), bump WORLD_NUM_ROOMS below, and add
 * its spawns to world_seed_room().
 */
#define WORLD_NUM_ROOMS 18  /* delivery_area, kitchen_dining, reception, piano_room,
                               conservatory, hall_2f, master_bedroom, east_hall,
                               library, east_stairwell, attic_stairwell,
                               attic_exit, garden_stairs, garden_courtyard,
                               fountain_square, outside_catacombs, maze_one,
                               maze_two.
                               NOTE the WorldDelta's `visited` is the bitmap that
                               caps this. It was a uint16_t and 16 was the
                               ceiling; Maze One is the room that hit it, so it
                               is now a uint32_t and the ceiling is 32. The
                               _Static_assert in world.c holds the two in step —
                               widen it again, don't just bump this. */

void world_new_game(void);          /* reset all rooms; capture the starting room */
void world_leave(GameState area);   /* live entities  -> the area's saved slot */
void world_enter(GameState area);   /* the area's saved slot -> live entities   */

/* Build a room's entities into the live arrays exactly as a FIRST VISIT leaves
   them. May not touch the current room's mesh or collision data — it is called
   for rooms that are not loaded (see world.c). */
void world_seed_room(GameState area);

/* Stop every monster sound at once, including the latched hardware loops (the
   spider scuttle and tentacle writhe). Called by door_anim_start() and
   stair_anim_start() so nothing carries over into the next room. */
void world_silence_monsters(void);

/* ---- Save delta -------------------------------------------------------------
 * What actually goes on the memory card. NOT a snapshot of the world: the
 * entities themselves are rebuilt by world_seed_room() from constants in the
 * code, and only the things the PLAYER changed are stored.
 *
 * That works because world_leave() — which savegame_capture() calls first —
 * normalises the room on the way out: every still-living enemy is put back at
 * its spawn, at full health, dormant, with all AI scratch cleared. So a living
 * enemy is byte-identical to a freshly seeded one and carries no information,
 * and a dead one is skipped by every draw/update/collide path and so carries
 * exactly one bit. Positions, spawn points, timers, animation counters and
 * knockback velocities are all re-derivable, which is why storing the raw
 * arrays cost 37,920 bytes (five card blocks) to hold a few hundred bits.
 *
 * ADDING A ROOM: bump WORLD_NUM_ROOMS. That grows the delta by 10 bytes and
 * costs no extra card block — one block holds 7,808 bytes of delta.
 * ADDING AN ENTITY TYPE: give it a field here and handle it in the encode and
 * decode halves of world.c, and raise the matching WD_MAX_* if its array is
 * bigger than the bit width below.
 */
#define WD_MAX_PER_ROOM   8   /* bits for zombies / dogs / meds / item pickups */
#define WD_MAX_CRATES    16
#define WD_MAX_KEYS       8
#define WD_MAX_FATDOORS  10
#define WD_MAX_TENTACLES 10
#define WD_MAX_SPIDERS    8   /* bits: one global area-tagged array */
#define WD_MAX_RABISUS    8
#define WD_MAX_MUSHROOMS  8   /* bits: likewise one global area-tagged array */
#define WD_MAX_LIVING_STATUES 8 /* bits: likewise. Only the LIVING ones can die, but
                                   the bit index counts every placement, so plain
                                   masonry occupies a bit it never sets. */

typedef struct {
    uint8_t  zombies_dead;    /* bit i: zombies[i] was killed        */
    uint8_t  dogs_dead;       /* bit i: demon_dogs[i] was killed     */
    uint16_t crates_smashed;  /* bit i: crates[i] was smashed        */
    uint16_t crate_drops_gone;/* bit i: and crate i's drop was taken */
    uint8_t  keys_gone;       /* bit i: keys[i] was picked up        */
    uint8_t  meds_gone;       /* bit i: sml_meds[i] was picked up    */
    uint8_t  items_gone;      /* bit i: item_pickups[i] was taken    */
    uint8_t  door_state;      /* DoorState                           */
} RoomDelta;                  /* 10 bytes */

typedef struct {
    /* bit r: room r entered. uint32_t, not uint16_t: this is the field that
       caps WORLD_NUM_ROOMS, and the sixteenth room filled the old one exactly.
       Widening it costs two bytes of a delta that has thousands spare. */
    uint32_t  visited;
    uint8_t   spiders_dead;                       /* keyed by (area, ordinal) */
    uint8_t   rabisus_dead;                       /* likewise                 */
    uint8_t   mushrooms_dead;                     /* likewise                 */
    uint8_t   living_statues_dead;                /* likewise                 */
    RoomDelta rooms[WORLD_NUM_ROOMS];
    uint8_t   fatdoor_health[WD_MAX_FATDOORS];    /* 0 = smashed              */
    uint8_t   tentacle_health[WD_MAX_TENTACLES];  /* 0 = killed               */
} WorldDelta;

/* Encode the current world into `d`. Call after world_leave() has flushed the
   room the player is standing in (savegame_capture does this). */
void world_save_delta(WorldDelta *d);

/* Rebuild the whole world from `d`, replacing whatever is there. Assumes a
   fresh world_new_game() has run and the player's own state (inventory, flags)
   has ALREADY been restored — nothing in here touches player state, precisely
   so it cannot undo that. Follow it with world_enter(saved area). */
void world_load_delta(const WorldDelta *d);

#endif
