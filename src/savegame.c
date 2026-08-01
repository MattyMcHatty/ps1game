#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "memcard.h"
#include "savegame.h"
#include "player.h"
#include "camera.h"
#include "title.h"
#include "world.h"

/* ---- PlayStation memory-card directory format ------------------------------
   Block 0 is the directory. Frame 0 is the card header ("MC" magic). Frames
   1..15 are one directory entry per data block (block N <-> frame N). A data
   block's own frame 0 is a "title frame" ("SC" magic + a Shift-JIS title + a
   16x16 icon) that the console's card manager displays.

   A save spans SEVERAL chained blocks when the world blob outgrows one (it did
   at 4 rooms, and outgrew two at 7). The chain uses the card's native directory
   format — a state-0x51 first block whose next-block field points at a 0x52
   middle block, and so on to a 0x53 tail — so the console's card manager sees
   one N-block file. Only the first block carries the filename/title/icon;
   savegame_list naturally skips the rest (no filename prefix). */

/* Directory-entry state byte (frame[0]). */
#define DIR_STATE_USED   0x51   /* in use, first block of a file */
#define DIR_STATE_MID    0x52   /* in use, middle block of a chained file */
#define DIR_STATE_LAST   0x53   /* in use, last block of a chained file */
#define DIR_STATE_FREE   0xA0   /* available */

/* All our files share this product-code prefix so listing/overwrite only ever
   touches OUR saves, never another game's on the same card. */
#define SAVE_PREFIX      "BESLES-00000GRV"

/* Layout within a save block. */
#define TITLE_FRAME       0     /* "SC" + title + icon clut  */
#define ICON_FRAME        1     /* 16x16 4bpp icon bitmap    */
#define DATA_FRAME        2     /* our SaveData              */
#define WORLD_FRAME0      3     /* per-room world blob, ceil(world_size/128) frames */

/* World-blob capacity: the rest of the first block, then whole chained blocks
   as needed. world_blob_size() must fit in WORLD_MAX_BYTES (see savegame.h,
   which world.c static-asserts against). */
#define WORLD_BLOCKS      SAVE_WORLD_BLOCKS
#define WORLD_FRAMES_B0   (MC_FRAMES_PER_BLOCK - WORLD_FRAME0)   /* 61 frames  */
#define WORLD_MAX_FRAMES  (WORLD_FRAMES_B0 + (WORLD_BLOCKS - 1) * MC_FRAMES_PER_BLOCK)
#define WORLD_MAX_BYTES   (WORLD_MAX_FRAMES * MC_FRAME_SIZE)

/* LBA of world-blob frame f within a chain of block numbers: frames fill the
   first block from WORLD_FRAME0, then each subsequent block from its frame 0. */
static int world_frame_lba(const int *blocks, int f) {
    if (f < WORLD_FRAMES_B0)
        return blocks[0] * MC_FRAMES_PER_BLOCK + WORLD_FRAME0 + f;
    int rest = f - WORLD_FRAMES_B0;
    return blocks[1 + rest / MC_FRAMES_PER_BLOCK] * MC_FRAMES_PER_BLOCK
         + (rest % MC_FRAMES_PER_BLOCK);
}

/* How many card blocks a blob of this size occupies (1..WORLD_BLOCKS). */
static int world_blocks_needed(uint32_t world_size) {
    int nf = ((int)world_size + MC_FRAME_SIZE - 1) / MC_FRAME_SIZE;
    if (nf <= WORLD_FRAMES_B0) return 1;
    return 1 + (nf - WORLD_FRAMES_B0 + MC_FRAMES_PER_BLOCK - 1) / MC_FRAMES_PER_BLOCK;
}

/* Directory chain: bytes 8-9 of an entry hold the 0-based next block number
   (block# - 1), 0xFFFF = end of chain. Returns the next block 1..15, or 0. */
static int dir_next_block(const uint8_t *dir) {
    int nxt = dir[8] | (dir[9] << 8);
    if (nxt == 0xFFFF) return 0;
    nxt += 1;
    if (nxt < 1 || nxt > SAVE_MAX_SLOTS) return 0;
    return nxt;
}

