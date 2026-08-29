#ifndef ASAG_ARENA_MESH_COLLISION_H
#define ASAG_ARENA_MESH_COLLISION_H

#include "collision.h"

/* >>> HAND-WRITTEN PLACEHOLDER, NOT GENERATOR OUTPUT. <<<
   Every other *_mesh_collision.c in src/ is smx_to_collision.py's output copied
   out of assets/. This one is not: Asag's arena has no mesh yet, so this file
   states the arena's INTENDED footprint by hand so the room can be entered,
   walked and tested before any art exists.

   WHEN THE REAL MESH LANDS, THIS FILE IS DELETED AND REGENERATED. Follow
   tools/ADDING_A_ROOM.txt STEP 2 exactly as for any other room:

       py tools\smx_to_collision.py "assets\garden\Asag Arena mesh.smx"

   and copy its output over both halves of this pair, re-applying the two
   hand-edits the generator does not emit (the lowercase #include, and
   r->multi_level / r->shoot_over_mask). Nothing else in the room module cares:
   asag_arena.c calls asag_arena_collision_init() and reads nothing else here. */

#define ASAG_ARENA_WALL_COUNT  4
#define ASAG_ARENA_FLOOR_COUNT 1

void asag_arena_collision_init(CollisionRoom *r);

#endif /* ASAG_ARENA_MESH_COLLISION_H */
