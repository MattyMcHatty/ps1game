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
#define WORLD_NUM_ROOMS 65  /* delivery_area, kitchen_dining, reception, piano_room,
                               conservatory, hall_2f, master_bedroom, east_hall,
                               library, east_stairwell, attic_stairwell,
                               attic_exit, garden_stairs, garden_courtyard,
                               fountain_square, outside_catacombs, maze_one,
                               maze_two, rear_gate, west_corridor,
                               library_destroyed, stables, keystone_maze,
                               greenhouse, chain_room, the_hatch, asag_arena,
                               catacombs_entry, up_down_maze, incinerator_room,
                               tomb, room_of_arms, the_pit,
                               north_chamber, room_of_heads, cleaver_corridor,
                               crucifix_corridor, sliding_bars_room,
                               room_of_legs, meat_plant, room_of_bones,
                               cleaver_l, zig_zag_tomb, h_corridor,
                               room_of_torsos, the_shelf, gaol_entry,
                               gaol_cells, nursery, room_of_baby_names,
                               and the five Dead Ends (benj, matthew,
                               christof, luke, mark) - one mesh, five slots,
                               because each keeps its own pickups - the
                               throat, room_of_guts, neck, the_head, kennels,
                               lamashtu_arena and seal, and Chapter 4's
                               hell_hath_no_fury, sharurs_trial and lava_u. (Descent to Hell has no
                               slot - see area_untracked in world.c.)
                               library_destroyed gets a slot of its own even
                               though it stands in the Library's place: the two
                               are alternative rooms behind the same doors, and
                               each has to remember separately what the player
                               killed and took in it.
                               NOTE the WorldDelta's `visited` is the bitmap that
                               caps this. It was a uint16_t and 16 was the
                               ceiling; Maze One is the room that hit it, so it
                               is now a uint32_t and the ceiling is 32. The
                               _Static_assert in world.c holds the two in step —
                               widen it again, don't just bump this.
                               >>> IT IS 64 NOW, AND THE PREDICTION ABOVE CAME
                               TRUE ON THE VERY NEXT ROOM. <<< The Room of Arms
                               was the thirty-second and filled the uint32_t
                               exactly; THE PIT is the thirty-third, the assert
                               fired, and `visited` is a uint64_t — as is
                               `cribs_solved`, which is capped by the same number
                               and had to be widened beside it. The ceiling is 64.
                               No SAVE_VERSION bump was needed for it: sizeof
                               WorldDelta moved (one more RoomDelta plus eight
                               bytes of bitmap), so the delta_size check in
                               savegame.c rejects every older save cleanly, which
                               is what a room addition does anyway. Do not reach
                               for a narrower trick when 64 fills up either.
                               >>> 64 FILLED WITH SHARUR'S TRIAL, AND LAVA U IS
                               THE SIXTY-FIFTH. <<< `visited` and `cribs_solved`
                               are RoomBits now (below), sized from this number,
                               so there is no ceiling left to hit. */

/* This area's slot number, 0..WORLD_NUM_ROOMS-1. The public face of world.c's
   room_index(), for anything outside that file that has to key persistent state
   by ROOM rather than by (room, ordinal) — today, the crib encounters'
   solved set (src/crib.h).

   >>> AN AREA WITH NO SLOT OF ITS OWN COMES BACK AS 0, THE DELIVERY AREA'S. <<<
   That is room_index()'s default and it is deliberate there; it makes this a
   sharp edge for a caller storing something permanent, so only ever pass a
   GameState that is a real room. Menu and cutscene states are not. */
int  world_room_index(GameState area);

