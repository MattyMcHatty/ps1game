#ifndef ANZU_TEX_H
#define ANZU_TEX_H

#include <stdint.h>

/* ---- The six Anzu tiles ----------------------------------------------------
   anzu1..anzu6 are the 3x2 jigsaw of one relief (textures/anzu_tab.png): laid
   out 1,2,3 across the top and 4,5,6 across the bottom, all unrotated, they
   reassemble the whole carving. That is the Anzu Tablet puzzle's solution.

   Unlike the room/prop art these are NOT streamed. They sit in VRAM slots
   nothing else claims (x480/608/736, y0 and y64 — all Voff < 128 with U inside
   the 128 window, so they are safe to draw under the piano room's texture
   window), so they are uploaded ONCE at startup and the RAM buffer is freed.
   That is why there is no texmgr registration here: nothing ever needs to
   re-upload them, and a resident RAM copy of six 8bpp 64x64 tiles would cost
   ~27 KB for nothing.

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

/* Tile 0..ANZU_TILE_COUNT-1 == anzu1..anzu6. Never NULL: an unloaded tile
   reads back as zeroes and simply draws wrong rather than crashing. */
const AnzuTex *anzu_tex(int tile);

#endif
