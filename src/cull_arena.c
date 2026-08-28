#include "cull_arena.h"

/* The arena itself. See cull_arena.h for why there is one of it. */
CullKey cull_keys[CULL_ARENA_PRIMS];
CullBox cull_boxes[CULL_ARENA_PRIMS];

/* Every room that builds into the arena, checked against its size at compile
   time. A mesh re-export that pushes a room past CULL_ARENA_PRIMS fails HERE,
   loudly, instead of writing past the end of the arena at load — which would
   land in whatever the linker put next and would not be visible until something
   unrelated broke. Add a room to the arena, add it to this list. */
#include "chain_room_tex_map.h"
#include "greenhouse_tex_map.h"
#include "keystone_maze_tex_map.h"
#include "maze_one_tex_map.h"
#include "maze_two_tex_map.h"
#include "rear_gate_tex_map.h"
#include "stables_tex_map.h"

/* A negative-width bitfield is the portable form of a static assert and needs
   nothing from the C standard the -nostdlib toolchain does not already give. */
#define CULL_ARENA_FITS(name, count) \
    struct cull_arena_fits_##name { int fits : ((count) <= CULL_ARENA_PRIMS) ? 1 : -1; }

CULL_ARENA_FITS(chain_room,    CHAIN_ROOM_PRIM_COUNT);
CULL_ARENA_FITS(greenhouse,    GREENHOUSE_PRIM_COUNT);
CULL_ARENA_FITS(keystone_maze, KEYSTONE_MAZE_PRIM_COUNT);
CULL_ARENA_FITS(maze_one,      MAZE_ONE_PRIM_COUNT);
CULL_ARENA_FITS(maze_two,      MAZE_TWO_PRIM_COUNT);
CULL_ARENA_FITS(rear_gate,     REAR_GATE_PRIM_COUNT);
CULL_ARENA_FITS(stables,       STABLES_PRIM_COUNT);
