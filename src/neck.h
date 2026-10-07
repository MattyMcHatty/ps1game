#ifndef NECK_H
#define NECK_H

#include <stdint.h>
#include "render.h"

/* The Neck: Chapter 3's THIRTY-FIRST room, through the Room of Baby Names'
   north-east door - the one with the blank plate over it.

   THE SHAPE. A long hall, x[0,3000] z[0,1800], flat at y=0, walls and ceiling
   at y=-800, with two square stone PILLARS standing full height down its
   middle: x[600,1200] and x[1800,2400], both z[600,1200]. A catacomb inner
   door at z[800,1000] in each end wall:

     WEST  x=0     -> the Room of Baby Names, the blank-plate door
     EAST  x=2999  -> the Head, its west door

   THE SAVE POINT stands in the north-east corner, 200 off both walls.

   Nothing is seeded in it.

   TWO TEXTURES, BOTH BORROWED: cobblestone and the inner door, through
   src/catacombs_entry.c's narrow uploaders.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void neck_load_assets(void);     /* startup: two compile-time headers      */
void neck_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void neck_upload_textures(void); /* room entry: pure LoadImage from RAM    */
void neck_init(void);            /* collision + floor zone + spawn + save point */
void neck_draw(RenderContext *ctx);

/* Arrival through the west door, from the Room of Baby Names - and
   neck_init()'s default spawn: just inside it, facing east down the hall. */
void neck_spawn_west(void);

/* Arrival through the east door, back from the Head: just inside it, facing
   west down the hall. main.c calls it after neck_init(). */
void neck_spawn_east(void);

/* One frame of each door's Circle test. `lock` is main's usual suppression
   (and the save point's veto). Returns 1 on a fresh press made in range and
   facing the door. Call both every frame, for their edge state. */
int  neck_west_door_triggered(int lock);
int  neck_east_door_triggered(int lock);

/* Arm every interaction in the room (both doors and the save point). Called by
   the spawn above. */
void neck_arm(void);

#endif
