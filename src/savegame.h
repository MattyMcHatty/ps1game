#ifndef SAVEGAME_H
#define SAVEGAME_H

#include <stdint.h>
#include "player.h"   /* MAX_AMMO_TYPES: the per-type reserve array below */
#include "menu.h"     /* MENU_ITEM_CELLS: the inventory arrangement below */
#include "world.h"    /* WorldDelta */

/* The game's on-card save, plus the helpers that manage the PlayStation
   directory structure around it.

   ONE BLOCK. A save is a header frame, an icon frame, the SaveData frame and
   then the WorldDelta, which is a few hundred bytes — so it all fits in the 61
   frames a single block has left over. Up to v11 the world went on the card as
   a raw 37,920-byte memcpy of every room's entity arrays and took FIVE blocks;
   see the note at the top of the WorldDelta declaration in world.h for why
   almost none of that carried any information. */

#define SAVE_MAGIC     0x47524F56u   /* 'VORG' — our save signature */
#define SAVE_VERSION   17            /* v17: a FIFTH Hadad — the West Corridor's
                                        RETURN, the corner ambush walked the
                                        other way round under FLAG_HADAD_TWO. It
                                        shares its room with the ambush, so it
                                        takes the bit pair immediately after it
                                        (world.c's seed order IS the tie-break
                                        inside a room) and every LATER Hadad's
                                        pair moves one to the right — the v16
                                        reorder again. It also overflowed
                                        hadads_state, which is now a uint16_t,
                                        so this time the delta_size check would
                                        catch a v16 save on its own; the bump
                                        still makes the reason legible;
                                        v16: a FOURTH Hadad — the Reception's
                                        ceiling drop. The WorldDelta did not
                                        change SHAPE, so the delta_size check
                                        cannot catch this one and the bump is
                                        the only thing that does: hadads_state
                                        indexes instances in room_areas[] order
                                        and Reception (room 2) is EARLIER than
                                        every other Hadad's room, so every
                                        existing save's dead/spent bits now sit
                                        one pair to the left of what they
                                        describe. >>> A SILENT REORDER OF AN
                                        EXISTING FIELD NEEDS A BUMP JUST AS
                                        MUCH AS A NEW ONE DOES. <<<
                                        v15: the WorldDelta gained hadads_state
                                        — TWO bits per instance rather than the
                                        usual one, because Hadad can be spent as
                                        well as dead (see world.h and hadad.h).
                                        Same reasoning as the v14 bump below:
                                        the delta_size check would already have
                                        rejected a v14 save, and the bump makes
                                        the reason legible;
                                        v14: the WorldDelta gained
                                        mushrooms_dead (the Mushroom Head, one
                                        bit per instance of a global
                                        area-tagged array — see world.h). The
                                        delta_size check alone would already
                                        have rejected a v13 save, since the
                                        struct grew a byte; the bump makes the
                                        reason legible rather than implicit;
                                        v13: the inventory GRID ARRANGEMENT
                                        (item_order) — items now land in the
                                        first free cell and the player can
                                        rearrange them, so where each one sits is
                                        no longer implied by its item ID;
                                        v12: one block — the raw world blob is
                                        replaced by a WorldDelta rebuilt through
                                        world_seed_room();
                                        v11: 5-block chain (the garden stairs,
                                        room 12, outgrew four);
                                        v10: persistent puzzle/world flag word
                                        (game_flags — the piano puzzle);
                                        v9: per-type ammo reserves + chambered
                                        type (Flame Rounds); v8: 4-block chain
                                        (the library, room 9, outgrew three);
                                        v7: ItemPickup carries a per-pickup
                                        rounds amount; v6: chain generalised to N
                                        blocks (the 7th room outgrew two);
                                        v5: tentacle array; v4: item inventory;
                                        v3: two blocks */
#define SAVE_MAX_SLOTS 15            /* blocks 1..15 are usable for saves */

/* Bytes a single block has left for the world delta, after the title, icon and
   SaveData frames. savegame.c static-asserts sizeof(WorldDelta) against this —
   at ~160 bytes there is room for roughly fifty times the current world. */
#define SAVE_DELTA_MAX_BYTES  (61 * 128)

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t  area;                  /* GameState of the room being saved in */
    int32_t  cam_x, cam_y, cam_z, cam_rot;
    int32_t  health;
    int32_t  ammo[MAX_AMMO_TYPES];  /* reserve ammo, per AmmoType */
    int32_t  loaded;                /* rounds in the Grave-olver cylinder */
    int32_t  loaded_type;           /* AmmoType currently chambered */
    int32_t  weapons;               /* owned-weapon bitmask */
    int32_t  keys;                  /* held-key bitmask */
    int32_t  items;                 /* held non-key item bitmask (player_items) */
    int32_t  flags;                 /* persistent GameFlag bitmask (game_flags) */
    uint8_t  item_order[MENU_ITEM_CELLS];  /* inventory grid: cell -> item ID + 1,
                                       0 = empty. Purely the ARRANGEMENT; what is
                                       held still comes from keys/items/ammo
                                       above, and menu_inventory_load reconciles
                                       the two. */
    uint32_t counter;               /* playthrough save count INCLUDING this save
                                       (mirrors player_save_count; restored on load) */
    uint32_t delta_size;            /* byte size of the WorldDelta in the frames
                                       after this one; must equal
                                       sizeof(WorldDelta) for the save to load.
                                       Unlike the old world_size this only moves
                                       when a ROOM or an entity CATEGORY is
                                       added — growing an entity struct no longer
                                       invalidates saves. */
} SaveData;                          /* well under 128 bytes */

typedef struct {
    int  used;                      /* 1 if this entry holds one of our saves */
    int  block;                     /* card block 1..15 */
    char title[33];                 /* human title shown in the menu */
} SaveSlotInfo;

/* Snapshot the live game state into sd. */
void savegame_capture(SaveData *sd);

/* Enumerate our saves on the card in `port` into out[0..max-1].
   Returns the count (>= 0) or a negative MC_* error. */
int  savegame_list(int port, SaveSlotInfo *out, int max);

/* Lowest unused block (1..15), 0 if the card is full, or a negative MC_* error. */
int  savegame_free_block(int port);

/* Write sd into `block` (1..15) with the given display title.
   Returns MC_OK or a negative MC_* error. */
int  savegame_write(int port, int block, const SaveData *sd, const char *title);

/* Read the SaveData stored in `block` (1..15), validating magic/version.
   Returns MC_OK or a negative MC_* error. */
int  savegame_read(int port, int block, SaveData *sd);

/* Loading happens in two steps because entering an area overwrites the camera
   and player state: stage_load() stashes a SaveData at selection time (title
   screen), then apply_pending() — called by main.c once the destination area
   is fully initialised — copies it over the live game state. apply_pending()
   is a no-op when nothing is staged, so normal transitions are unaffected. */
void savegame_stage_load(const SaveData *sd);
void savegame_apply_pending(void);

#endif
