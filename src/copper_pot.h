#ifndef COPPER_POT_H
#define COPPER_POT_H

#include <stdint.h>
#include "render.h"

/* The Copper Pot: a permanent collectible the player carries between rooms
   (stored in the player_items bitmask, so it persists through transitions and
   saves — see player.h / savegame.c). One instance sits in the conservatory;
   collecting it sets ITEM_COPPER_POT and the world sprite stops drawing.

   Its texture time-shares the KEY's VRAM slot (the Front Door Key is spent long
   before the pot is reachable), so it is NOT uploaded at startup — that would
   clobber the still-needed key. copper_pot_load_assets() only captures the
   handle; copper_pot_upload_texture() does the actual LoadImage, called on
   conservatory entry (for the world sprite) and on any room load once the pot
   is owned (so the menu icon is correct everywhere). */

void copper_pot_load_assets(void);      /* startup: read TIM, capture handle (no upload) */
void copper_pot_upload_texture(void);   /* LoadImage into the key slot (safe: GPU idle) */
void copper_pot_reset(void);            /* new game: drop it from the inventory */
void copper_pot_update(void);           /* conservatory tick: proximity pickup */
void copper_pot_draw(RenderContext *ctx);  /* world billboard (view matrix must be set) */

int  copper_pot_owned(void);            /* 1 once collected */

/* Menu icon handle (same VRAM slot as the key, own CLUT). Valid after
   copper_pot_load_assets(); the menu draws it when the pot is owned. */
void copper_pot_icon(uint16_t *tpage, uint16_t *clut,
                     uint8_t *u0, uint8_t *v0, uint8_t *u1, uint8_t *v1);

#endif
