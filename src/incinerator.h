#ifndef INCINERATOR_H
#define INCINERATOR_H

#include <stdint.h>
#include "render.h"
#include "title.h"

/* The Incinerator: the machine down the south end of the Incinerator Room's
   hall, and Chapter 3's third prop. A 2000-long body with a furnace door, a
   button panel on its north face and a conveyor tray sticking out of its west
   end.

   ONE texture of its own ("incinerator", \TEXCTCMB\INCINPRP.TIM) and one mesh
   ("Incinerator.smx" -> assets/props/incinerator.smd). STATIC: it never moves
   and never opens. What it does is HOLD ONE THING and, when the button is
   pressed, destroy it — see THE HOPPER below.

   >>> ITS COLLISION IS ITS OWN MESH. <<< There is no <name>_mesh.smx and no
   smx_to_collision.py pass: incinerator_load_assets() walks the loaded SMD's
   vertices for the real min/max on all three axes and incinerator_place()
   bakes those into a world AABB. Re-export the model bigger and the box
   follows it on the next build. Same arrangement as src/oil_dispenser.h and
   src/sconce.h, and it means the same thing: the engine collides BOXES, so the
   mesh's job is to SIZE the box.

   >>> AND THE MESH IS AUTHORED IN ROOM WORLD COORDINATES. <<< This is the one
   place it differs from every other prop in the game, including the oil
   dispenser it otherwise copies. That prop's model sits around its own origin
   and the room supplies the corner to stand it on; THIS model's vertices are
   already the coordinates the machine occupies in the Incinerator Room —
   x[-2000,-1], y[-300,0], z[-3150,-2550], which is why it clears the north
   door at x[200,400] without anyone having positioned it. So the room places
   it at the ORIGIN (0, floor, 0) and the position comes out of the file rather
   than out of a pair of #defines somebody has to keep in step with Blender.

   Re-export it moved and it moves in the game, with nothing to edit, and that
   has already been collected on once: the re-export that took the machine from
   400 tall to 300 needed no change to the placement or the collision box at
   all. What it DID need was the two signs and the conveyor camera brought down
   100 to match, because those are authored numbers rather than measured ones -
   see src/incinerator_room.c and src/incinerator_panel.c. That is the standing
   cost of this arrangement: the geometry follows the file, the things HUNG on
   the geometry do not.

   The other cost is that rot_y is meaningless for this prop — rotating it would spin the
   whole hall's worth of coordinates about x=0,z=0 — so the room passes 0 and
   the placement code keeps the rotation arithmetic only because the AABB bake
   is shared with the props it was taken from.

   THE BOX IS THE WHOLE BODY, conveyor included: x[-2000,-1] across the full
   z[-3150,-2550]. The tray only sticks out over z[-2950,-2750], so the box is
   solid across 200 units of air either side of it. That is the usual price of
   an AABB and it costs nothing here — the player interacts with the conveyor
   from the WEST, standing outside the box, and the 200 units of phantom
   solidity are against a face they never walk along.

   Area-tagged, like the sconces and the oil dispenser, so incinerator_collide()
   and incinerator_draw() can be called unconditionally from the shared routines
   and are a no-op in every other room.

   CHAPTER 3 ONLY, and so NOT in src/area_bank.c's free list: the purge at the
   catacomb mouth gives back the props Chapters 1 and 2 use, and this is one of
   the few that has to survive it. Its texture is an ordinary deferred
   TEXBANK_CATACOMBS registration, so it holds no pixels until the chapter door;
   only its ~6 KB of geometry is permanent.

   ITS VRAM PAGE IS TIME-SHARED with Rabisu tex.tim and vines.tim at (704,256),
   on the argument the sconce and the oil dispenser both make: the catacomb
   mouth is one-way, so the boss's hide and the greenhouse's vines are never
   drawn again in a session that got here, and both come back by their own
   deferred bank upload if a title load puts the player back in Chapter 1 or 2.
   Voff 0, so the Incinerator Room's one 128 texture window serves it — which
   this model NEEDS, because its UVs run past a tile (u to 193, v to 190) where
   the plating and the grating tile across its big faces. The two FURNACE
   OPENINGS depend on that wrap for their art as well as for their glow; the
   long note above the glow mask in src/incinerator.c says how they are found. */

