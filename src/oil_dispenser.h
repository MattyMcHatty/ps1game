#ifndef OIL_DISPENSER_H
#define OIL_DISPENSER_H

#include <stdint.h>
#include "render.h"
#include "title.h"

/* Oil Dispenser: a wall tank with a tap under it, Chapter 3's second prop and
   the first thing down here the player can walk up to and press a button at
   that is not a door.

   ONE texture of its own ("oil_container", \TEXCTCMB\OILCNTNR.TIM) and one mesh
   ("Oil Dispenser.smx" -> assets/props/oil_dispenser.smd). STATIC: it never
   moves, never opens, never breaks. What it does is HOLD OIL and pour it into
   the Helluminator — see THE RESERVOIR below.

   >>> ITS COLLISION IS ITS OWN MESH. <<< There is no <name>_mesh.smx and no
   smx_to_collision.py pass for this prop: oil_dispenser_load_assets() walks the
   loaded SMD's vertices for the real min/max on all three axes, and
   oil_dispenser_place() bakes those into a world AABB for the instance. Re-
   export the model bigger and the box follows it on the next build, with
   nothing to keep in step by hand. Same arrangement as src/sconce.h and
   src/save_point.c, and it means the same thing: the engine collides BOXES, so
   the mesh's job is to SIZE the box.

   IT IS MEASURED min/max, NOT AS HALF-EXTENTS, and that is the one place this
   module differs from the sconce's. The sconce is centred on its origin in plan
   and stands up from y=0, so |vx|max and |vz|max describe it. THIS MODEL IS
   OFFSET FROM ITS ORIGIN ON EVERY AXIS — as authored it spans x[-40,0],
   z[0,40] and y[-280,-170], i.e. its origin sits at the +X/-Z corner of its
   footprint and the tank hangs 170 to 280 units ABOVE it (-Y is up). That is
   not an accident of export: the origin is the point on the FLOOR in the corner
   the prop is set into, so `x`/`z` below are the corner and `y` is the floor,
   and a wall-mounted tank needs no separate mounting height to be passed in.
   Take absolute half-extents off a model like this and the box lands offset by
   its own width.

   THE SPOUT points out along (-X,+Z) at the bottom of the tank, which is what
   makes rot_y 0 the SOUTH-EAST corner orientation: set into a corner whose
   walls are +X and -Z, the tap faces back into the room.

   Area-tagged, like the sconces and the levers, so oil_dispensers_collide() and
   oil_dispensers_draw() can be called unconditionally from the shared routines
   and are a no-op in every other room. Without the tag an instance left
   standing would block the player invisibly anywhere its coordinates land.

   CHAPTER 3 ONLY, and it is NOT in src/area_bank.c's free list for that reason:
   the purge at the catacomb mouth gives back the props Chapters 1 and 2 use,
   and this is one of the few that has to survive it. Its texture is an ordinary
   deferred TEXBANK_CATACOMBS registration, so it holds no pixels at all until
   the chapter door; only its ~2 KB of geometry is permanent. */

/* ---- THE RESERVOIR ---------------------------------------------------------
   100 units, the same hundred the lantern arrives holding (HELL_OIL_MAX in
   src/player.h), and the two are deliberately the same number: one full tank is
   exactly one full lantern, so "how many refills is this worth" is a question
   the player answers by looking rather than by counting. It is NOT an item and
   not a pickup — player.h's oil block already says why the fuel is a plain
   scalar rather than an AmmoType, and the same reasoning applies a second time
   here.

   >>> IT IS ONE RESERVOIR FOR THE WHOLE GAME, NOT ONE PER INSTANCE. <<< A
   module-level scalar, not a field of OilDispenser, and that is forced rather
   than chosen: catacombs_entry_init() calls oil_dispensers_clear() and places
   the prop again on EVERY entry to the room, so per-instance state would top
   itself back up every time the player walked out and back. There is exactly
   one refill point in the game today and one scalar describes it exactly.
   WHEN A SECOND ONE SHIPS this has to become an array that clear() does not
   touch, indexed by placement order the way world.c indexes its entities, with
   a save field per tank — and the two would otherwise silently share a tank,
   which is the kind of bug that reads as a memory fault.

   IT IS SAVED, as SaveData.disp_oil (SAVE_VERSION 24). A dispenser that came
   back full after a load would make the whole resource free.

   THE BAR over it is the only readout: blue at full, red at empty, and it fades
   up on exactly the curve the examine prompt does, so the tank tells you what
   is in it at the same moment it tells you it can be used. */
#define OD_OIL_MAX  100

#define MAX_OIL_DISPENSERS 4

/* What a press on the dispenser did. The room turns these into log lines — it
   owns the room's wording, this module owns the arithmetic. */
typedef enum {
    OD_REFILL_NO_LANTERN,  /* the player does not own the Helluminator      */
    OD_REFILL_EMPTY,       /* the tank is dry; nothing moved                */
    OD_REFILL_FULL,        /* the lantern was already at HELL_OIL_MAX       */
    OD_REFILL_DONE         /* oil moved: the lantern is full or the tank dry */
} OdRefill;

/* Pour. Moves min(what the lantern is short, what the tank holds) from one to
   the other and says which of the four cases happened. The ONLY thing that
   spends the reservoir. */
OdRefill oil_dispenser_refill(void);

int  oil_dispenser_oil(void);            /* 0..OD_OIL_MAX, for the bar + save  */
void oil_dispenser_set_oil(int units);   /* savegame restore; clamps           */
void oil_dispensers_reset(void);         /* new game: the tank back to full    */

/* The bar over the tank. `radius`/`fade_near` are the caller's own prompt-fade
   numbers — passed in rather than duplicated here so the bar and the prompt it
   belongs beside can never drift onto different curves. Call it from the room's
   draw, after the room mesh. */
void oil_dispensers_draw_bar(RenderContext *ctx, int32_t radius, int32_t fade_near);

void oil_dispenser_load_assets(void);    /* startup: geometry + DEFERRED reg    */
void oil_dispenser_upload_texture(void); /* room entry: pure LoadImage, no CD   */
void oil_dispensers_clear(void);         /* drop every placed instance          */

/* Place one. x/z is the model's ORIGIN in plan — the floor corner the tank is
   set into, not its centre — and y the floor reference (world y = y +
   GROUND_FLOOR_Y). rot_y is 0..4096 = a full turn. */
void oil_dispenser_place(GameState area, int32_t x, int32_t y, int32_t z,
                         int32_t rot_y);

void oil_dispensers_draw(RenderContext *ctx);
void oil_dispensers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

#endif
