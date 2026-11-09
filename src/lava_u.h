#ifndef LAVA_U_H
#define LAVA_U_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   LAVA U - Chapter 4's third room
   =========================================================================
   Behind Sharur's Trial's two southern doors.

   THE SHAPE. A U of lava, x[-6900,2100] z[-5100,300], floor at y=0: two arms
   running south from the north rim - the west arm x[-6900,-3100], the east
   arm x[-1300,2100] - joined along the south, around a rock island in the
   middle. Volcanic paths cross the lava; the lava burns (src/lava.h, its mask
   from tools/gen_lava_floors.py). Under the Underworld's sky
   (src/underworld_sky.h), centred on the room. The collision proxy's rim and
   the island stop the player; one flat floor zone.

   TWO DOORS, both wooden, both in the north rim (z=300):
     - NORTH-EAST, at x=-6: to Sharur's Trial's SOUTH door (a catacomb inner
       door at (0,-1500) - the user's "south east" door, being the eastern of
       that room's two southern doors).
     - NORTH-WEST, at x=-4730: to Sharur's Trial's SOUTH-WEST door (wooden).

   >>> THE TRANSITION DEPENDS ON THE DIRECTION, NOT THE DOORWAY. <<< Each
   side's transition is the door the player actually walked up to: out of
   here both doors are wooden, so both play DOOR_PANEL_WOOD; into here from
   Sharur's Trial its catacomb door plays DOOR_PANEL_CATACOMB and its wooden
   south-west door DOOR_PANEL_WOOD. See the Chapter 4 block of
   tools/ADDING_A_ROOM.txt.

   No props. One kind of enemy, placed by world.c: three AHRIMANS - height B
   in the south-west corner, A by the north-west door, C in the south-east
   corner. THE MUSIC is the
   chapter's (main.c's Chapter 4 block). Chapter rules (the fog, the sky):
   src/underworld.h. */

void lava_u_load_assets(void);     /* startup: TIM_SLOTs only, no CD access */
void lava_u_load_geometry(void);   /* ROOM ENTRY: mesh + the sky            */
void lava_u_upload_textures(void); /* room entry: pure LoadImage            */
void lava_u_init(void);            /* collision, floor, default spawn       */
void lava_u_draw(RenderContext *ctx);

/* The two doors, both to Sharur's Trial. A fresh Circle within reach and
   facing the door. THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a
   Circle held across a menu closing does not read as a fresh press. */
void lava_u_doors_arm(void);
int  lava_u_ne_door_triggered(int lock);   /* -> Sharur's south door      */
int  lava_u_nw_door_triggered(int lock);   /* -> Sharur's south-west door */

/* Arrivals. The north-east one is also the default spawn. */
void lava_u_spawn_ne(void);
void lava_u_spawn_nw(void);

#endif
