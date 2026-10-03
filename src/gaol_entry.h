#ifndef GAOL_ENTRY_H
#define GAOL_ENTRY_H

#include <stdint.h>
#include "render.h"

/* The Gaol Entry: Chapter 3's TWENTIETH room, through the Up Down Maze's
   east-upper door (x=3900, z[-100,100], on the y=-1000 walkway storey).

   THE SHAPE. One plain box, x[-300,899] z[-500,1700], flat at y=0 under a
   y=-800 ceiling, empty. In its east wall is the GAOL DOOR, and through the
   door's bars the cells beyond can be seen: bars over x[900,2700], drawn but
   outside the collision box, so the player looks into them and cannot walk
   in.

   THE DOORS. Two are drawn and ONE is wired up:

     WEST   x=-300  z[-300,-100]  y[-400,0]  -> Up Down Maze, east-upper door
     gaol   x=899   z[300,700]    y[-400,0]     not built (the cells)

   The west door is in the YZ plane approached from +X (wall 2 runs x=-300
   with nx=+4096), so TEXT_PLANE_YZ with mirror=0. No storey test here: this
   room is flat (the Up Down Maze's side of the door takes one). The gaol door
   is drawn and nothing else: no sign, no trigger, no collision gap.

   FOUR TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the catacomb inner door
   come through src/catacombs_entry.c's narrow uploaders and the bars through
   src/bars.c's. `gaol door` is new art only this room draws, 4bpp, registered
   DEFERRED on the ROOM OF ARMS' page (x640 y0) with a palette of its own at
   (576,502). See gaol_entry_load_assets().

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void gaol_entry_load_assets(void);     /* startup: one deferred reg + headers */
void gaol_entry_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void gaol_entry_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void gaol_entry_init(void);            /* collision + floor zone + spawn        */
void gaol_entry_draw(RenderContext *ctx);

/* Arrival through the west door, from the Up Down Maze, and the only arrival
   there is: just inside it, facing east across the room to the gaol door. */
void gaol_entry_spawn_west(void);

/* One frame of the west door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door — the frame
   main.c starts the transition on. */
int  gaol_entry_west_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void gaol_entry_arm(void);

#endif
