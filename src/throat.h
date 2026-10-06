#ifndef THROAT_H
#define THROAT_H

#include <stdint.h>
#include "render.h"

/* The Throat: Chapter 3's twenty-ninth room, through the Room of Baby Names'
   south door - the one with ANTONI over it.

   THE SHAPE. The Dead End's corridor (src/dead_end.h) with a second doorway:
   x[-300,300], z[0,1200], flat at y=0, walls and ceiling at y=-800, and a
   catacomb inner door at x[-100,100] in BOTH end walls.

     NORTH  z=1199  -> the Room of Baby Names, Antoni's door
     SOUTH  z=0     -> the Room of Guts (src/room_of_guts.h), its one door

   Nothing is seeded in it.

   TWO TEXTURES, BOTH BORROWED: cobblestone and the inner door, through
   src/catacombs_entry.c's narrow uploaders.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void throat_load_assets(void);     /* startup: two compile-time headers      */
void throat_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void throat_upload_textures(void); /* room entry: pure LoadImage from RAM    */
void throat_init(void);            /* collision + floor zone + spawn         */
void throat_draw(RenderContext *ctx);

/* Arrival through the north door, from the Room of Baby Names — the default
   throat_init() applies: just inside it, facing south down the corridor. */
void throat_spawn_north(void);

/* Arrival through the south door, from the Room of Guts: just inside it,
   facing north up the corridor. main.c applies it after throat_init(). */
void throat_spawn_south(void);

/* One frame of the north door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range and facing the door. */
int  throat_north_door_triggered(int lock);
int  throat_south_door_triggered(int lock);   /* the same, the south door */

/* Arm every interaction in the room (both doors). Called by both spawns
   above. */
void throat_arm(void);

#endif
