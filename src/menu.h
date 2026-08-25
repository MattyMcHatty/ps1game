#ifndef MENU_H
#define MENU_H

#include <stdint.h>
#include "render.h"

void menu_init(void);
void menu_open(void);
void menu_update(void);
void menu_draw(RenderContext *ctx);

/* ---- Item IDs -------------------------------------------------------------
   MENU_SLOT_* is an ITEM IDENTITY, not a position on screen. It indexes the
   parallel description/icon/held tables in menu.c and is the handle every
   picker passes around (the stove, piano and exit-door puzzles all list the
   same items from the same icons, so anything added here shows up in all of
   them). WHERE an item sits in the ITEMS grid is a separate, runtime thing —
   see the inventory-order block below. */
#define MENU_SLOT_FRONT_DOOR_KEY   0
#define MENU_SLOT_ROUNDS           1
#define MENU_SLOT_COPPER_POT       2
#define MENU_SLOT_WAX_CUBE         3
#define MENU_SLOT_GREEN_KEY_STONE  4
#define MENU_SLOT_FLAME_ROUNDS     5
#define MENU_SLOT_PIANO_KEY        6
#define MENU_SLOT_BLUE_KEY_STONE   7
#define MENU_SLOT_YELLOW_KEY_STONE 8
#define MENU_SLOT_MAGENTA_KEY_STONE 9
#define MENU_SLOT_HATCH_KEY        10
#define MENU_ITEM_SLOTS           11   /* number of item IDs that exist */

/* ---- Inventory order ------------------------------------------------------
   The ITEMS column is 3x4 = 12 CELLS, and which item ID lives in which cell is
   decided at runtime, not by the numbering above: a newly collected item drops
   into the first free cell and the player can rearrange the grid with Circle
   (take) / Circle (place or swap). Past 12 items the layout itself has to grow
   again — it went from 2x4 to 3x4, with smaller icons, when the Yellow Key
   Stone made a 9th item.

   The arrangement is part of the save (SaveData.item_order), so it survives a
   save/load rather than snapping back to ID order. */
#define MENU_ITEM_CELLS           12

void menu_inventory_reset(void);   /* new game: empty every cell */
/* Reconcile the grid with what the player actually holds: drop cells whose item
   is gone, and drop each newly held item into the first free cell. Called on
   pickup (so cells fill in collection order) and again when the menu opens, to
   catch grants and consumptions from elsewhere — puzzles, crates, debug grants. */
void menu_inventory_sync(void);
/* Serialise/restore the arrangement: MENU_ITEM_CELLS bytes, each an item ID + 1
   with 0 for an empty cell. _load validates and then syncs, so a corrupt or
   stale blob degrades to the default first-free-cell order rather than lying
   about what is held. */
void menu_inventory_save(uint8_t *out);
void menu_inventory_load(const uint8_t *in);

int         menu_item_held(int slot);   /* 1 if the player currently holds it */
const char *menu_item_name(int slot);
/* Draw a slot's icon at an arbitrary screen rect (no-op if not held). The
   caller must have reset the texture window — see the note in menu_draw. */
void menu_draw_item_icon(RenderContext *ctx, int slot, int x, int y, int size,
                         int ot_idx);
/* Draw a WeaponType's icon at an arbitrary screen rect (the HUD's weapon box).
   No ownership check — the caller passes the weapon it wants drawn. The caller
   must have reset the texture window, as for menu_draw_item_icon. */
void menu_draw_weapon_icon(RenderContext *ctx, int weapon, int x, int y, int size,
                           int ot_idx);

#endif
