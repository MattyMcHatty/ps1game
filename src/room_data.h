#ifndef ROOM_DATA_H
#define ROOM_DATA_H

#include <stdint.h>
#include "collision.h"

/* THE CURRENT ROOM'S PER-POLY TABLES AND COLLISION WALLS, READ FROM THE DISC.
 *
 * Every room's texture map, no-cull table and collision walls used to be
 * compiled into the executable (src/<slug>_tex_map.h, src/<slug>_mesh_
 * collision.c), which kept all 43 rooms' copies in RAM for the whole game —
 * ~118 KB, in every area, for tables only ever read while standing in their
 * own room. tools/pack_rooms.py now appends them to the END of the room's .SMD
 * (build/rooms/, which disc.xml reads), so they arrive in the room arena with
 * the mesh and only the current room's copy exists. The generated .h/.c files
 * are still the sources; the packer reads them and fails the build on anything
 * it cannot represent. See tools/RAM_RECLAIM_CHECKLIST.txt PART A for why, and
 * the docstring of tools/pack_rooms.py for the byte layout — the two must
 * agree, and ROOM_DATA_VERSION below is the handshake.
 *
 * HOW A ROOM USES IT, three lines that replace the old three:
 *
 *   _load_geometry:  buff = room_arena_load(path);
 *                    smd  = buff ? smdInitData(buff) : NULL;
 *                    if (smd && !room_data_bind(smd->n_prims)) smd = NULL;
 *   draw loop:       room_tex_map[i]      (was <slug>_tex_map[i], same values)
 *                    room_nocull(i)       (was <slug>_nocull[i])
 *   _init:           room_data_collision(&current_collision_room);
 *                                         (was <slug>_collision_init(...))
 *
 * A FAILED BIND MEANS THE MESH GOES NULL, so the room draws nothing — loud,
 * the room_arena_load() convention — rather than indexing a table that is not
 * there. And room_data_collision() with nothing bound zeroes the room: no
 * walls, which is equally loud, rather than the LAST room's walls, which is the
 * silent version of the same bug. room_arena_load() unbinds on every read, so
 * a stale bind cannot outlive the file it described.
 *
 * <SLUG>_PRIM_COUNT still comes from src/<slug>_tex_map.h, for the cull-key
 * clamp and CULL_ARENA_FITS. Nothing else in that header is compiled in: the
 * arrays are static const and unreferenced, so the compiler drops them. */

#define ROOM_DATA_VERSION 1

/* Per primitive: the engine texture slot, 0xFF = untextured. Exactly the
   values src/<slug>_tex_map.h holds. NULL while nothing is bound. */
extern const uint8_t *room_tex_map;

/* Per primitive, one BIT: never backface-cull it (a "triangle-shaped" quad
   whose winding the GTE cannot judge — see any gen_<slug>_tex_map.py). */
extern const uint8_t *room_nocull_bits;
static inline int room_nocull(int i) {
    return (room_nocull_bits[i >> 3] >> (i & 7)) & 1;
}

/* Find the block at the end of the file room_arena_load() just read. Returns 1
   and points the two tables at it if the block is present, current and
   describes exactly `n_prims` primitives (the SMD's own count); 0 otherwise. */
int  room_data_bind(int n_prims);

/* Forget the bound block. room_arena_load() calls this before every read. */
void room_data_unbind(void);

/* Fill `r` with the bound room's walls, bounds, multi_level and
   shoot_over_mask — everything <slug>_collision_init() used to write, and
   nothing else (ceiling_y stays the room's own collision_set_ceiling_y() call).
   With nothing bound, `r` gets no walls at all. */
void room_data_collision(CollisionRoom *r);

#endif /* ROOM_DATA_H */
