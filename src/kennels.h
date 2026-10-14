#ifndef KENNELS_H
#define KENNELS_H

#include <stdint.h>
#include "render.h"

/* The Kennels: Chapter 3's THIRTY-THIRD room, through the Nursery's west door -
   the one the Gula Tablet stands across, and which only opens once all six
   cots are lit (src/nursery.c, THE WEST DOOR).

   THE SHAPE. A long hall running north from its one door, x[-2389,2387]
   z[0,4299], flat at y=0, its stone rising to y=-800:

     - the south wall z=0, x[-1200,1200], chamfered out to x=+-1800 at z=600;
     - the side walls x=+-1800 up to z=3000, where each opens into a diagonal
       bay reaching x=+-2389;
     - at the north end, TWO KENNELS: barred cages x[-1200,-50] and
       x[4,1200], z[3700,4299], behind a bars front at z[3600,3700] (the
       collision proxy stops it at y=-400, the drawn bars at y=-600);
     - FOUR square posts, 200 across, at (+-700, 900) and (+-700, 2500):
       600 tall in the collision proxy, but DRAWN floor to ceiling (y 0 to
       -800 in Kennels.smx) - read the drawn one for anything seen;
     - a 4x4 inlay of gula-tablet tiles on the floor, x[-400,400]
       z[1400,2200], and fourteen GULA CODES panels high on the back walls.

   THE DOOR. One, and it is wired:

     SOUTH  z=0  x[-200,0]  y[-400,0]  -> the Nursery, its west door

   In the XY plane at fixed Z, approached from +Z (wall 3 runs z=0 with
   nz=+4096), so TEXT_PLANE_XY with mirror=1 and the sign 11 proud of the wall
   along +Z - the Throat's south door's terms.

   world.c seeds nothing in it; the lever puzzle places its own eight dogs.

   GULA (src/gula.h). When the eighth dog dies she fades in on the inlay, and
   the door is sealed until her death scene ends. Her sheet streams over the
   CRAWLER's two blocks, so no crawler may be placed here, and the room has a
   sound bank of its own (SND_BANK_KENNELS, src/sound.h).

   THE LEVER PUZZLE (src/kennels_puzzle.h). Four cells - the north-west bay,
   the two kennels, the north-east bay - each behind a Bars gate in its
   mouth, and four Levers on the east wall x=1800; a cell opens when exactly
   its levers are down. Two DEMON DOGS lie idle in each cell and come out
   when its gate lifts. kennels_init() places all of it.

   FIVE TEXTURES, ONE OF THEM ITS OWN: GULACODE.TIM (4bpp, x768 y0, on the arm
   switch's page AND palette - see kennels.c), STREAMED on entry rather than
   registered. Cobblestone and the inner door
   come through src/catacombs_entry.c's narrow uploaders, the gula tablet
   through src/gula_tablet.c's and the bars through src/bars.c's.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void kennels_load_assets(void);     /* startup: five headers, no registration  */
void kennels_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void kennels_upload_textures(void); /* room entry: LoadImage + one CD stream    */
void kennels_init(void);            /* collision + floor zone + spawn           */
void kennels_draw(RenderContext *ctx);

/* Arrival through the south door, from the Nursery - and the only arrival
   there is: just inside it, facing north up the hall. */
void kennels_spawn_south(void);

/* One frame of the south door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range and facing the door. */
int  kennels_south_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above. */
void kennels_arm(void);

#endif
