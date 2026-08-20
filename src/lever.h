#ifndef LEVER_H
#define LEVER_H

#include <stdint.h>
#include "render.h"
#include "title.h"

/* Lever: a placeable prop, same shape of module as the chainlink door. One SMD
   ("Lever.smx"), no texture — the model is flat-shaded, and its face colours are
   load-bearing for placement:

     BLUE  (0,0,204)   a single cap at local z=+75  -> mounts AGAINST the wall
     BROWN (67,32,3)   the shaft, local z=-45..+75
     RED   (77,1,1)    the tip,   local z=-75..-45  -> sticks OUT into the room

   So local +Z points into the wall. The model is a 15x15 shaft 150 long, and
   unlike the base-origin props (concrete blocks, chainlink door) its origin is
   at its CENTROID — local x/y span only +/-7.5. Place it by the centre of the
   fixture it mounts in, then push it along Z so the blue cap meets the wall.

   Area-tagged, so levers_collide/point_solid can be called unconditionally from
   the shared collision routines. */
void levers_load_assets(void);   /* startup: geometry (no texture to load) */
void levers_clear(void);         /* drop every placed instance */

/* Place one. x/y/z is the model's centroid: x/y are the world position directly
   (y is the standing reference, so world y = y + GROUND_FLOOR_Y), z is the shaft
   centre. rot_y is 0..4096 = a full turn; 0 aims the blue cap at +Z (a wall on
   the room's north side), 2048 aims it at -Z (a south wall). */
void lever_place(GameState area, int32_t x, int32_t y, int32_t z, int32_t rot_y);

/* Throw the Nth placed lever (0-based, in lever_place order). `pitch` is a
   rotation about the model's LOCAL X axis in 4096ths of a turn, pivoting on the
   centre of the BLUE cap (local 0,0,+75) — the point where the fixture meets the
   wall — so the red tip swings while the mounting stays put. Positive pitch
   swings the tip DOWN towards the floor (-Y is up in this engine).

   The pitch is applied INSIDE the yaw, so it means the same thing for the north
   wall pair (rot_y 0) and the south wall pair (rot_y 2048).

   The collision AABB is NOT rebuilt: a thrown lever sweeps at most 75 units
   through space its own wall recess already occupies, and levers_collide is
   inert in the Attic Exit anyway (the 195 wall standoff holds the player
   further out than the shaft ever reaches). */
void lever_set_pitch(int index, int32_t pitch);

/* Player push-out and hitscan volume; both no-op outside the placing area. */
void levers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
int  levers_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack);

/* Is ANY instance of this family solid in the CURRENT AREA right now? Mirrors
   the non-coordinate gates of levers_point_solid above/below, and nothing
   else. collision_segment_blocked uses it to skip its whole segment-sampling
   pass in rooms that hold no props at all — see the note there. */
int  levers_any_solid(void);

/* Draw every instance in the current area. Restores the caller's view matrix. */
void levers_draw(RenderContext *ctx);

#endif
