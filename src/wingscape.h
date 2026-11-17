#ifndef WINGSCAPE_H
#define WINGSCAPE_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   WINGSCAPE - Chapter 4's fourth room
   =========================================================================
   Behind Hell Hath No Fury's north-west door.

   THE SHAPE. x[-3300,3300] z[-2700,3100], TWO STOREYS:
     - THE GROUND, y=0, the southern two thirds: volcanic rock with a lava
       field in its south-west (x<1500, z<100) that burns (src/lava.h, its
       mask from tools/gen_lava_floors.py).
     - THE TERRACE, y=-500, a rock shelf wrapping the north end, reached by
       two ramps rising north: the EAST ramp x[2100,3300] z[-100,1300] and
       the WEST ramp x[-3300,-2500] z[-500,700]. Its south edge is a steep
       rock face (~z=1300-1500) the player can step off but not climb.
     - THE WINGS TABLET (src/wings_tablet.h) stands on the terrace in the
       alcove at the north rim, z~2930.
   Under the Underworld's sky (src/underworld_sky.h), centred on the room.

   ONE DOOR, a catacomb inner door on the cut south-east corner (2900,-2300):
   to Hell Hath No Fury's NORTH-WEST door (wooden).

   >>> THE TRANSITION DEPENDS ON THE DIRECTION, NOT THE DOORWAY. <<< Out of
   here through the catacomb door plays DOOR_PANEL_CATACOMB; into here from
   Hell Hath No Fury through its wooden door plays DOOR_PANEL_WOOD. See the
   Chapter 4 block of tools/ADDING_A_ROOM.txt.

   The wooden door on the WEST wall (x=-3300, z~-1400) is DECORATION for now:
   no trigger, no sign. It is wired up when the room behind it exists.

   ONE ENEMY: a GOLEM in front of that west door, (-2800,-1400)
   (src/golem.h, seeded in world.c) - and a second, the WINGS TABLET's
   GUARD, seeded ABSENT in front of the tablet and summoned by every wrong
   answer to its puzzle (src/wings_tablet.h). THE MUSIC is the chapter's (main.c's Chapter 4 block). Chapter
   rules (the fog, the sky): src/underworld.h. */

void wingscape_load_assets(void);     /* startup: TIM_SLOTs only, no CD access */
void wingscape_load_geometry(void);   /* ROOM ENTRY: mesh + the sky            */
void wingscape_upload_textures(void); /* room entry: pure LoadImage            */
void wingscape_init(void);            /* collision, floors, default spawn      */
void wingscape_draw(RenderContext *ctx);

/* The south-east door, to Hell Hath No Fury's north-west door. A fresh Circle
   within reach and facing it. THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE
   LOCKED, so a Circle held across a menu closing does not read as a fresh
   press. */
void wingscape_doors_arm(void);
int  wingscape_se_door_triggered(int lock);

/* Arrival through the south-east door; also the default spawn. */
void wingscape_spawn_se(void);

#endif
