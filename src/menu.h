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
#define MENU_SLOT_VALVE_HANDLE     11
#define MENU_SLOT_BLOOD_PEARL      12
#define MENU_ITEM_SLOTS           13   /* number of item IDs that exist */

/* ---- Inventory order ------------------------------------------------------
   The ITEMS column is 3x4 = 12 CELLS, and which item ID lives in which cell is
   decided at runtime, not by the numbering above: a newly collected item drops
   into the first free cell and the player can rearrange the grid with Circle
   (take) / Circle (place or swap). It went from 2x4 to 3x4, with smaller icons,
   when the Yellow Key Stone made a 9th item.

   The arrangement is part of the save (SaveData.item_order), so it survives a
   save/load rather than snapping back to ID order.

   >>> CELLS ARE NOT IDS, AND THERE ARE NOW MORE IDS THAN CELLS. <<< 13 items
   exist against 12 cells, and that is deliberate: the grid only has to hold
   what the player can carry AT ONCE, not everything the game contains. Every
   Mansion and Garden item is consumed by its own puzzle before the catacomb
   mouth (the stones at the exit door and the plinths, the pot and cube at the
   stove, the keys in their locks, the valve in its pipe), so Chapter 3 starts
   with only the two ammo types in the grid and its own items — the Blood Pearl
   first — land in the space those left. Ammo and weapons are the only things
   that carry across.

   So the question for a new item is NOT "is MENU_ITEM_SLOTS past 12" but "can
   the player be holding more than 12 things at one moment". If they can, the
   grid has to grow; if not, just add the ID. menu_inventory_sync() places a
   newly held item in the first free cell and has NOTHING to do if there is
   none — the 13th thing held is silently left off the grid (only a debug grant
   can do that today). See tools/ADDING_AN_ITEM.txt.

   The puzzle PICKERS show these same 12 cells, in this same arrangement
   (menu_item_at_cell below), which is why they never grow with the ID count. */
#define MENU_ITEM_CELLS           12

void menu_inventory_reset(void);   /* new game: empty every cell */
/* Reconcile the grid with what the player actually holds: drop cells whose item
   is gone, and drop each newly held item into the first free cell. Called on
   pickup (so cells fill in collection order) and again when the menu opens, to
   catch grants and consumptions from elsewhere — puzzles, crates, debug grants. */
void menu_inventory_sync(void);
/* The pickers' view of the grid: the item ID in inventory cell `cell`, or -1
   for an empty cell (or one out of range). Call menu_inventory_sync() when the
   picker OPENS, so anything a puzzle granted since the last pickup or menu visit
   is in a cell. menu_cell_of_item is the reverse, -1 if the item has no cell. */
int  menu_item_at_cell(int cell);
int  menu_cell_of_item(int item);
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
/* ---- The reserve count over an icon ---------------------------------------
   menu_item_count is the ONE answer to "does this slot show a number, and what
   number" — the ammo reserves, and the Hatch Keys once there are two of them
   (one is not worth a glyph). 0 means draw nothing. The hatch puzzle's board
   shows the same icons this menu does and asks the same question of it, so the
   two can never disagree about what the player is carrying.

   menu_draw_count paints it in the menu's yellow-over-black-shadow digits, with
   (left_x, bottom_y) the number's left/bottom corner and the shadow one OT step
   behind ot_idx. menu_count_width is what the pixels will span, so a caller that
   wants the number in the icon's bottom-RIGHT can subtract it from the right
   edge — which is what the puzzle board does. */
int  menu_item_count(int slot);
int  menu_count_width(int value, int scale);
void menu_draw_count(RenderContext *ctx, int left_x, int bottom_y, int value,
                     int scale, int ot_idx);
/* Draw a WeaponType's icon at an arbitrary screen rect (the HUD's weapon box).
   No ownership check — the caller passes the weapon it wants drawn. The caller
   must have reset the texture window, as for menu_draw_item_icon. */
void menu_draw_weapon_icon(RenderContext *ctx, int weapon, int x, int y, int size,
                           int ot_idx);

#endif
