/*
 * Collision data for DESCENT TO HELL - hand-written, see the .c: the room is
 * a cutscene and has no collision mesh, so it has zero walls.
 */

#ifndef DESCENT_TO_HELL_MESH_COLLISION_H
#define DESCENT_TO_HELL_MESH_COLLISION_H

#include "collision.h"

#define DESCENT_TO_HELL_WALL_COUNT  0
#define DESCENT_TO_HELL_FLOOR_COUNT 0

void descent_to_hell_collision_init(CollisionRoom *r);

#endif /* DESCENT_TO_HELL_MESH_COLLISION_H */
