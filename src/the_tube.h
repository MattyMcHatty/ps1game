#ifndef THE_TUBE_H
#define THE_TUBE_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   THE TUBE - Chapter 4's twelfth room
   =========================================================================
   Behind Heavens Gate's EAST door.

   THE SHAPE. A rock-lined cylinder, radius 300, lying along Z: z[0,2400],
   its axis at x=0, y=-300, so it touches the floor plane y=0 at x=0 and its
   crown is at y=-600. It has no end caps - the chapter's sky shows through
   either end around the doors.

   >>> THE TUBE SPINS. <<< Nothing else does. The cylinder turns about its
   own long axis, a full revolution every THE_TUBE_SPIN_PERIOD frames, while
   the doors, the collision and the floor stay exactly where the model puts
   them. It is two exports in one mesh: "The Tube.smx" (the cylinder) and
   "The Tube doors.smx" (both doors), merged by tools/merge_smx.py with the
   cylinder FIRST, so the draw turns prims [0, THE_TUBE_SPIN_PRIMS) and leaves
   the rest. The spin is drawing only: the collision proxy is a plain box
   (four 600-tall walls and the floor at y=0) and the player walks on that.

   TWO DOORS:
     - SOUTH (z=0): a wooden door (wd_dr), distorted - it leans as it rises.
       It leads to HEAVENS GATE's east door, a catacomb inner door. Per
       direction (the transition is the door on the side being left): out of
       here DOOR_PANEL_WOOD, out of Heavens Gate DOOR_PANEL_CATACOMB.
     - NORTH (z=2400): a catacomb inner door, likewise distorted. It leads
       nowhere yet - no prompt and no trigger until its room exists.

   THE ARRIVAL. One: through the south door from Heavens Gate, just inside
   it facing north, down the tube (the_tube_spawn_s, the init's spawn and
   where a debug jump lands).

   ONE ENEMY: an ALERT GOLEM in front of the north door (src/golem.h),
   attacking from the moment the player arrives; it will block that door
   once the door is wired. No lava, no props. THE MUSIC is the chapter's
   (CDAUDIO_UNDERWORLD_TRACK, main.c's Chapter 4 block). Textures: three,
   all borrowed - see the_tube.c. Chapter rules (the fog, the sky):
   src/underworld.h. */

void the_tube_load_assets(void);     /* startup: TIM_SLOTs only            */
void the_tube_load_geometry(void);   /* ROOM ENTRY: mesh + the sky         */
void the_tube_upload_textures(void); /* room entry: narrow uploaders       */
void the_tube_init(void);            /* collision, floor, spawn            */
void the_tube_update(void);          /* the spin                           */
void the_tube_draw(RenderContext *ctx);

/* Seed the door's Circle edge state "held", so a press carried in through
   the transition cannot fire on the arrival frame. */
void the_tube_doors_arm(void);

/* The SOUTH door, to Heavens Gate: a fresh Circle within reach and facing
   it. The edge state is kept current while locked. */
int  the_tube_s_door_triggered(int lock);

/* Arrival through the south door: just inside it, facing north. */
void the_tube_spawn_s(void);

#endif
