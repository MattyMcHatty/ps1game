#ifndef ASAG_ARENA_MESH_COLLISION_H
#define ASAG_ARENA_MESH_COLLISION_H

#include "collision.h"

/* >>> HAND-WRITTEN PLACEHOLDER, NOT GENERATOR OUTPUT. <<<
   Every other *_mesh_collision.c in src/ is smx_to_collision.py's output copied
   out of assets/. This one is not: Asag's arena has a VISUAL mesh but no
   collision proxy exported for it yet, so this file states that mesh's
   BOUNDING BOX by hand so the room can be entered and
   walked while the mesh is being judged for size and feel. Nothing INSIDE the
   perimeter collides.

   WHEN THE COLLISION PROXY IS EXPORTED, THIS FILE IS DELETED AND REGENERATED. Follow
   tools/ADDING_A_ROOM.txt STEP 2 exactly as for any other room:

       py tools\smx_to_collision.py "assets\bosses\Asag\Asag-Arena_mesh.smx"

   and copy its output over both halves of this pair, re-applying the two
   hand-edits the generator does not emit (the lowercase #include, and
   r->multi_level / r->shoot_over_mask). Nothing else in the room module cares:
   asag_arena.c calls asag_arena_collision_init() and reads nothing else here. */

#define ASAG_ARENA_WALL_COUNT  4
#define ASAG_ARENA_FLOOR_COUNT 1

void asag_arena_collision_init(CollisionRoom *r);

#endif /* ASAG_ARENA_MESH_COLLISION_H */
