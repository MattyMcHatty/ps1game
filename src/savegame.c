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

   We keep our saves to a single block each (our data is tiny), so every
   directory entry is a self-contained "first and only block". */

/* Directory-entry state byte (frame[0]). */
#define DIR_STATE_USED   0x51   /* in use, first & only block of a file */
#define DIR_STATE_FREE   0xA0   /* available */

/* All our files share this product-code prefix so listing/overwrite only ever
   touches OUR saves, never another game's on the same card. */
#define SAVE_PREFIX      "BESLES-00000GRV"

/* Layout within a save block. */
#define TITLE_FRAME       0     /* "SC" + title + icon clut  */
#define ICON_FRAME        1     /* 16x16 4bpp icon bitmap    */
#define DATA_FRAME        2     /* our SaveData              */
#define WORLD_FRAME0      3     /* per-room world blob, ceil(world_size/128) frames */

/* Frames left in a block for the world blob; world_blob_size() must fit. */
#define WORLD_MAX_BYTES  ((MC_FRAMES_PER_BLOCK - WORLD_FRAME0) * MC_FRAME_SIZE)

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
    sd->rounds  = player_rounds;
    sd->loaded  = graveolver_loaded;
    sd->weapons = player_weapons;
    sd->keys    = player_keys;
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
    memcard_begin();
    int b;
    for (b = 1; b <= SAVE_MAX_SLOTS; b++) {
        int rc = memcard_read_frame(port, b, dir);
        if (rc == MC_NO_CARD || rc == MC_TIMEOUT) { memcard_end(); return rc; }
        if (rc != MC_OK) continue;
        if ((dir[0] & 0xF0) != 0x50) { memcard_end(); return b; }  /* free */
    }
    memcard_end();
    return 0;   /* card full */
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

    int total = (int)sd->world_size;
    int nf    = (total + MC_FRAME_SIZE - 1) / MC_FRAME_SIZE;
    for (int f = 0; f < nf; f++) {
        rc = memcard_read_frame(port,
                 block * MC_FRAMES_PER_BLOCK + WORLD_FRAME0 + f, frame);
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
    player_rounds     = sd->rounds;
    graveolver_loaded = sd->loaded;
    player_weapons    = sd->weapons;
    player_keys       = sd->keys;
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

    /* --- Directory entry (block 0, frame `block`) --- */
    memset(frame, 0, sizeof(frame));
    frame[0] = DIR_STATE_USED;
    frame[4] = 0x00; frame[5] = 0x20; frame[6] = 0x00; frame[7] = 0x00;  /* size 0x2000 = 1 block */
    frame[8] = 0xFF; frame[9] = 0xFF;                                    /* first & only block */
    snprintf(filename, sizeof(filename), "%s%02d", SAVE_PREFIX, block);  /* e.g. BESLES-00000GRV03 */
    memcpy(frame + 10, filename, strlen(filename));                      /* NUL-terminated by memset */
    set_dir_checksum(frame);
    if (memcard_write_frame(port, block, frame) != MC_OK) { memcard_end(); return MC_BAD_DATA; }

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

    /* --- Per-room world blob, 128 bytes per frame from WORLD_FRAME0 --- */
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
                    block * MC_FRAMES_PER_BLOCK + WORLD_FRAME0 + f, frame) != MC_OK) {
                memcard_end(); return MC_BAD_DATA;
            }
        }
    }

    memcard_end();
    return MC_OK;
}
