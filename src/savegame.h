#ifndef SAVEGAME_H
#define SAVEGAME_H

#include <stdint.h>

/* The game's on-card save: what we serialise into one 128-byte data frame, plus
   the helpers that manage the PlayStation directory structure around it. This is
   SAVE only; a matching load path (title-screen "Continue") can read the same
   SaveData frame later. */

#define SAVE_MAGIC     0x47524F56u   /* 'VORG' — our save signature */
#define SAVE_VERSION   4             /* v4: adds the non-key item inventory
                                        (v3: two chained blocks; v2: single block) */
#define SAVE_MAX_SLOTS 15            /* blocks 1..15 are usable for saves */

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t  area;                  /* GameState of the room being saved in */
    int32_t  cam_x, cam_y, cam_z, cam_rot;
    int32_t  health;
    int32_t  rounds;                /* reserve ammo */
    int32_t  loaded;                /* rounds in the Grave-olver cylinder */
    int32_t  weapons;               /* owned-weapon bitmask */
    int32_t  keys;                  /* held-key bitmask */
    int32_t  items;                 /* held non-key item bitmask (player_items) */
    uint32_t counter;               /* playthrough save count INCLUDING this save
                                       (mirrors player_save_count; restored on load) */
    uint32_t world_size;            /* byte size of the world blob stored in the
                                       frames after this one; must equal
                                       world_blob_size() for the save to load
                                       (a layout change invalidates old saves) */
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
