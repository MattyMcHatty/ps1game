/*
 * ASAG'S ARENA — PLACEHOLDER COLLISION. Hand-written; see the header.
 *
 * A 4000 x 4000 square box, floor at y=0, walls 900 tall (y[-900,0]), normals
 * facing INWARD so the push is off the perimeter and into the room. The numbers
 * are round on purpose: nothing here is mined from a mesh, so nothing here
 * should look as though it was.
 *
 *   Bounds: X(-2000 to 2000)  Z(-2000 to 2000)
 *   Normal scale: 4096 = 1.0 (fixed point), matching the generator's output.
 *
 * WHY 4000 SQUARE. It is sized off the ONE boss fight the game has already
 * built, so the number is an argument rather than a guess. The Rabisu sweeps an
 * arc of radius 1432 across the Garden Courtyard's sunken lawn, which is about
 * 2864 x 2288 of usable ground; at 80% reach (tools/ADDING_A_BOSS_ENCOUNTER.txt
 * STEP 6) that fight actually uses ~2300 of width. 4000 square leaves a boss of
 * the Rabisu's build room to sweep, to charge and to be circled, with the walls
 * far enough out that the player is never fighting in a corner.
 *
 * WHY 900 TALL. The Rabisu is RBS_HEIGHT = 559 and hovers; the courtyard's crane
 * shot frames it from y=200. 900 clears a boss half again that tall and gives a
 * reveal camera somewhere to be. It is a DRAWN roofline as well as a collision
 * one here, because there is no mesh yet to disagree with it — the moment there
 * is, read the height off the .smx and not off this file (the "visual vs
 * collision heights" rule in tools/ADDING_A_ROOM.txt).
 *
 * multi_level = 0: one flat plane, so the shared wall routine is the whole of
 * the collision. shoot_over_mask = 0: there are no low walls to shoot over yet.
 * >>> BOTH OF THOSE ARE LOAD-BEARING FOR THE FIGHT AND MUST BE RE-DECIDED WHEN
 * THE REAL ARENA EXISTS. <<< collision_segment_blocked() only Y-gates walls in a
 * multi_level room, so if the finished arena has a knee-high lip or a step
 * anywhere in it, EVERY projectile — the boss's and the player's — dies on it
 * until asag_arena_init() calls collision_shoot_over_short_walls() with a
 * threshold read out of this file's y spans. That trap cost the Rabisu fight a
 * whole terrace; see tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 6.
 */

#include "asag_arena_mesh_collision.h"

void asag_arena_collision_init(CollisionRoom *r) {
    r->wall_count = ASAG_ARENA_WALL_COUNT;
    r->multi_level = 0;      /* flat: the single floor plane is y=0 */
    r->shoot_over_mask = 0;  /* no low walls; nothing is shot over here yet */
    r->min_x = -2000;
    r->max_x =  2000;
    r->min_z = -2000;
    r->max_z =  2000;

    /* Wall 0 — NORTH (z = -2000), normal +Z: pushes back into the room. */
    r->walls[0].x1 =   -2000;  r->walls[0].z1 =   -2000;
    r->walls[0].x2 =    2000;  r->walls[0].z2 =   -2000;
    r->walls[0].nx =       0;  r->walls[0].nz =    4096;
    r->walls[0].y_min =  -900;  r->walls[0].y_max =      0;

    /* Wall 1 — SOUTH (z = +2000), normal -Z. */
    r->walls[1].x1 =    2000;  r->walls[1].z1 =    2000;
    r->walls[1].x2 =   -2000;  r->walls[1].z2 =    2000;
    r->walls[1].nx =       0;  r->walls[1].nz =   -4096;
    r->walls[1].y_min =  -900;  r->walls[1].y_max =      0;

    /* Wall 2 — WEST (x = -2000), normal +X. */
    r->walls[2].x1 =   -2000;  r->walls[2].z1 =    2000;
    r->walls[2].x2 =   -2000;  r->walls[2].z2 =   -2000;
    r->walls[2].nx =    4096;  r->walls[2].nz =       0;
    r->walls[2].y_min =  -900;  r->walls[2].y_max =      0;

    /* Wall 3 — EAST (x = +2000), normal -X. */
    r->walls[3].x1 =    2000;  r->walls[3].z1 =   -2000;
    r->walls[3].x2 =    2000;  r->walls[3].z2 =    2000;
    r->walls[3].nx =   -4096;  r->walls[3].nz =       0;
    r->walls[3].y_min =  -900;  r->walls[3].y_max =      0;
}
