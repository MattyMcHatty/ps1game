#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxcd.h>
#include "cdaudio.h"        /* cdaudio_suspend/resume around the entry read */
#include "anzu_tex.h"

static AnzuTex tiles[ANZU_TILE_COUNT];

#define ANZU_TIM "\\TEX\\ANZUTAB.TIM;1"

/* The grid, in texels from the texture's top-left. 128 does not divide by
   three, so the middle column is one texel narrower than the outer two. */
static const uint8_t COL_U0[3] = { 0, 43, 85 };
static const uint8_t COL_W [3] = { 43, 42, 43 };
#define ROW_H 64

/* Read the tablet, LoadImage it into the slot baked into its own header, and
   cut the six tiles out of it. Same derivation as item_pickup.c's load_sprite:
   U0 is the texture's x WITHIN its 64-word tpage, scaled by texels per word.

   The caller supplies the scratch buffer, the shape kitchen_stream_textures()
   and the Greenhouse's stream use, and the reason this can run on a transition
   without adding a permanent byte to the heap. A file too big for the buffer is
   SKIPPED, the same tolerance a missing file gets: the puzzle draws wrong
   rather than the console crashing. The tablet is 8256 bytes, five sectors,
   against ANZU_SCRATCH below. */
static void load_tablet(uint8_t *buf, int bufcap) {
    CdlFILE file;
    if (!CdSearchFile(&file, ANZU_TIM)) return;
    int sectors = (file.size + 2047) / 2048;
    if (sectors * 2048 > bufcap) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    uint16_t clut = 0;
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        clut = getClut(tim.crect->x, tim.crect->y);
    }
    uint16_t tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;  /* px per word */
    int u_base   = (tim.prect->x & 63) * px_mult;
    int v_base   = tim.prect->y % 256;

    int i;
    for (i = 0; i < ANZU_TILE_COUNT; i++) {
        int col = i % 3, row = i / 3;
        tiles[i].tpage = tpage;
        tiles[i].clut  = clut;
        tiles[i].u0    = (uint8_t)(u_base + COL_U0[col]);
        tiles[i].v0    = (uint8_t)(v_base + row * ROW_H);
        tiles[i].du    = (uint8_t)(COL_W[col] - 1);
        tiles[i].dv    = (uint8_t)(ROW_H - 1);
    }
}

/* 10 KB against a 10240-byte (five-sector) read. Freed again, so neither entry
   point leaves anything behind. */
#define ANZU_SCRATCH (10 * 1024)


static void stream_tiles(void) {
    uint8_t *buf = malloc(ANZU_SCRATCH);
    if (!buf) return;           /* leave the slot as it is, do not crash */
    load_tablet(buf, ANZU_SCRATCH);
    free(buf);
}

/* Startup. No CD-DA bracket: nothing is playing this early, and this runs
   alongside the other LoadImage-at-boot loaders. */
void anzu_tex_load(void) {
    stream_tiles();
}

/* ROOM ENTRY. Re-read the tablet on the way into the piano room.

   >>> THE TILES USED TO BE UPLOADED ONCE AND NEVER PUT BACK, AND THAT WAS A
   BUG, NOT ONLY A COST. <<< Other rooms write over the slot (the six old 8bpp
   tiles sat under the Catacombs' sconce), and the player can quit to the title
   and load a Chapter 1 save without restarting the executable (see the note in
   src/area_bank.h), walk into the piano room, and find the puzzle showing
   whatever art landed there last. Re-reading on entry closes that, and it is
   what lets the slot (x832 y0) be shared with other rooms' art; see the note in
   anzu_tex.h. When something takes it, add
   the pair to KNOWN_STREAM_PAIRS in tools/vram_map.py.

   THE BRACKET IS MANDATORY. A data read issued while CD-DA streams hangs the
   drive; cdaudio_suspend/resume are no-ops when nothing is playing. Call only
   from main's STATE_LOADING, where the GPU is already idle — load_tablet
   DrawSyncs after every LoadImage in any case. */
void anzu_tex_stream(void) {
    cdaudio_suspend();
    stream_tiles();
    cdaudio_resume();
}

const AnzuTex *anzu_tex(int tile) {
    if (tile < 0 || tile >= ANZU_TILE_COUNT) tile = 0;
    return &tiles[tile];
}
