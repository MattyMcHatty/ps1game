/*
 * ASAG'S ARENA - PLACEHOLDER COLLISION. Hand-written; see the header.
 *
 * STILL A PLACEHOLDER, BUT NO LONGER AN INVENTED ONE. It is now the modelled
 * arena's BOUNDING BOX, read off assets/bosses/Asag/Asag-Arena.smx:
 *
 *   Bounds: X(-1500 to 1500)  Z(0 to 3700)   floor y=0, walls y[-1000,0]
 *   Normal scale: 4096 = 1.0 (fixed point), matching the generator's output.
 *
 * >>> IT IS A BOX AROUND A MESH THAT IS NOT A BOX, AND THAT IS THE WHOLE
 * CAVEAT. <<< The mesh has pillars, steps up to y=-1624 and detail along its
 * walls, and NONE of it stops the player: this file describes the perimeter and
 * nothing inside it. That is deliberate and temporary - the mesh went in to be
 * looked at and paced out, not to be fought in. Walking through a pillar down
 * there is this file, not a broken mesh.
 *
 * WHEN THE ARENA IS FOR REAL, DELETE THIS FILE AND GENERATE IT. Export the
 * collision proxy alongside the visual mesh and follow tools/ADDING_A_ROOM.txt
 * STEP 2 exactly as for any other room:
 *
 *     py tools\smx_to_collision.py "assets\bosses\Asag\Asag-Arena_mesh.smx" --room asag_arena
 *
 * re-applying the two hand-edits the generator does not emit (the lowercase
 * #include, and r->multi_level / r->shoot_over_mask).
 *
 * WHY THE WALLS ARE 1000 TALL: that is where the mesh's perimeter walls top
 * out, and asag_arena_init()'s collision_set_ceiling_y() takes the same number.
 * The mesh reaches y=-1624 in places; a ceiling probe wants the height over the
 * walkable ground and not the tallest thing in the room.
 *
 * multi_level = 0: the mesh's walkable ground is one flat plane at y=0 (1656 of
 * its vertices sit there and none of the floor is anywhere else), so the shared
 * wall routine is the whole of the collision. shoot_over_mask = 0: this box has
 * no low walls in it.
 * >>> BOTH MUST BE RE-DECIDED WHEN THE REAL COLLISION IS GENERATED. <<<
 * collision_segment_blocked() only Y-gates walls in a multi_level room, so if
 * the finished proxy has a knee-high lip or a step anywhere in it, EVERY
 * projectile - the boss's and the player's - dies on it until asag_arena_init()
 * calls collision_shoot_over_short_walls() with a threshold read out of this
 * file's y spans. That trap cost the Rabisu fight a whole terrace; see
 * tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 6.
 */

#include "asag_arena_mesh_collision.h"

void asag_arena_collision_init(CollisionRoom *r) {
    r->wall_count = ASAG_ARENA_WALL_COUNT;
    r->multi_level = 0;      /* flat: the single floor plane is y=0 */
    r->shoot_over_mask = 0;  /* no low walls; nothing is shot over here yet */
    r->min_x = -1500;
    r->max_x =  1500;
    r->min_z =     0;
    r->max_z =  3700;

    /* Wall 0 - NORTH (z = 0), normal +Z: pushes back into the room. */
    r->walls[0].x1 =   -1500;  r->walls[0].z1 =       0;
    r->walls[0].x2 =    1500;  r->walls[0].z2 =       0;
    r->walls[0].nx =       0;  r->walls[0].nz =    4096;
    r->walls[0].y_min = -1000; r->walls[0].y_max =    0;

    /* Wall 1 - SOUTH (z = 3700), normal -Z. */
    r->walls[1].x1 =    1500;  r->walls[1].z1 =    3700;
    r->walls[1].x2 =   -1500;  r->walls[1].z2 =    3700;
    r->walls[1].nx =       0;  r->walls[1].nz =   -4096;
    r->walls[1].y_min = -1000; r->walls[1].y_max =    0;

    /* Wall 2 - WEST (x = -1500), normal +X. */
    r->walls[2].x1 =   -1500;  r->walls[2].z1 =    3700;
    r->walls[2].x2 =   -1500;  r->walls[2].z2 =       0;
    r->walls[2].nx =    4096;  r->walls[2].nz =       0;
    r->walls[2].y_min = -1000; r->walls[2].y_max =    0;

    /* Wall 3 - EAST (x = +1500), normal -X. */
    r->walls[3].x1 =    1500;  r->walls[3].z1 =       0;
    r->walls[3].x2 =    1500;  r->walls[3].z2 =    3700;
    r->walls[3].nx =   -4096;  r->walls[3].nz =       0;
    r->walls[3].y_min = -1000; r->walls[3].y_max =    0;
}
