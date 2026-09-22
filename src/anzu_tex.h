#ifndef ANZU_TEX_H
#define ANZU_TEX_H

#include <stdint.h>

/* ---- The six Anzu tiles ----------------------------------------------------
   anzu1..anzu6 are the 3x2 jigsaw of one relief (textures/anzu_tab.png): laid
   out 1,2,3 across the top and 4,5,6 across the bottom, all unrotated, they
   reassemble the whole carving. That is the Anzu Tablet puzzle's solution.

   They sit at x480/608/736, y0 and y64 — all Voff < 128 with U inside the 128
   window, so they are safe to draw under the piano room's texture window.

   THEY ARE STREAMED OFF THE CD ON ENTRY TO THE PIANO ROOM, not held in RAM.
   There is still no texmgr registration here and the reason is main RAM: six
   registrations would cost 36 KB permanently inside TEXBANK_MANSION, which is
   already the peak bank at 456 KB, and the heap is what runs out first on this
   machine (tools/HEAP_BUDGET.txt). The Greenhouse and the kitchen answer that
   the same way — read on the transition into a scratch buffer that is freed
   again — and so does this.

   They were uploaded once at startup and never put back until September 2026.
   See anzu_tex_stream() for what that cost and what it broke.

   anzu3 doubles as the Tablets prop's face texture (assets/props/Tablets.smx
   names it as that face's material), which is why this bank is a module of its
   own rather than a static inside anzu_puzzle.c — piano_props.c needs it too. */

#define ANZU_TILE_COUNT 6
#define ANZU_TILE_PX   64   /* every tile is 64x64 */

typedef struct {
    uint16_t tpage, clut;
    uint8_t  u0, v0;        /* top-left of the tile within its tpage */
} AnzuTex;

/* Load all six tiles into VRAM. Call ONCE at startup, alongside the other
   LoadImage-at-boot texture loaders (item_pickups_load_textures). */
void anzu_tex_load(void);

/* ROOM ENTRY: re-read all six off the CD, so the three half-page columns they
   hold (x480, x608, x736 at y0..128) are borrowable by other rooms. Touches the
   drive and brackets its own CD-DA suspend/resume, so callers must be on the
   STATE_LOADING path with the GPU idle. */
void anzu_tex_stream(void);

/* Tile 0..ANZU_TILE_COUNT-1 == anzu1..anzu6. Never NULL: an unloaded tile
   reads back as zeroes and simply draws wrong rather than crashing. */
const AnzuTex *anzu_tex(int tile);

#endif
