#ifndef SCONCE_H
#define SCONCE_H

#include <stdint.h>
#include "render.h"
#include "title.h"

/* Sconce: a floor-standing brass torch stand, Chapter 3's first prop.

   ONE texture of its own ("sconce", \TEXCTCMB\SCONCE.TIM) and one mesh
   ("Sconce.smx" -> assets/props/sconce.smd). The model is 120 x 120 in plan and
   180 tall, authored with its BASE at model y=0 like the dresser and the
   concrete props — so the draw adds GROUND_FLOOR_Y and the feet rest on the
   floor, and `y` here is the room's "standing on the floor" reference (0 in the
   Catacombs Entry's chamber, whose floor zone is y=0).

   >>> ITS COLLISION IS ITS OWN MESH. <<< There is no <name>_mesh.smx and no
   smx_to_collision.py pass for this prop: sconce_load_assets() walks the loaded
   SMD's vertices and takes the real XZ half-extents and the real height out of
   them, and sconce_place() bakes those into a world AABB for the instance. Re-
   export the model wider or taller and the box follows it on the next build,
   with nothing to keep in step by hand. It is the save point's arrangement
   (src/save_point.c), which is what "the mesh is the collision mesh" means for
   a prop: the engine collides boxes, so the mesh's job is to SIZE the box.

   THE FLAME is not in the mesh. It is a single camera-facing quad standing on
   the model's black coal bed, flipping between the two frames in the BOTTOM
   half of the same texture — see the note above SCONCE_FLAME_HALF_W in the .c
   for the cells, the alpha and why it needs no texture-window bracket.

   Area-tagged, like the levers and the grinders, so sconces_collide() and
   sconces_draw() can be called unconditionally from the shared routines and are
   a no-op in every other room. Without the tag an instance left standing would
   block the player invisibly anywhere its coordinates happen to land.

   CHAPTER 3 ONLY, and it is NOT in src/area_bank.c's free list for that reason:
   the purge at the catacomb mouth gives back the props Chapters 1 and 2 use,
   and this is one of the few that has to survive it. Its texture is an ordinary
   deferred TEXBANK_CATACOMBS registration, so it holds no pixels at all until
   the chapter door; only its ~4 KB of geometry is permanent. */

#define MAX_SCONCES 8

void sconce_load_assets(void);    /* startup: geometry + a DEFERRED registration */
void sconce_upload_texture(void); /* room entry: pure LoadImage, no CD access    */
void sconces_clear(void);         /* drop every placed instance (an area's init) */

/* Place one. x/z is the model's centre in plan, y the floor reference (world y
   = y + GROUND_FLOOR_Y puts the base on the floor). rot_y is 0..4096 = a full
   turn. */
void sconce_place(GameState area, int32_t x, int32_t y, int32_t z, int32_t rot_y);

/* One frame of the flame flip. Call it from the room's update beside the other
   props'; nothing else in the module has per-frame state. */
void sconces_update(void);

void sconces_draw(RenderContext *ctx);
void sconces_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

#endif
