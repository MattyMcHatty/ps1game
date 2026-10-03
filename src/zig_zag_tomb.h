#ifndef ZIG_ZAG_TOMB_H
#define ZIG_ZAG_TOMB_H

#include <stdint.h>
#include "render.h"

/* The Zig Zag Tomb: Chapter 3's SIXTEENTH room, through the Crucifix Corridor's
   south door (z=-1500, x[2600,2800], the foot of its cross arm).

   THE SHAPE. A 4200 square, flat and single-storey, walled to y=-800 and open
   above that into the dark, as the Cleaver Corridor and Cleaver L are. Nine
   600-square blocks stand in it on a 1200 grid (x and z each [600,1200],
   [1800,2400], [3000,3600]), their faces cut with loculi, leaving 600-wide
   lanes between them and round the walls.

   THE ZIG-ZAG. Three rows of barred partitions close the north-south gaps
   between the blocks, a row at a time, each leaving ONE end of its row open:

       z~3300   bars x[0,600] [1200,1800] [2400,3000]   open at the EAST end
       z~2100   bars x[1200,1800] [2400,3000] [3600,4200]  open at the WEST end
       z~900    bars x[0,600] [1200,1800] [2400,3000]   open at the EAST end

   so from the north door (north-west corner) the way south runs east, west,
   east, and the east-west lanes between the rows are the legs of the Z.

   THE DOORS. Three, all wired up:

     NORTH  z=4200   x[200,400]   y[-400,0]  -> Crucifix Corridor, south door
     EAST   x=4200   z[1400,1600] y[-400,0]  -> Cleaver L, west door
     SOUTH  z=0      x[200,400]   y[-400,0]  -> H Corridor, north door

   The north door is in the XY plane approached from -Z (wall 4 runs z=4200 with
   nz=-4095), so TEXT_PLANE_XY with mirror=0. The east door is in the YZ plane
   approached from -X (wall 32 runs x=4199 with nx=-4095), so TEXT_PLANE_YZ with
   mirror=1. The south door is in the XY plane approached from +Z (wall 3 runs
   z=0 with nz=+4095), so TEXT_PLANE_XY with mirror=1.

   >>> THE EAST DOOR IS LOCKED FROM THIS SIDE'S POINT OF VIEW: "Locked from the
   other side". <<< The Cleaver Corridor / Up Down Maze pair exactly, on
   FLAG_ZIG_ZAG_DOOR (player.h): the first Circle at Cleaver L's west door
   unlocks it, and until then this side reads red and does nothing.

   FOUR TEXTURES, ALL BORROWED — cobblestone, the catacomb inner door and the
   loculus through src/catacombs_entry.c's narrow uploaders, the bars through
   bars_upload_texture(). The room owns none.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'
   ("Zig Zag Tomb.smx" and "Zig Zag Tomb mesh.smx"). */

void zig_zag_tomb_load_assets(void);     /* startup: four compile-time headers  */
void zig_zag_tomb_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void zig_zag_tomb_upload_textures(void); /* room entry: pure LoadImage from RAM */
void zig_zag_tomb_init(void);            /* collision + floor zone + spawn      */
void zig_zag_tomb_draw(RenderContext *ctx);

/* Arrival through the north door, from the Crucifix Corridor: just inside it,
   facing south. zig_zag_tomb_init()'s default. */
void zig_zag_tomb_spawn_north(void);
/* Arrival through the east door, from Cleaver L: just inside it, facing west. */
void zig_zag_tomb_spawn_east(void);
/* Arrival through the south door, from the H Corridor: just inside it, facing
   north. */
void zig_zag_tomb_spawn_south(void);

/* One frame of each door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door — the frame
   main.c starts the transition on. The east one also returns 0 until
   FLAG_ZIG_ZAG_DOOR is set, but still advances its edge state. */
int  zig_zag_tomb_north_door_triggered(int lock);
int  zig_zag_tomb_east_door_triggered(int lock);
int  zig_zag_tomb_south_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawns above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void zig_zag_tomb_arm(void);

#endif
