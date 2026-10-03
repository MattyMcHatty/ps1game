#ifndef H_CORRIDOR_H
#define H_CORRIDOR_H

#include <stdint.h>
#include "render.h"

/* The H Corridor: Chapter 3's SEVENTEENTH room, through the Zig Zag Tomb's
   south door (z=0, x[200,400], the Tomb's south-west corner).

   THE SHAPE. Three 600-wide corridors in one flat storey at y=0, walled to
   y=-800 and open above that into the dark, as the Zig Zag Tomb is:

       west leg   x[0,600]      z[0,5400]   the full length, north to south
       crossbar   x[600,1800]   z[1200,1800]
       east leg   x[1800,2400]  z[0,1800]   the short one, south of the bar

   so in plan it is an "h": the crossbar joins the two legs 1200 up from the
   south wall, and only the west leg runs on north past it.

   THE MESHES ARE MODELLED IN THE TOMB'S WORLD: H Corridor z = Zig Zag Tomb
   z + 5400. Its north door at z=5400 x[200,400] is the Tomb's south door at
   z=0 x[200,400], the same x span.

   THE DOORS AND THE LADDER. Two doors are drawn and ONE is wired up:

     NORTH  z=5400  x[200,400]    y[-400,0]   -> Zig Zag Tomb, south door
     south  z=0     x[2000,2200]               not built (the east leg's foot)
     ladder z=0     x[200,400]    y[-1600,0]   not built (the west leg's foot)

   The north door is in the XY plane approached from -Z (wall 9 runs z=5400
   with nz=-4096), so TEXT_PLANE_XY with mirror=0. The unbuilt south door and
   the ladder are drawn and nothing else: no sign, no trigger. The ladder climbs
   a 600-square shaft over the west leg's south end to y=-1600, capped by one
   black untextured quad.

   THREE TEXTURES, ALL BORROWED — cobblestone and the catacomb inner door
   through src/catacombs_entry.c's narrow uploaders, the ladder through
   north_chamber_upload_ladder(), the Cleaver Corridor's set. The room owns none.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'
   ("H Corridor.smx" and "H Corridor mesh.smx"). */

void h_corridor_load_assets(void);     /* startup: three compile-time headers */
void h_corridor_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void h_corridor_upload_textures(void); /* room entry: pure LoadImage from RAM */
void h_corridor_init(void);            /* collision + floor zone + spawn      */
void h_corridor_draw(RenderContext *ctx);

/* Arrival through the north door, from the Zig Zag Tomb: just inside it,
   facing south. h_corridor_init()'s default, and today the only arrival. */
void h_corridor_spawn_north(void);

/* One frame of the north door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range and facing the door —
   the frame main.c starts the transition on. */
int  h_corridor_north_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void h_corridor_arm(void);

#endif
