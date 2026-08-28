#ifndef CULL_ARENA_H
#define CULL_ARENA_H

#include <stdint.h>

/* The cull-key arena: the reject-path tables for the room the player is
 * currently standing in. src/room_arena.h is the model and its argument is this
 * one's argument, one level up.
 *
 * WHAT THESE TABLES ARE
 * ---------------------
 * Every big room's draw loop walks its whole mesh once a frame and throws most
 * of it away. Doing that off the SMD costs a read of each primitive's header
 * for its stride, a read of its first vertex index, and a chase into a mesh of
 * up to 118 KB with no data cache behind it — a main-memory stall per rejected
 * primitive. So the reject path reads a flat table built once at load instead:
 *
 *   CullKey  the primitive's first vertex X/Z and its stride. Six sequential
 *            bytes, enough for the distance cull and for advancing the walk
 *            without touching the header. Read for EVERY primitive, every frame.
 *   CullBox  the primitive's XZ bounding box, for the side-plane frustum test.
 *            Read only by the primitives that survive the distance cull, so it
 *            is a separate array — folding it into the key would put 8 bytes of
 *            extra traffic on the hot path to save reads on the cold one.
 *
 * The measurements and the hole-free proof behind each live beside the room
 * that motivated them: src/greenhouse.c for the box, src/maze_one.c for the key
 * and for the box's second measurement.
 *
 * WHY THEY ARE ONE ARENA AND NOT SEVEN ARRAYS
 * -------------------------------------------
 * Seven rooms carry these tables and every one of them used to have its own,
 * all resident at once: 8800 primitives of key across the seven, plus the
 * Greenhouse's box, for 61 KB of BSS holding reject data for six rooms nobody
 * is in. That is the same waste room_arena.h was written to end, and it ends
 * the same way — the tables are indexed by a primitive number that is only
 * meaningful for the mesh currently in the room arena, so exactly one room's
 * are live at any moment. One arena sized to the worst room (Maze One's 2056
 * primitives) costs 28 KB and hands back 31 KB.
 *
 * IT IS SAFE FOR THE SAME REASON THE MESH ARENA IS. Only one room is ever
 * drawn; collision and floor heights come from compile-time tables, not from
 * the mesh or from these; and each room rebuilds its own keys inside its
 * <room>_load_geometry(), on the same call that reloads the mesh they describe.
 * A room whose keys have been overwritten is a room whose MESH has been
 * overwritten — it was already not drawable, and nothing else reads them.
 *
 * THE ONE RULE
 * ------------
 * Build the keys in <room>_load_geometry(), immediately after room_arena_load,
 * and never anywhere else. A room that rebuilt them on entry without reloading
 * the mesh would be describing whatever room ran last.
 */

/* Sized to the largest mesh on the disc, which is Maze One's. cull_arena.c
 * static-asserts every room that uses the arena against this, so a re-export
 * that grows a mesh past it is a compile error rather than an overrun.
 */
#define CULL_ARENA_PRIMS 2056

typedef struct { int16_t x, z; uint8_t stride, pad; } CullKey;
typedef struct { int16_t min_x, max_x, min_z, max_z; } CullBox;

extern CullKey cull_keys[CULL_ARENA_PRIMS];
extern CullBox cull_boxes[CULL_ARENA_PRIMS];

#endif /* CULL_ARENA_H */