/* ---- A room bitmap: one bit per room, as wide as WORLD_NUM_ROOMS -----------
 * WorldDelta.visited and WorldDelta.cribs_solved (and the crib module's live
 * solved set) are RoomBits. They were a uint16_t, then a uint32_t, then a
 * uint64_t, and each of those filled up exactly (Maze One, The Pit, then Lava U
 * as room 65), so the width is now DERIVED from WORLD_NUM_ROOMS and adding a
 * room never has to widen anything again.
 *
 * >>> A 64-BIT SHIFT BY A *VARIABLE* DOES NOT LINK IN THIS TOOLCHAIN. <<< It
 * compiles to a call to libgcc's __ashldi3 / __lshrdi3, and -nostdlib means there
 * is no libgcc to call. Words of 32 bits sidestep that entirely: every shift
 * here is a plain 32-bit one. (collision.c's walls_crossed() records the same
 * constraint for 64-bit DIVIDE.) `r` must be a valid room index; anything from
 * world_room_index() is.
 */
#define WORLD_ROOM_WORDS ((WORLD_NUM_ROOMS + 31) / 32)
typedef struct { uint32_t w[WORLD_ROOM_WORDS]; } RoomBits;

static inline int room_bits_test(const RoomBits *b, int r) {
    return (int)((b->w[r >> 5] >> (r & 31)) & 1u);
}
static inline void room_bits_set(RoomBits *b, int r) {
    b->w[r >> 5] |= 1u << (r & 31);
}

void world_new_game(void);          /* reset all rooms; capture the starting room */
void world_leave(GameState area);   /* live entities  -> the area's saved slot */
void world_enter(GameState area);   /* the area's saved slot -> live entities   */

/* DEBUG (DBG_PEARL_*): mark the Blood Pearl on `area`'s sconce as already
   collected. Deactivated wherever the room's pickups live right now — the live
   array if it is the current room, its saved slot if visited — and, for a room
   not yet seeded, on its first world_enter. The record is kept, inactive, so
   the sconce lights (src/sconce.c) and the save's items_gone bit is set as if
   it had been picked up. Cleared by world_new_game(). */
void world_take_sconce_pearl(GameState area);

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
#define WD_MAX_CRAWLERS  16   /* bits: likewise. 16 rather than 8 since the North
                                 Chamber's two took the whole-game total to nine
                                 — which is why crawlers_dead below is a
                                 uint16_t, the widening mushrooms_dead took. */
#define WD_MAX_RABISUS    8
#define WD_MAX_NESSIES    8   /* bits: one global area-tagged array, one byte */
/* bits: likewise one global area-tagged array. 16 rather than 8 since The
   Shelf's two took the whole-game total to nine — which is why lumberers_dead
   below is a uint16_t, the widening crawlers_dead took (SAVE_VERSION 33). */
#define WD_MAX_LUMBERERS 16
/* Asag. Eight like the Rabisu's, and eight is generous: the arena is sealed and
   holds one. It is a whole byte either way — the point of the number is the
   ceiling it puts on placements, not the storage. */
#define WD_MAX_ASAGS      8
#define WD_MAX_MUSHROOMS 16   /* bits: likewise one global area-tagged array. 16
                                 rather than 8 since the Greenhouse's flood
                                 brought the whole-game total to nine — which is
                                 why mushrooms_dead below is a uint16_t while
                                 its neighbours are still bytes. */
#define WD_MAX_LIVING_STATUES 16 /* bits: likewise. Only the LIVING ones can die,
                                   but the bit index counts every placement, so
                                   plain masonry occupies a bit it never sets.
                                   16 rather than 8 since The Hatch put a statue
                                   on each of its four corner plinths and took
                                   the whole-game total to ten — which is why
                                   living_statues_dead below is a uint16_t, the
                                   same widening mushrooms_dead took. */
/* Hadad needs TWO bits per placement, not one: as well as being dead he can be
   SPENT — the flag-three encounter has been met and burned out, which is the one
   fact about him that the two GameFlags cannot reconstruct (see hadads_rest).
   So his field is packed as bit (2*i) = dead, bit (2*i + 1) = spent.

   >>> IT WAS A uint8_t AND FOUR PLACEMENTS FILLED IT EXACTLY. <<< The West
   Corridor's second encounter (HAD_ROLE_WEST_CORR_RET) is the fifth, so the
   field is now a uint16_t and this is 8. Widening it changed the WorldDelta's
   SHAPE, so delta_size already rejects an older save — the SAVE_VERSION 17 bump
   in savegame.h is what makes the reason legible. */
