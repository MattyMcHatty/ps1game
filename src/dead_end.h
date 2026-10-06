#ifndef DEAD_END_H
#define DEAD_END_H

#include <stdint.h>
#include "render.h"

/* The Dead End: ONE mesh, ONE module, FIVE rooms. Chapter 3's twenty-fourth
   to twenty-eighth rooms, each behind one of the Room of Baby Names' named
   doors:

       STATE_DEAD_END_BENJ      north-west door   a Lumberer
       STATE_DEAD_END_MATTHEW   north door        a Crawler
       STATE_DEAD_END_CHRISTOF  east door         Flame Rounds
       STATE_DEAD_END_LUKE      south-east door   a small medipac
       STATE_DEAD_END_MARK      south-west door   a Lumberer

   THE SHAPE. A plain cobblestone corridor two squares long: x[-300,300],
   z[0,1200], flat at y=0, walls and ceiling at y=-800. One catacomb inner door
   in the SOUTH wall, x[-100,100] at z=0, which leads back to the Room of Baby
   Names whichever Dead End this is.

   WHAT IS IN IT is decided by world.c, not here: each room's one occupant is
   seeded by world_seed_room() at (0, 900) — the middle of the NORTH square,
   opposite the door — and it is the area tag (the enemies) or the room slot
   (the pickups) that keeps the five apart. This module draws, collides and
   wires the door for all five alike and never asks which one it is in.

   TWO TEXTURES, BOTH BORROWED: cobblestone and the inner door, through
   src/catacombs_entry.c's narrow uploaders.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void dead_end_load_assets(void);     /* startup: two compile-time headers      */
void dead_end_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void dead_end_upload_textures(void); /* room entry: pure LoadImage from RAM    */
void dead_end_init(void);            /* collision + floor zone + spawn         */
void dead_end_draw(RenderContext *ctx);

/* Arrival through the south door, and the only arrival there is: just inside
   it, facing north up the corridor. */
void dead_end_spawn_door(void);

/* One frame of the door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door. */
int  dead_end_door_triggered(int lock);

/* Arm every interaction in the room (the door). Called by the spawn above. */
void dead_end_arm(void);

#endif
