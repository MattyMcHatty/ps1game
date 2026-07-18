#ifndef WORLD_H
#define WORLD_H

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
 * Adding a room: add it to room_index() and bump WORLD_NUM_ROOMS in world.c.
 */
void world_new_game(void);          /* reset all rooms; capture the starting room */
void world_leave(GameState area);   /* live entities  -> the area's saved slot */
void world_enter(GameState area);   /* the area's saved slot -> live entities   */

/* Raw access to the whole rooms[] array for the save system: savegame.c writes
   the blob into the save block and installs a loaded one over it. After
   world_install, call world_enter(saved area) to refresh the live arrays. */
void *world_blob(void);
int   world_blob_size(void);
void  world_install(const void *blob);

#endif
