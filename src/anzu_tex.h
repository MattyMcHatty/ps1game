#ifndef ANZU_TEX_H
#define ANZU_TEX_H

#include <stdint.h>

/* ---- The six Anzu tiles ----------------------------------------------------
   ONE texture, anzu_tab.tim: the whole relief, 4bpp 128x128. The six tiles are
   a 3x2 grid cut out of it by UV - laid out 1,2,3 across the top and 4,5,6
   across the bottom, all unrotated, they reassemble the whole carving. That is
   the Anzu Tablet puzzle's solution.

   THE TILES ARE NOT SQUARE IN THE TEXTURE. The relief is a 3:2 picture squeezed
   into a 128x128 square, so a tile is a third of 128 wide (43, 42 and 43
   texels: 128 does not divide by three) and 64 tall, and drawing it on a square
   quad stretches it back out to the carving's real proportions. A quarter-turn
   just permutes which texture corner lands on which quad corner, so rotation
   is unaffected. Each tile therefore carries its own size.

   It sits at VRAM x832 y0: the LEFT half of its page (U 0..127) at Voff 0, so
   it is safe to draw under the piano room's 128x128 texture window. At 4bpp a
   right half would be U 128..255, which that window wraps onto other art.

   THAT SLOT IS OTHER ROOMS' ART (double_door, drain, plinth, stables wood and
   more, all texmgr-owned and none of it used in the piano room). That is safe
   because the tablet is re-read on every piano-room entry and every owner is
   re-uploaded on entry to its own room; tools/vram_map.py lists the pairs.

   IT IS STREAMED OFF THE CD ON ENTRY TO THE PIANO ROOM, not held in RAM, and
   there is no texmgr registration: the heap is what runs out first on this
   machine (tools/HEAP_BUDGET.txt), and the Greenhouse and the kitchen answer
   that the same way - read on the transition into a scratch buffer that is
   freed again. See anzu_tex_stream() for why it is re-read at all.

   Tile 3 doubles as the Tablets prop's face texture (assets/props/Tablets.smx
   names anzu3 as that face's material), which is why this is a module of its
   own rather than a static inside anzu_puzzle.c - piano_props.c needs it too. */

#define ANZU_TILE_COUNT 6

typedef struct {
    uint16_t tpage, clut;
    uint8_t  u0, v0;        /* top-left of the tile within its tpage */
    uint8_t  du, dv;        /* last texel column/row, relative to u0/v0 */
} AnzuTex;

/* Load the tablet into VRAM. Call ONCE at startup, alongside the other
   LoadImage-at-boot texture loaders (item_pickups_load_textures). */
void anzu_tex_load(void);

/* ROOM ENTRY: re-read the tablet off the CD, so its slot is borrowable by
   other rooms. Touches the drive and brackets its own CD-DA suspend/resume, so
   callers must be on the STATE_LOADING path with the GPU idle. */
void anzu_tex_stream(void);

/* Tile 0..ANZU_TILE_COUNT-1 == tiles 1..6. Never NULL: an unloaded tile reads
   back as zeroes and simply draws wrong rather than crashing. */
const AnzuTex *anzu_tex(int tile);

#endif