/* ---- Save-block icon (cosmetic; shown in the console card manager) ---------
   16 colours, BGR555. 0 = transparent black, 1 = purple, 2 = white. */
static const uint16_t icon_clut[16] = {
    0x0000, 0x70A5, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

/* Build the 16x16 4bpp icon (128 bytes, two pixels per byte, low nibble = left
   pixel): a white border with a purple fill. */
static void build_icon(uint8_t *frame) {
    int y, x;
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x += 2) {
            int edge0 = (x == 0 || y == 0 || y == 15);
            int edge1 = (x + 1 == 15 || y == 0 || y == 15);
            uint8_t lo = edge0 ? 2 : 1;
            uint8_t hi = edge1 ? 2 : 1;
            frame[(y * 16 + x) / 2] = (uint8_t)((hi << 4) | lo);
        }
    }
}

void savegame_capture(SaveData *sd) {
    /* The current room's live entities are normally only snapshotted into its
       rooms[] slot when the player LEAVES it — flush them now so the world blob
       serialised alongside this capture reflects the moment of saving. */
    world_leave(current_area);

    memset(sd, 0, sizeof(*sd));
    sd->magic   = SAVE_MAGIC;
    sd->version = SAVE_VERSION;
    sd->area    = (int32_t)current_area;
    sd->cam_x   = cam_x;
    sd->cam_y   = cam_y;
    sd->cam_z   = cam_z;
    sd->cam_rot = cam_rot;
    sd->health  = player_health;
    {
        int a;
        for (a = 0; a < MAX_AMMO_TYPES; a++) sd->ammo[a] = player_ammo[a];
    }
    sd->loaded      = graveolver_loaded;
    sd->loaded_type = (int32_t)graveolver_ammo;
    sd->weapons = player_weapons;
    sd->keys    = player_keys;
    sd->items   = player_items;
    sd->flags   = game_flags;
    sd->counter = 0;
    sd->world_size = (uint32_t)world_blob_size();
}

/* True if a directory frame is one of ours (used, with our filename prefix). */
static int dir_is_ours(const uint8_t *dir) {
    if ((dir[0] & 0xF0) != 0x50) return 0;   /* not in use */
    return memcmp(dir + 10, SAVE_PREFIX, sizeof(SAVE_PREFIX) - 1) == 0;
}

/* Decode a save block's title frame into an ASCII string (Shift-JIS, but our
   titles are plain ASCII, a Shift-JIS subset). Falls back to "SAVE" on error. */
