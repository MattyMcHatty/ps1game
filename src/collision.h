#ifndef COLLISION_H
#define COLLISION_H

#include <stdint.h>

#define MAX_WALLS_PER_ROOM 32

typedef struct {
    int32_t x1, z1;
    int32_t x2, z2;
    int32_t nx, nz;  /* inward-facing normal, fixed-point: 4096 = 1.0 */
} Wall;

typedef struct {
    Wall    walls[MAX_WALLS_PER_ROOM];
    int     wall_count;
    int32_t min_x, max_x;
    int32_t min_z, max_z;
} CollisionRoom;

extern CollisionRoom current_collision_room;

int  collide_wall(Wall *w, int32_t *px, int32_t *pz, int32_t radius);
void collision_init(void);

#endif
