#ifndef SAVE_MENU_H
#define SAVE_MENU_H

#include "render.h"

/* The memory-card save flow, opened by pressing Circle next to a save point.
   Visually mirrors the pause menu (dimmed backdrop + bordered panel). It walks
   the player through: pick a card slot -> pick "new" or an existing save to
   overwrite -> confirm -> write -> result, then returns to the game. */

void save_menu_open(void);              /* enter the flow (seeds button edges) */
void save_menu_update(void);            /* per-frame input + the deferred write */
void save_menu_draw(RenderContext *ctx);

#endif
