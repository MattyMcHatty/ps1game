#ifndef MENU_H
#define MENU_H

#include "render.h"

void menu_init(void);
void menu_open(void);
void menu_update(void);
void menu_draw(RenderContext *ctx);

/* ---- Inventory slots ------------------------------------------------------
   The ITEMS column's grid cells, in draw order (row = slot/2, col = slot&1).
   Shared with the stove puzzle, whose picker lists the same items from the same
   icons, so anything added here shows up in both places. */
#define MENU_SLOT_FRONT_DOOR_KEY   0
#define MENU_SLOT_ROUNDS           1
#define MENU_SLOT_COPPER_POT       2
#define MENU_SLOT_WAX_CUBE         3
#define MENU_SLOT_GREEN_KEY_STONE  4
#define MENU_SLOT_FLAME_ROUNDS     5
#define MENU_SLOT_PIANO_KEY        6
#define MENU_SLOT_BLUE_KEY_STONE   7
#define MENU_SLOT_YELLOW_KEY_STONE 8
#define MENU_ITEM_SLOTS            9   /* the ITEMS grid holds 3x4 = 12 cells, so
                                          there are 3 free; past 12 the layout
                                          itself has to grow again (it went from
                                          2x4 to 3x4, with smaller icons, when
                                          the Yellow Key Stone made a 9th item) */

int         menu_item_held(int slot);   /* 1 if the player currently holds it */
const char *menu_item_name(int slot);
/* Draw a slot's icon at an arbitrary screen rect (no-op if not held). The
   caller must have reset the texture window — see the note in menu_draw. */
void menu_draw_item_icon(RenderContext *ctx, int slot, int x, int y, int size,
                         int ot_idx);

#endif
