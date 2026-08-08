#ifndef ROOM_ARENA_H
#define ROOM_ARENA_H

/* The room-geometry arena: one buffer, holding the mesh of the room the player
 * is currently standing in.
 *
 * WHY IT IS ONE BUFFER AND NOT FOURTEEN MALLOCS
 * ---------------------------------------------
 * Every room's .smd used to be read at startup and kept for the whole run —
 * about 518 KB of a ~1.5 MB heap, of which thirteen fourteenths was geometry
 * for rooms nobody was in. Streaming them in and out with malloc/free would
 * have handed that back, but PSn00bSDK's allocator is a first-fit free list:
 * cycling 20-64 KB blocks through it for an hour, interleaved with the small
 * per-texture allocations texmgr makes, fragments the heap until some later
 * allocation fails for no reason the player's route explains. A fixed arena
 * cannot fragment. It costs one worst-case room (see room_arena_size.h) in BSS
 * and nothing at all in allocator behaviour.
 *
 * WHY IT IS SAFE TO OVERWRITE THE OUTGOING ROOM
 * ---------------------------------------------
 * Only ONE room is ever drawn. Collision and floor heights do NOT come from the
 * mesh — they are compile-time arrays in <room>_mesh_collision.c and hardcoded
 * zone tables in each room's floor_zones_init — so a room whose geometry has
 * been evicted still collides correctly right up to the moment it stops being
 * drawn. Entity state lives in world.c, which never touches the mesh either.
 * Each room's SMD pointer is private to its own module; nothing reads another
 * room's geometry.
 *
 * THE ONE RULE
 * ------------
 * room_arena_load() does a CdRead, so it must not run while CD-DA is playing —
 * a data read issued under audio streaming hangs the drive (this is the June
 * 2026 crash documented in tools/TEXTURE_STREAMING_DEBUG.txt). It brackets
 * itself with cdaudio_suspend/resume, so callers do not have to, but callers
 * DO have to be somewhere it is legal to block for a few hundred milliseconds:
 * the STATE_LOADING transition, behind the door animation's fade to black.
 * Never from a draw or update path.
 *
 * AND ONE TRAP
 * ------------
 * smdInitData() REBASES THE BUFFER IN PLACE — it reads three file-relative
 * offsets out of the header, adds the buffer's address to each, and writes them
 * back (eleven instructions, no allocation, returns the buffer you gave it).
 * Running it twice on one buffer adds the base twice and every pointer in the
 * model is then garbage. So each <room>_load_geometry() must call it on a
 * FRESHLY READ buffer, which is why they are all written as
 *
 *     buff = room_arena_load(path);
 *     smd  = buff ? smdInitData(buff) : NULL;
 *
 * and never cache the result across a load. Re-entering the same room re-reads
 * it from the disc, so the buffer is always pristine.
 */

/* Read `filename` off the disc into the arena and return its base address, or
 * NULL if the file is missing or larger than the arena (in which case the arena
 * is left untouched and the caller's SMD pointer should go NULL — the room then
 * draws nothing, which is visible, rather than reading past the buffer, which
 * is not). Whatever the arena previously held is gone.
 */
void *room_arena_load(const char *filename);

/* Bytes the last successful load occupied. Diagnostics only. */
int room_arena_used(void);

#endif /* ROOM_ARENA_H */
