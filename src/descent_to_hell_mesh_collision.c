/*
 * Collision data for DESCENT TO HELL - HAND-WRITTEN, NOT GENERATED.
 * Walls:  0
 * Floors: 0
 * Bounds: X(-1800 to 1800)  Z(-1800 to 1800)   (the visual mesh's box)
 *
 * There is no "Descent to Hell mesh.smx" and there will not be one: the room
 * is a cutscene from its first frame to its last, so the player never walks in
 * it and nothing collides. This file exists only because tools/pack_rooms.py
 * packs every room's walls onto its .SMD and room_data_bind() expects the
 * block - zero walls is a legal block.
 */

#include "descent_to_hell_mesh_collision.h"

void descent_to_hell_collision_init(CollisionRoom *r) {
    r->wall_count = DESCENT_TO_HELL_WALL_COUNT;
    /* No walls at all, so neither flag has anything to describe. */
    r->multi_level     = 0;
    r->shoot_over_mask = 0;
    r->min_x = -1800;
    r->max_x = 1800;
    r->min_z = -1800;
    r->max_z = 1800;
}