#define WD_MAX_HADADS         8

/* The vine curtains, and the valve handle's mounts. Both are global area-tagged
   arrays placed once at startup, exactly like the fat doors, so an array index
   is stable across a save and needs no canonical remap. Both are also tiny, and
   deliberately: MAX_VINES bytes plus one bitmask is what the whole feature costs
   the delta, against the ~2.5 KB the blob has spare. */
#define WD_MAX_VINES          8
#define WD_MAX_VALVE_MOUNTS   8

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
    /* bit r: room r entered. A RoomBits now: this is the field that used to cap
       WORLD_NUM_ROOMS, and it filled up THREE times — the uint16_t, the uint32_t
       and the uint64_t each exactly. Its width follows the room count now. */
    RoomBits  visited;
    uint8_t   spiders_dead;                       /* keyed by (area, ordinal) */
    /* NESSIE (src/nessie.h), keyed like the spiders. >>> IT LIVES IN WHAT WAS
       THE PADDING BYTE BEFORE crawlers_dead, AND THAT IS WHY THERE WAS NO
       SAVE_VERSION BUMP. <<< sizeof and every other field's offset are
       unchanged (world.c asserts both), and world_save_delta memsets the delta
       before filling it, so every older save already holds 0 here — "no Nessie
       dead", which is the truth for a save made before she existed. */
    uint8_t   nessies_dead;                       /* likewise; see WD_MAX_NESSIES */
    uint16_t  crawlers_dead;                      /* likewise; 16 bits — see
                                                     WD_MAX_CRAWLERS           */
    uint8_t   rabisus_dead;                       /* likewise                 */
    uint16_t  lumberers_dead;                     /* likewise; 16 bits — see
                                                     WD_MAX_LUMBERERS          */
    uint16_t  mushrooms_dead;                     /* likewise; 16 bits — see
                                                     WD_MAX_MUSHROOMS          */
    uint16_t  living_statues_dead;                /* likewise; 16 bits — see
                                                     WD_MAX_LIVING_STATUES     */
    /* ASAG. >>> RESERVED, AND NOTHING WRITES IT YET. <<< There is no Asag entity
       and no arena content; this byte exists so that adding them is a change to
       world.c's encode and decode halves ONLY, with no third SAVE_VERSION bump
       and no save invalidated a second time. Keyed by canonical_index() like
       every other global area-tagged array when it arrives — a raw array index
       is not stable across playthroughs that took different routes. */
    uint8_t   asags_dead;                         /* likewise; see WD_MAX_ASAGS */
    uint16_t  hadads_state;                       /* 2 bits each: dead, spent */
    RoomDelta rooms[WORLD_NUM_ROOMS];
    uint8_t   fatdoor_health[WD_MAX_FATDOORS];    /* 0 = smashed              */
    uint8_t   tentacle_health[WD_MAX_TENTACLES];  /* 0 = killed               */
    uint8_t   vine_health[WD_MAX_VINES];          /* 0 = cleared              */
    uint8_t   valve_present;                      /* bit i: mount i is filled */
    /* THE CRIB ENCOUNTERS. bit r: the crib in room r has been beaten — its ten
       Creeps released and killed. Keyed by room_index(), NOT by canonical_index()
       like every entity field above it, and src/crib.h argues why at length: a
       crib is placed by its ROOM's init and cribs_clear() empties the array on
       every room entry, so an instance has no stable whole-game ordinal to be
       keyed by, and the room is both the only stable identity available and the
       one the design cares about.

       >>> IT IS A RoomBits FOR THE SAME REASON `visited` IS. <<< One bit per
       room, sized from WORLD_NUM_ROOMS, so it grows with the room count.

       >>> AND IT IS HERE RATHER THAN IN game_flags BECAUSE game_flags IS FULL.
       <<< That word is 32 bits in SaveData and 31 of them are spent
       (FLAG_ASAG_DEAD is bit 30). A mechanic that is going into several rooms
       needs a SET of bits and there was one left. See src/crib.h. */
    RoomBits  cribs_solved;
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
