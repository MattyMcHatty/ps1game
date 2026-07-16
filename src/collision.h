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

#define MAX_WALLS_PER_ROOM 128  /* large enough for the detailed reception collision mesh */

typedef struct {
    int32_t x1, z1;
    int32_t x2, z2;
    int32_t nx, nz;  /* inward-facing normal, fixed-point: 4096 = 1.0 */
    /* Vertical extent of the wall face in world Y (-Y is up, so y_min is the
       wall's TOP and y_max its bottom/floor). Set by the SMX collision
       generator; enables per-face Y-aware collision so a wall only blocks on
       its own floor. y_min == y_max means "no Y data" -> treated as full height. */
    int32_t y_min, y_max;
} Wall;

typedef struct {
    Wall    walls[MAX_WALLS_PER_ROOM];
    int     wall_count;
    int32_t min_x, max_x;
    int32_t min_z, max_z;
    /* 1 for rooms with real per-wall Y data spanning more than one floor
       (delivery, reception): the hitscan Y-gates walls so a shot isn't blocked
       by geometry on a different level. 0 for flat rooms (kitchen), whose per-
       wall Y values are debug-visualisation only and must not gate shots. */
    int     multi_level;
    /* Bitmask of low walls (indices 0-31) the gun shoots OVER: they still block
       the player, but a hitscan ignores them so you can fire across a low
       counter and hit enemies on the far side. */
    uint32_t shoot_over_mask;
} CollisionRoom;

extern CollisionRoom current_collision_room;

int  collide_wall(Wall *w, int32_t *px, int32_t *pz, int32_t radius);
void collision_init(void);
void apply_collision(void);
void apply_collision_kitchen_dining(void);
void apply_collision_reception(void);
void apply_vampire_collision(void);
void apply_ddog_collision(int32_t *x, int32_t *z, int on_upper_floor, int on_ramp);

/* Wall collision for a free-roaming entity in a single-level (flat) room such
   as the kitchen: collides against every wall in the current room, front faces
   only — the same scheme apply_collision_kitchen_dining() uses for the player. */
void apply_flat_entity_collision(int32_t *x, int32_t *z, int32_t radius);

/* Hitscan line-of-sight test: returns 1 if the straight segment from
   (ax,ay,az) to (bx,by,bz) is blocked by a wall or solid prop — i.e. a wall
   polygon lies between the two points. Uses an exact ray/segment crossing test
   for walls (no point-sampling, so thin walls can never be skipped at any
   distance) and marches the segment for volumetric props. Y coordinates are in
   camera-offset space, matching cam_y / enemy coordinates. */
int  collision_segment_blocked(int32_t ax, int32_t ay, int32_t az,
                               int32_t bx, int32_t by, int32_t bz);

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
void apply_ddog_height(int32_t *px, int32_t *py, int32_t *pz, int32_t *vy, int *on_upper_floor, int *on_ramp);

#ifdef DEBUG_COLLISION
void debug_draw_walls(RenderContext *ctx);
void debug_draw_coords(RenderContext *ctx);
#endif

#endif
