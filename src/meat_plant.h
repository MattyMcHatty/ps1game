#ifndef MEAT_PLANT_H
#define MEAT_PLANT_H

#include <stdint.h>
#include "render.h"

/* The Meat Plant: Chapter 3's THIRTEENTH room, through the SOUTH of the two
   doors in the Sliding Bars Room's east wall (x=4200, z[200,400]).

   THE SHAPE. A hall x[-2700,2700] z[600,6000], flat at y=0 under a vault at
   y=-800, with a 600-deep alcove in the middle of each side:

       north   x[-900,900]    z[6000,6600]
       south   x[-900,900]    z[0,600]       the south door at its back
       west    x[-3299,-2700] z[2399,4199]   the west-alcove door at its back
       east    x[2700,3299]   z[2399,4199]

   In the middle of the hall stands ONE MASS OF LEGS, a C open to the east,
   drawn to y=-730 and walled in the proxy at y[-600,0] (walls 0-2 and 23-76):

       the south arm   z~[1199,2800], stepping up from x=-300 in the middle to
                       x=+-1700 at its ends
       the spine       x[-2100,-1100], z[2399,4600]
       the north arm   z~[4199,5399], x[-2100,1700]
       the mouth       x~[1100,1700], z[2800,3999], opening east

   So what the player walks is a ring round the mass, 600 wide at its narrowest
   (between the spine and the west wall), and a courtyard inside the C reached
   through the mouth. The mass is taller than the proxy, so nothing is
   shoot-over.

   THE DOORS. Three are drawn and all three are wired up:

     WEST    x=-2700  z[5400,5600]  -> the Sliding Bars Room, south-east door.
                                       The northern of the two doors on the
                                       west side. YZ plane, approached from +X:
                                       mirror=0.
     WEST ALCOVE  x=-3300  z[3200,3400]  -> the Room of Bones, at its one door.
                                       At the back of the west alcove, the
                                       middle of the west side. YZ plane,
                                       approached from +X: mirror=0.
     SOUTH ALCOVE z=0  x[-100,100]   -> Cleaver L, at its north door. At the
                                       back of the south alcove, the middle of
                                       the south side. XY plane, approached
                                       from +Z: mirror=1.

   THREE TEXTURES, ALL BORROWED: `rusty` from The Pit, `legs` from the Room of
   Legs and the catacomb inner door from the Catacombs Entry, each through its
   owner's narrow uploader. The room registers nothing.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void meat_plant_load_assets(void);     /* startup: three headers, no CD        */
void meat_plant_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void meat_plant_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void meat_plant_init(void);            /* collision + floor zone + spawn        */
void meat_plant_draw(RenderContext *ctx);

/* Arrival through the west door, from the Sliding Bars Room: just inside it,
   facing east along the hall's north side. The default arrival. */
void meat_plant_spawn_west(void);

/* Arrival through the west-alcove door, back from the Room of Bones: just
   inside it, facing east out of the alcove into the hall. */
void meat_plant_spawn_west_alcove(void);

/* Arrival through the south-alcove door, back from Cleaver L: just inside it,
   facing north out of the alcove into the hall. */
void meat_plant_spawn_south_alcove(void);

/* One frame of the west door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door — the frame
   main.c starts the transition on. */
int  meat_plant_west_door_triggered(int lock);

/* The same test for the west-alcove door, into the Room of Bones. */
int  meat_plant_west_alcove_door_triggered(int lock);

/* ...and for the south-alcove door, into Cleaver L. */
int  meat_plant_south_alcove_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void meat_plant_arm(void);

#endif
