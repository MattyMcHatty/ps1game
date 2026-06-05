#ifndef COLLISION_H
#define COLLISION_H

#include <stdint.h>

/* Comment out this line for release builds */
#define DEBUG_COLLISION 1

extern int debug_mode;  /* toggled by Select; always available */

#ifdef DEBUG_COLLISION
#include "render.h"
#endif

/* -----------------------------------------------------------------------
 * Wall collision
 * ----------------------------------------------------------------------- */

#define MAX_WALLS_PER_ROOM 32

typedef struct {
    int32_t x1, z1;
    int32_t x2, z2;
    int32_t nx, nz;  /* inward-facing normal, fixed-point: 4096 = 1.0 */
#ifdef DEBUG_COLLISION
    int32_t y_floor, y_ceil;  /* debug visualisation extents only */
#endif
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
void apply_collision(void);
void apply_vampire_collision(void);

/* -----------------------------------------------------------------------
 * Floor / height zones
 * ----------------------------------------------------------------------- */

typedef enum {
    FLOOR_FLAT,    /* flat floor at a fixed Y */
    FLOOR_RAMP,    /* sloped — Y interpolated along one axis */
    FLOOR_UPPER,   /* elevated flat floor */
} FloorType;

typedef struct {
    FloorType type;

    /* XZ bounding box — player must be inside to match this zone */
    int32_t min_x, max_x;
    int32_t min_z, max_z;

    /* Floor surface Y for FLOOR_FLAT / FLOOR_UPPER */
    int32_t y;

    /* Ramp parameters (FLOOR_RAMP only) */
    int32_t ramp_y_start;    /* floor Y at the low end of the ramp  */
    int32_t ramp_y_end;      /* floor Y at the high end of the ramp */
    int32_t ramp_axis_start; /* coordinate (X or Z) at low end      */
    int32_t ramp_axis_end;   /* coordinate (X or Z) at high end     */
    int     ramp_along_x;    /* 1 = ramp axis is X, 0 = ramp axis is Z */
} FloorZone;

#define MAX_FLOOR_ZONES 16

/* Ground floor Y in world space — cam_y=0 corresponds to this level */
#define GROUND_FLOOR_Y 149
#define GRAVITY        1
#define MAX_FALL_VEL   20

extern FloorZone floor_zones[MAX_FLOOR_ZONES];
extern int       floor_zone_count;
extern int       player_on_upper_floor; /* 1 when player is on the upper floor */
extern int       player_on_ramp;        /* 1 when ramp zone is active this frame */
extern int       vampire_on_upper_floor;
extern int       vampire_on_ramp;

void floor_zones_init(void);
void apply_height(void);
void apply_vampire_height(void);

#ifdef DEBUG_COLLISION
void debug_draw_walls(RenderContext *ctx);
void debug_draw_coords(RenderContext *ctx);
#endif

#endif