static void read_title(int port, int block, char *out /*33*/) {
    uint8_t frame[128];
    strcpy(out, "SAVE");
    if (memcard_read_frame(port, block * MC_FRAMES_PER_BLOCK + TITLE_FRAME, frame) != MC_OK)
        return;
    if (frame[0] != 'S' || frame[1] != 'C') return;
    int i;
    for (i = 0; i < 32; i++) {
        uint8_t c = frame[4 + i];
        if (c == 0) break;
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    out[i] = '\0';
}

int savegame_list(int port, SaveSlotInfo *out, int max) {
    uint8_t dir[128];
    int count = 0;

    memcard_begin();
    int b;
    for (b = 1; b <= SAVE_MAX_SLOTS && count < max; b++) {
        int rc = memcard_read_frame(port, b, dir);   /* dir frame b == block b */
        if (rc == MC_NO_CARD || rc == MC_TIMEOUT) { memcard_end(); return rc; }
        if (rc != MC_OK) continue;                   /* bad frame: skip it */
        if (!dir_is_ours(dir)) continue;
        out[count].used  = 1;
        out[count].block = b;
        read_title(port, b, out[count].title);
        count++;
    }
    memcard_end();
    return count;
}

int savegame_free_block(int port) {
    uint8_t dir[128];
    /* A new save claims extra blocks when the world blob spills past the first,
       so only offer "new save" if enough free blocks exist. */
    int needed = world_blocks_needed((uint32_t)world_blob_size());
    int found = 0, lowest = 0;

    memcard_begin();
    int b;
    for (b = 1; b <= SAVE_MAX_SLOTS && found < needed; b++) {
        int rc = memcard_read_frame(port, b, dir);
        if (rc == MC_NO_CARD || rc == MC_TIMEOUT) { memcard_end(); return rc; }
        if (rc != MC_OK) continue;
        if ((dir[0] & 0xF0) != 0x50) {   /* free */
            if (!lowest) lowest = b;
            found++;
        }
    }
    memcard_end();
    return (found >= needed) ? lowest : 0;   /* 0 = card full */
}

/* XOR checksum over bytes [0..126], stored at [127] (directory-frame format). */
static void set_dir_checksum(uint8_t *frame) {
    uint8_t cs = 0;
    for (int i = 0; i < 127; i++) cs ^= frame[i];
    frame[127] = cs;
}

/* Format a blank card just enough for our own saves: write the "MC" header and
   mark every directory entry free. Only called when frame 0 lacks the magic. */
static int format_card(int port) {
    uint8_t frame[128];
    int i;

    memset(frame, 0, sizeof(frame));
    frame[0] = 'M'; frame[1] = 'C';
    set_dir_checksum(frame);
    if (memcard_write_frame(port, 0, frame) != MC_OK) return MC_BAD_DATA;

    for (i = 1; i <= SAVE_MAX_SLOTS; i++) {
        memset(frame, 0, sizeof(frame));
        frame[0] = DIR_STATE_FREE;
        frame[4] = 0x00; frame[5] = 0x00;   /* size 0 */
        frame[8] = 0xFF; frame[9] = 0xFF;   /* no next block */
        set_dir_checksum(frame);
        if (memcard_write_frame(port, i, frame) != MC_OK) return MC_BAD_DATA;
    }
    return MC_OK;
}

/* World blob staging: savegame_read parks the loaded rooms[] image here; it is
   installed by savegame_apply_pending once the destination area is set up. */
static uint8_t staged_world[WORLD_MAX_BYTES];
static int     world_staged = 0;

int savegame_read(int port, int block, SaveData *sd) {
    uint8_t frame[128];
    if (block < 1 || block > SAVE_MAX_SLOTS) return MC_BAD_DATA;

    world_staged = 0;
    memcard_begin();

    int rc = memcard_read_frame(port, block * MC_FRAMES_PER_BLOCK + DATA_FRAME, frame);
    if (rc != MC_OK) { memcard_end(); return rc; }
    memcpy(sd, frame, sizeof(*sd));
    /* world_size must match the CURRENT rooms[] layout: a save from an older
       build (different entity structs) cannot be installed safely. */
    if (sd->magic != SAVE_MAGIC || sd->version != SAVE_VERSION ||
        sd->world_size != (uint32_t)world_blob_size()) {
        memcard_end(); return MC_BAD_DATA;
    }

    /* Walk the block chain from the save's own directory entry, so the chain
       travels with the file. Only the head carries our filename; the middle and
       tail entries are plain 0x52/0x53 continuations. */
    int blocks[WORLD_BLOCKS];
    int nb = world_blocks_needed(sd->world_size);
    blocks[0] = block;
    for (int i = 1; i < nb; i++) {
        rc = memcard_read_frame(port, blocks[i - 1], frame);
        if (rc != MC_OK) { memcard_end(); return rc; }
        if (i == 1 && !dir_is_ours(frame)) { memcard_end(); return MC_BAD_DATA; }
        int nxt = dir_next_block(frame);
        if (!nxt) { memcard_end(); return MC_BAD_DATA; }
        for (int j = 0; j < i; j++)
            if (blocks[j] == nxt) { memcard_end(); return MC_BAD_DATA; }
        blocks[i] = nxt;
    }

    int total = (int)sd->world_size;
    int nf    = (total + MC_FRAME_SIZE - 1) / MC_FRAME_SIZE;
    for (int f = 0; f < nf; f++) {
        rc = memcard_read_frame(port, world_frame_lba(blocks, f), frame);
        if (rc != MC_OK) { memcard_end(); return rc; }
        int off = f * MC_FRAME_SIZE;
        int n   = total - off;
        if (n > MC_FRAME_SIZE) n = MC_FRAME_SIZE;
        memcpy(staged_world + off, frame, n);
    }

    memcard_end();
    world_staged = 1;
    return MC_OK;
}

/* ---- Staged load (see savegame.h) ------------------------------------------ */
static SaveData staged_load;
static int      load_pending = 0;

void savegame_stage_load(const SaveData *sd) {
    staged_load  = *sd;
    load_pending = 1;
}

void savegame_apply_pending(void) {
    if (!load_pending) return;
    load_pending = 0;

    SaveData *sd = &staged_load;
    cam_x   = sd->cam_x;
    cam_y   = sd->cam_y;
    cam_z   = sd->cam_z;
    cam_rot = sd->cam_rot;
    cam_vy  = 0;
    player_health     = sd->health;
    {
        int a;
        for (a = 0; a < MAX_AMMO_TYPES; a++) player_ammo[a] = sd->ammo[a];
    }
    graveolver_loaded = sd->loaded;
    /* Clamp the chambered type: a corrupt or out-of-range value would index
       ammo_info[] out of bounds every frame in the HUD. */
    graveolver_ammo   = (sd->loaded_type >= 0 && sd->loaded_type < MAX_AMMO_TYPES)
                        ? (AmmoType)sd->loaded_type : AMMO_STANDARD;
    player_weapons    = sd->weapons;
    player_keys       = sd->keys;
    player_items      = sd->items;
    game_flags        = sd->flags;
    player_save_count = (int)sd->counter;

    /* Install the saved per-room world state over the fresh rooms[] the new-game
       path just built, then re-enter the saved area so the live entity arrays
       (enemies, crates, pickups, door state) come from the LOADED slot rather
       than the room init's defaults. */
    if (world_staged) {
        world_install(staged_world);
        world_staged = 0;
        world_enter((GameState)sd->area);
    }
}

int savegame_write(int port, int block, const SaveData *sd, const char *title) {
    uint8_t frame[128];
    char    filename[21];

    if (block < 1 || block > SAVE_MAX_SLOTS) return MC_BAD_DATA;

    memcard_begin();

    /* Ensure the card is formatted (has the "MC" header); format if blank. */
    if (memcard_read_frame(port, 0, frame) != MC_OK || frame[0] != 'M' || frame[1] != 'C') {
        if (format_card(port) != MC_OK) { memcard_end(); return MC_BAD_DATA; }
    }

    /* --- Continuation blocks, when the world blob spills past the first ---
       Overwriting one of our own chained saves reuses its existing continuation
       blocks; any still missing are claimed from the lowest free ones. Not
       enough free blocks -> card full. */
    int nb = world_blocks_needed(sd->world_size);
    int blocks[WORLD_BLOCKS];
    blocks[0] = block;
    {
        /* Follow the existing chain as far as it goes. */
        int have = 1;
        while (have < nb) {
            if (memcard_read_frame(port, blocks[have - 1], frame) != MC_OK) break;
            if (have == 1 && !dir_is_ours(frame)) break;
            int nxt = dir_next_block(frame);
            if (!nxt) break;
            int dup = 0;
            for (int j = 0; j < have; j++) if (blocks[j] == nxt) dup = 1;
            if (dup) break;
            blocks[have++] = nxt;
        }
        /* Claim free blocks for the rest. */
        for (int b = 1; b <= SAVE_MAX_SLOTS && have < nb; b++) {
            int used = 0;
            for (int j = 0; j < have; j++) if (blocks[j] == b) used = 1;
            if (used) continue;
            if (memcard_read_frame(port, b, frame) != MC_OK) continue;
            if ((frame[0] & 0xF0) != 0x50) blocks[have++] = b;   /* free */
        }
        if (have < nb) { memcard_end(); return MC_BAD_DATA; }
    }

    /* --- Directory entries (block 0, frame `blocks[i]`) ---
       Head carries the filename/state 0x51; continuations are 0x52 (middle) and
       0x53 (tail) with no filename, so savegame_list skips them. */
    for (int i = 0; i < nb; i++) {
        memset(frame, 0, sizeof(frame));
        frame[0] = (i == 0)      ? DIR_STATE_USED
                 : (i == nb - 1) ? DIR_STATE_LAST
                                 : DIR_STATE_MID;
        /* File size in bytes (0x2000 per block), little-endian at [4..7]. */
        {
            uint32_t sz = (uint32_t)nb * 0x2000u;
            frame[4] = (uint8_t)(sz);       frame[5] = (uint8_t)(sz >> 8);
            frame[6] = (uint8_t)(sz >> 16); frame[7] = (uint8_t)(sz >> 24);
        }
        if (i + 1 < nb) { frame[8] = (uint8_t)(blocks[i + 1] - 1); frame[9] = 0x00; }
        else            { frame[8] = 0xFF; frame[9] = 0xFF; }   /* end of chain */
        if (i == 0) {
            snprintf(filename, sizeof(filename), "%s%02d", SAVE_PREFIX, block);
            memcpy(frame + 10, filename, strlen(filename));     /* NUL-terminated by memset */
        }
        set_dir_checksum(frame);
        if (memcard_write_frame(port, blocks[i], frame) != MC_OK) {
            memcard_end(); return MC_BAD_DATA;
        }
    }

    /* --- Title frame (block, frame 0): "SC" magic + title + icon palette --- */
    memset(frame, 0, sizeof(frame));
    frame[0] = 'S'; frame[1] = 'C';
    frame[2] = 0x11;   /* icon: 1 frame */
    frame[3] = 0x01;   /* 1 block */
    {
        int n = title ? (int)strlen(title) : 0;
        if (n > 32) n = 32;
        if (title) memcpy(frame + 4, title, n);   /* ASCII == Shift-JIS subset */
    }
    memcpy(frame + 96, icon_clut, sizeof(icon_clut));   /* clut at offset 96 */
    if (memcard_write_frame(port, block * MC_FRAMES_PER_BLOCK + TITLE_FRAME, frame) != MC_OK) {
        memcard_end(); return MC_BAD_DATA;
    }

    /* --- Icon bitmap frame --- */
    memset(frame, 0, sizeof(frame));
    build_icon(frame);
    if (memcard_write_frame(port, block * MC_FRAMES_PER_BLOCK + ICON_FRAME, frame) != MC_OK) {
        memcard_end(); return MC_BAD_DATA;
    }

    /* --- Save-data frame --- */
    memset(frame, 0, sizeof(frame));
    memcpy(frame, sd, sizeof(*sd));
    if (memcard_write_frame(port, block * MC_FRAMES_PER_BLOCK + DATA_FRAME, frame) != MC_OK) {
        memcard_end(); return MC_BAD_DATA;
    }

    /* --- Per-room world blob, 128 bytes per frame across the block chain --- */
    {
        const uint8_t *wb    = (const uint8_t *)world_blob();
        int            total = (int)sd->world_size;
        if (total > WORLD_MAX_BYTES) { memcard_end(); return MC_BAD_DATA; }
        int nf = (total + MC_FRAME_SIZE - 1) / MC_FRAME_SIZE;
        for (int f = 0; f < nf; f++) {
            int off = f * MC_FRAME_SIZE;
            int n   = total - off;
            if (n > MC_FRAME_SIZE) n = MC_FRAME_SIZE;
            memset(frame, 0, sizeof(frame));
            memcpy(frame, wb + off, n);
            if (memcard_write_frame(port,
                    world_frame_lba(blocks, f), frame) != MC_OK) {
                memcard_end(); return MC_BAD_DATA;
            }
        }
    }

    memcard_end();
    return MC_OK;
}
