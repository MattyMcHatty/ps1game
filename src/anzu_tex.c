#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxcd.h>
#include "cdaudio.h"        /* cdaudio_suspend/resume around the entry read */
#include "anzu_tex.h"

static AnzuTex tiles[ANZU_TILE_COUNT];

static const char * const tile_tim[ANZU_TILE_COUNT] = {
    "\\TEX\\ANZU1.TIM;1",
    "\\TEX\\ANZU2.TIM;1",
    "\\TEX\\ANZU3.TIM;1",
    "\\TEX\\ANZU4.TIM;1",
    "\\TEX\\ANZU5.TIM;1",
    "\\TEX\\ANZU6.TIM;1",
};

/* Read a TIM, LoadImage it into the slot baked into its own header, and record
   the handles needed to draw it. Same derivation as item_pickup.c's
   load_sprite: U0 is the texture's x WITHIN its 64-word tpage, so a tile in the
   right half of a page still maps correctly.

   The caller supplies the scratch buffer, so all six reads share ONE allocation
   instead of a malloc/free per tile — the shape kitchen_stream_textures() and
   the Greenhouse's stream use, and the reason this can run on a transition
   without adding a permanent byte to the heap. A tile too big for the buffer is
   SKIPPED, the same tolerance a missing file gets: the puzzle draws that frame
   wrong rather than the console crashing. Every tile is 6144 bytes on the disc
   (three sectors) against ANZU_TILE_SCRATCH below. */
static void load_tile(const char *filename, AnzuTex *t, uint8_t *buf, int bufcap) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return;
    int sectors = (file.size + 2047) / 2048;
    if (sectors * 2048 > bufcap) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        t->clut = getClut(tim.crect->x, tim.crect->y);
    }
    t->tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;  /* px per word */
    t->u0 = (uint8_t)((tim.prect->x & 63) * px_mult);
    t->v0 = (uint8_t)(tim.prect->y % 256);
}

/* 8 KB against a largest tile of 6144 bytes. One buffer for all six, freed
   again, so neither entry point leaves anything behind. */
#define ANZU_TILE_SCRATCH (8 * 1024)

static void stream_tiles(void) {
    uint8_t *buf = malloc(ANZU_TILE_SCRATCH);
    if (!buf) return;           /* leave the slots as they are, do not crash */
    int i;
    for (i = 0; i < ANZU_TILE_COUNT; i++)
        load_tile(tile_tim[i], &tiles[i], buf, ANZU_TILE_SCRATCH);
    free(buf);
}

/* Startup. No CD-DA bracket: nothing is playing this early, and this runs
   alongside the other LoadImage-at-boot loaders. */
void anzu_tex_load(void) {
    stream_tiles();
}

/* ROOM ENTRY. Re-read all six on the way into the piano room.

   >>> THE TILES USED TO BE UPLOADED ONCE AND NEVER PUT BACK, AND THAT WAS A
   BUG, NOT ONLY A COST. <<< The Catacombs' sconce (x448 y0, 64 columns) lands
   on anzu1 and anzu4 at x480. tools/vram_map.py allows that pair on the grounds
   that neither tile is drawn again "once the mouth closes" — true of the
   FICTION and not of the PROGRAM. The player can quit to the title and load a
   Chapter 1 save without restarting the executable (see the note in
   src/area_bank.h), walk into the piano room, and find two frames of the Anzu
   puzzle showing a lit sconce until the console is reset. This closes that.

   It also unlocks three half-page columns — x480, x608 and x736, y0..128 — for
   everyone else. Two of them sit beside pages the kitchen's own entry stream
   unlocked, which makes x576 y0 and x704 y0 whole 64-column pages that a
   128x128 8bpp texture could take; the garden rooms have been writing around
   the x704 half of that for five rooms now (see the notes in keystone_maze.c,
   maze_one.c, maze_two.c and garden_stairs.c). NOTHING BORROWS THEM YET — this
   is the restore path the borrowing needs. When something takes one, add the
   pair to KNOWN_STREAM_PAIRS in tools/vram_map.py.

   THE BRACKET IS MANDATORY. A data read issued while CD-DA streams hangs the
   drive; cdaudio_suspend/resume are no-ops when nothing is playing. Call only
   from main's STATE_LOADING, where the GPU is already idle — load_tile
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