/* ---- THE HOPPER ------------------------------------------------------------
   ONE slot. It holds a MENU_SLOT_* and a count, and it is a SINGLETON rather
   than a field of an instance array — src/oil_dispenser.h's reservoir note
   sets out at length what goes wrong when a stateful prop is placed again on
   every room entry, and the answer there was forced by exactly this. There is
   one incinerator in the game and one slot describes it. A second would need
   this to become an array that clear() does not touch, indexed by placement
   order, with a save field each.

   >>> AMMO IS CAPPED AT SIX ROUNDS. <<< Rounds and Flame Rounds are counts in
   player_ammo[], not bits in player_items, so "the item" the player puts in is
   a QUANTITY and the machine has to decide how much of it it takes. Six, and
   the rest stays in the player's pocket — so feeding the incinerator can never
   cost a player their whole reserve in one press.

   Hatch Keys are the third counted slot (player_hatch_keys) and take exactly
   one, because a hatch key is a thing rather than a quantity. Everything else
   is a bit and moves whole.

   IT IS SAVED, as SaveData.incin_slot / incin_count (SAVE_VERSION 26). An item
   left in the machine has to still be there after a load, or the player loses
   it — the incinerator is the only place in the game that takes something out
   of the inventory and can give it back. */
#define INC_AMMO_MAX  6

int  incinerator_slot(void);         /* MENU_SLOT_* held, or -1 if empty  */
int  incinerator_count(void);        /* rounds held; 1 for a plain item   */
void incinerator_set_stored(int slot, int count);  /* savegame restore    */
void incinerator_reset(void);        /* new game: empty                   */

/* Move the player's `slot` into the machine. Returns the count actually taken,
   or 0 if it refused (the hopper is full, or the player does not hold it).
   THE ONLY code that takes the item off the player. */
int  incinerator_store(int slot);

/* Give it back. Returns the MENU_SLOT_* handed over, or -1 if it was empty.
   THE ONLY code that puts it back. */
int  incinerator_retrieve(void);

/* ---- THE BUTTON ------------------------------------------------------------
   TWO plays of SFX_MCHNE back to back, then SFX_GAS and a line in the log on
   the same frame. The grind is 2.80 s (168 frames at 60 fps —
   src/chainlink_door.c measured it for the same reason), so the cycle is 336
   frames, 5.6 s, and the vent lands as the second play ends.

   IT WAS THREE GRINDS AND 8.4 SECONDS. The third added length without adding
   information; the gas is what the sequence was missing, because it gives the
   cycle an ending rather than just a stop.

   >>> THE LOG LINE LANDS AT THE END OF THE CYCLE, NOT ON THE PRESS. <<< The
   brief says so ("then the log should say...", and the gas prints "as" it
   plays), and a button that reported its result early on one branch and on time
   on the other would be two mechanisms wearing one cap. So both branches wait,
   and the press itself is acknowledged by the machine starting up.

   >>> AND THE CONVEYOR IS CLOSED FOR THE WHOLE 336 FRAMES. <<< Both its prompt
   and its press go away while the machine runs and come back when it stops:
   src/incinerator_room.c hides the sign, src/incinerator_panel.c refuses the
   trigger, and both ask incinerator_cycle_active(). They have to move together
   — a prompt over a dead button reads as a dropped input.

   THE ITEM IS CONSUMED ON THE PRESS, not at the end. The press is the decision
   and the cycle is the machine carrying it out; a player who reloaded a save
   mid-cycle would otherwise get the item back for free, and the cycle does not
   survive a room change anyway. */
typedef enum {
    INC_PRESS_IGNORED,   /* a cycle is already running: the press does nothing */
    INC_PRESS_EMPTY,     /* nothing in the hopper -> "It has no effect"        */
    INC_PRESS_BURNED     /* something was in it, and is not now                */
} IncPress;

IncPress incinerator_button_press(void);   /* starts the cycle; consumes       */
void     incinerator_cycle_update(void);   /* per frame: fires the three plays */
int      incinerator_cycle_active(void);   /* 1 while the machine is running   */

/* 1 on the frame the cycle ENDS, and only that frame — the room reads it to put
   its line in the log. Latched rather than returned by the update so the room
   owns the wording, which is the split src/catacombs_entry.c keeps with the oil
   dispenser. */
int      incinerator_cycle_finished(void);

void incinerator_load_assets(void);     /* startup: geometry + DEFERRED reg   */
void incinerator_upload_texture(void);  /* room entry: pure LoadImage, no CD  */
void incinerator_clear(void);           /* drop the placed instance           */

/* Place it. x/z/y is the model's ORIGIN, and for this prop that is the room's
   own origin — see the note above. y is the floor reference (world y = y +
   GROUND_FLOOR_Y). rot_y is 0..4096 and should be 0; see above for why. */
void incinerator_place(GameState area, int32_t x, int32_t y, int32_t z,
                       int32_t rot_y);

void incinerator_draw(RenderContext *ctx);
void incinerator_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

#endif
