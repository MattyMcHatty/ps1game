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

   THEY GIVE OFF LIGHT, and the light is not a shading pass: each one registers
   a render.h point light at its base, and every surface inside its radius is
   fogged and culled as though the camera stood that much nearer. So a lit
   sconce widens the same view distance the player's lantern widens, locally
   and around itself, and the room needs no second lighting model to receive
   it — see "THE GLOW" in the .c and "Point lights" in render.h.

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
   turn.

   `lit` 0 is a COLD sconce: the same stand and the same collision, but no flame
   sprite and no point light, so it sits in the room's fog like any other prop
   (the Cleaver Corridor's, with the Blood Pearl on its coal bed).

   >>> A COLD SCONCE WITH A BLOOD PEARL ON IT LIGHTS WHEN THE PEARL IS TAKEN. <<<
   Nothing to wire per room: "on it" means a PICKUP_BLOOD_PEARL in the room's
   live pickup array whose X/Z falls inside this sconce's footprint, and
   sconces_update() lights the sconce the first frame that pickup is inactive.
   A collected pickup keeps its slot (active 0, kind and position intact) both
   in world.c's room swap and across a save load, so on re-entry the sconce
   comes back lit. The ONE way to break it is a runtime spawn into that room,
   which reuses the first inactive slot and would overwrite the record. */
void sconce_place(GameState area, int32_t x, int32_t y, int32_t z, int32_t rot_y,
                  int lit);

/* One frame of the flame flip, and the pearl check above. Call it from the
   room's update beside the other props', AFTER item_pickups_update() so the
   sconce lights on the frame its pearl is collected. */
void sconces_update(void);

/* Hand this area's sconces to the renderer as point lights, one frame's worth,
   and advance their glow ramps. Call it from the room's draw AFTER
   g_fog_near/g_fog_far are set for the frame and BEFORE the room mesh is
   queued — see the note in the .c. */
void sconces_publish_lights(void);

void sconces_draw(RenderContext *ctx);
void sconces_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

#endif
