#ifndef SAVE_POINT_H
#define SAVE_POINT_H

#include <stdint.h>
#include "render.h"

#define MAX_SAVE_POINTS 8

/* A reusable static prop (untextured, flat-shaded — the model carries its own
   per-face vertex colours). Load the geometry once at startup with
   save_points_init(); place instances per area with save_point_add() from that
   area's init (after save_points_clear()), and render them from that area's draw
   with save_points_draw().

   y is the room "standing on the floor" reference (e.g. -149 in the kitchen,
   -189 on reception's bottom floor); the draw adds GROUND_FLOOR_Y so the model's
   feet rest on the floor, matching the other props. */
typedef struct {
    int32_t x, y, z;
    int32_t rot_y;
    int32_t scale;       /* 4096 = full size, 2048 = half, etc. (ONE = 1.0) */
    int32_t active;
} SavePoint;

extern SavePoint save_points[MAX_SAVE_POINTS];
extern int       save_point_count;

void save_points_init(void);   /* load geometry once at startup (CD read only) */
void save_points_clear(void);  /* remove all instances (call in an area's init) */
void save_points_update(void); /* slow indefinite Y-axis spin */
/* scale is fixed-point with 4096 = 1.0 (use 4096 for full size, 2048 for half). */
int  save_point_add(int32_t x, int32_t y, int32_t z, int32_t rot_y, int32_t scale);
void save_points_draw(RenderContext *ctx);
/* Player collision against the save-point meshes (call from apply_collision_*). */
void save_points_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

#endif
