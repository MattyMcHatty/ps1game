#ifndef CRIB_H
#define CRIB_H

#include <stdint.h>
#include "render.h"
#include "title.h"

/* Crib: an iron cot, Chapter 3's fifth prop and the first thing in the
   Catacombs that is furniture rather than machinery. One stands in the alcove in
   the SOUTH-WEST corner of the Room of Arms (src/room_of_arms.c places it).

   ONE texture of its own ("crib", \TEXCTCMB\CRIB.TIM) and one mesh
   ("Crib.smx" -> assets/props/crib.smd, 60 primitives, 3644 bytes — remodelled
   down from 100 and 5692 before it had ever been placed).

   >>> STATIC, AND ONLY FOR NOW. <<< It has no update(), it registers no light,
   it takes no button press and nothing in the game reads its state, because
   there is none. THE MOVEMENT IS COMING — a rock, or a rattle — and this header
   is where its numbers will go. Two things about the shape below are already
   built for that and should not be simplified away on the grounds that a static
   prop does not need them:

     - THE INSTANCE CARRIES ITS OWN rot_y AND ITS OWN BAKED AABB, rather than
       the module holding one world box. A rocking crib moves its rot_y (or its
       x/z) per frame, and the box has to be re-derivable from whatever the
       instance currently is. crib_place() is already the one function that does
       that derivation.
     - THE DRAW REBUILDS ITS MODEL MATRIX EVERY FRAME from the instance's
       fields. Nothing is cached across frames, so animating a field is a write
       and not a cache invalidation.

   >>> ITS COLLISION IS ITS OWN MESH. <<< There is no Crib_mesh.smx and no
   smx_to_collision.py pass for this prop: crib_load_assets() walks the loaded
   SMD's vertices for the real min/max on all three axes, and crib_place() bakes
   those into a world AABB for the instance. Re-export the model bigger and the
   box follows it on the next build, with nothing to keep in step by hand. Same
   arrangement as src/sconce.h, src/oil_dispenser.h and src/save_point.c, and it
   means the same thing: the engine collides BOXES, so the mesh's job is to SIZE
   the box.

   IT IS MEASURED min/max AND NOT AS HALF-EXTENTS, which the oil dispenser's
   header argues at length. THIS MODEL WOULD SURVIVE HALF-EXTENTS — as authored
   it spans x[-175,175] z[-100,100] y[-195,0], i.e. it is centred on its origin
   in plan and stands up off the floor the origin sits on, so |vx|max and |vz|max
   would describe it exactly. The measured form is here anyway because it costs
   the same six comparisons and it cannot be wrong after a re-export that shifts
   the origin — which is precisely the change an animator makes when they decide
   the thing should rock about one end.

   SO x/z IS THE CRIB'S CENTRE IN PLAN and y is the floor under it (world y = y +
   GROUND_FLOOR_Y, so a room whose floor is y=0 passes -GROUND_FLOOR_Y).
   rot_y 0 lays the LONG axis (350) along X and the short one (200) along Z.

   Area-tagged, like the sconces, the levers and the oil dispenser, so
   cribs_collide() and cribs_draw() can be called unconditionally from the shared
   routines and are a no-op in every other room. Without the tag an instance left
   standing would block the player invisibly anywhere its coordinates land — and
   the Room of Arms' footprint is centred on the origin, so that is a live risk
   here rather than a formality (the note in room_of_arms_init() spells it out).

   CHAPTER 3 ONLY, and it is NOT in src/area_bank.c's free list for that reason:
   the purge at the catacomb mouth gives back the props Chapters 1 and 2 use, and
   this is one of the few that has to survive it. Its texture is an ordinary
   deferred TEXBANK_CATACOMBS registration, so it holds no pixels at all until
   the chapter door; only its ~3.6 KB of geometry is permanent.

   >>> IT IS THE 70th texmgr REGISTRATION OF 72. <<< TEXMGR_MAX in src/texmgr.c
   is the cap and texmgr_register past it returns -1 SILENTLY, which breaks that
   texture in every room that draws it. TWO LEFT. The way to spend none is the
   one the Room of Arms' cobblestone and door slots take: if another module
   already uploads the page you need, call ITS narrow uploader and use TIM_SLOT()
   for the header rather than registering a second RAM copy of the same file.
   Count with py tools/heap_budget.py before adding the seventy-first.

   THE VRAM PAGE IS x576 y0 (8bpp, Voff 0) — an exact 128x128 fit, and the first
   use of a page tools/VRAM_MAP_CATACOMBS.txt has been marking "AVAILABLE, needs a
   way back first" ever since the way back was written. It is not this prop's to
   owe: anzu_tex_stream() re-reads all six Anzu tiles off the CD on piano-room
   entry (src/anzu_tex.c says so at length, and names x576/x704 y0 as the two whole
   pages it unlocks), and kitchen_stream_owned_textures() re-reads red_wlppr on
   kitchen entry. BOTH CALLS ARE LOAD-BEARING FOR THIS PROP NOW — delete either
   and two frames of the Anzu puzzle, or a wall of the kitchen, become a cot until
   the console is reset.

   >>> IT IS x576 AND NOT x704, AND IT WENT TO x704 FIRST. <<< The catacombs map
   calls the two pages equally available because it prints only the occupants it
   knows need a way back; x704's LEFT half is six 4bpp garden textures it does not
   print at all, and an 8bpp 128 texture takes all 64 columns of a page. py
   tools/vram_map.py is what caught that, and it is the thing to run before
   believing either map about a page.

   The CLUT is borrowed on the arms field's argument, from anzu2.tim at (256,484):
   a palette belonging to a texture whose PIXELS this one is already displacing,
   so the two go back together in the one stream.

   Voff 0 is what lets the Room of Arms' own 128x128 texture window serve this
   prop rather than mask it, which is the whole requirement — a Voff >= 128 page
   would have the cot sampling 128 texels too high in every windowed room.

   >>> THE PROP NO LONGER DEPENDS ON THAT WINDOW, AND IT DID BEFORE THE REMODEL.
   <<< The first export reached u=128, one past the tile, and only landed back on
   the art because the window wrapped it mod-128; the 60-poly model keeps every UV
   inside u[2,127] v[2,124]. So it is now correct with or without one, which is a
   property of THIS export and not a rule — re-UV the model past 127 on any axis
   and the window is load-bearing again. */

#define MAX_CRIBS 4

void crib_load_assets(void);      /* startup: geometry + DEFERRED registration  */
void crib_upload_texture(void);   /* room entry: pure LoadImage, no CD           */
void cribs_clear(void);           /* drop every placed instance                 */

/* Place one. x/z is the model's CENTRE in plan and y the floor reference (world
   y = y + GROUND_FLOOR_Y). rot_y is 0..4096 = a full turn. */
void crib_place(GameState area, int32_t x, int32_t y, int32_t z, int32_t rot_y);

void cribs_draw(RenderContext *ctx);
void cribs_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

#endif
