#ifndef THE_SHELF_H
#define THE_SHELF_H

#include <stdint.h>
#include "render.h"

/* The Shelf: Chapter 3's NINETEENTH room, at the top of the H Corridor's ladder
   and through the south door on the Up Down Maze's upper storey.

   THE SHAPE. One flat storey at y=0, walled to y=-800 and open above that into
   the dark, as the H Corridor is:

       the hall      x[-4800,-600]  z[-2200,400]
       east passage  x[-600,600]    z[-1200,-600]   out of the hall's east wall
                     x[0,600]       z[-1200,0]      ...and north to the shaft

   TWO BARRED PARTITIONS split the hall into three east-west lanes. Each runs
   from the east wall (x=-600) to x=-4200 and is 67 thick; west of x=-4200 the
   hall is open from wall to wall, so the three lanes meet there:

       north corridor  z[-266,400]     behind the north bars, z[-333,-266]
       the middle      z[-1466,-333]   the east passage opens off it
       south corridor  z[-2200,-1533]  behind the south bars, z[-1533,-1466]
       west end        x[-4800,-4200]  where all three meet

   The bars are full-height collision walls (walls 9-14), so they stop the
   player, the monsters and their sightlines; they are drawn see-through.

   THE MESHES ARE NOT IN THE NEIGHBOURS' WORLDS. Each room's coordinates are its
   own; the door and the ladder are paired by the transitions alone.

   THE WAYS OUT:

     NORTH   z=400  x[-2400,-2200] y[-400,0]  -> the Up Down Maze, through the
                                                south door on its UPPER storey.
                                                It opens into the NORTH
                                                CORRIDOR, behind the bars.
     LADDER  z=0    x[200,400]     y[0,1200]  -> the H Corridor, down the shaft
                                                at the end of the east passage
                                                to the foot of its ladder.

   The door is in the XY plane approached from -Z (wall 15 runs z=400 with
   nz=-4096), so TEXT_PLANE_XY with mirror=0. The ladder runs DOWN the shaft's
   south wall; the shaft (x[0,600] z[0,600], floored in black at y=+1200) is
   OUTSIDE the proxy — wall 0 runs z=0 the full height and holds the player 195
   back from its edge, the Cleaver Corridor's arrangement — so its prompt is an
   XY sign approached from -Z, mirror=0, and climbing it is the ladder
   transition (src/ladder_anim.h).

   THE MONSTERS (seeded in src/world.c, nav table in src/lumberer.c):
     two Lumberers patrolling the two barred corridors from the east wall to the
       west end, and following the corridor round the end of the bars when they
       wake to a player on the other side of them;
     two Crawlers side by side at the west end, between the two rows of bars.

   FIVE TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the inner door through
   src/catacombs_entry.c's narrow uploaders, the ladder through
   north_chamber_upload_ladder() and the bars through bars_upload_texture(). The
   one panel of incinerator art on the west wall is the room's OWN copy,
   SHLFINCN.TIM: the real incinerator shares its page AND its palette with the
   ladder, and this is the one room that draws both — see the_shelf.c.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'
   ("The Shelf.smx" and "The Shelf mesh.smx"). */

void the_shelf_load_assets(void);     /* startup: four headers + one deferred reg */
void the_shelf_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void the_shelf_upload_textures(void); /* room entry: pure LoadImage from RAM */
void the_shelf_init(void);            /* collision + floor zone + spawn      */
void the_shelf_draw(RenderContext *ctx);

/* Arrival through the north door, from the Up Down Maze: just inside it in the
   north corridor, facing south. the_shelf_init()'s default. */
void the_shelf_spawn_north(void);

/* Arrival up the ladder, from the H Corridor: on the passage floor just off the
   shaft's edge, facing south down the passage. */
void the_shelf_spawn_ladder(void);

/* One frame of the north door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range and facing the door —
   the frame main.c starts the transition on. */
int  the_shelf_north_door_triggered(int lock);

/* The same test for the top of the ladder, down to the H Corridor. */
int  the_shelf_ladder_triggered(int lock);

/* Arm every interaction in the room. Called by the spawns above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void the_shelf_arm(void);

#endif
