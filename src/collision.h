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
    /* Height of this room's DRAWN ceiling in mesh Y, or 0 for "derive it from
       the wall tops". A room's collision data comes from a simplified proxy
       mesh (*_mesh.smx), whose walls do not always reach as high as the ceiling
       in the mesh actually rendered (*.smx): the East Hall's proxy walls stop at
       -375 while its drawn ceiling is at -520. Wall tops are therefore only a
       LOWER bound on the ceiling, and any room whose two meshes disagree must
       state the real value here. Set via collision_set_ceiling_y() next to the
       room's *_collision_init() call — the generated collision code does not
       touch this field, so leaving it unset would inherit the previous room's. */
    int32_t  ceiling_y;
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

/* Ceiling height above a world XZ point. When the room states its drawn ceiling
   (CollisionRoom.ceiling_y) that value wins outright. Otherwise it is read out
   of the mesh-generated collision data: a wall's y_min is the TOP edge of that
   wall face, i.e. where the proxy mesh's walls stop. Returns the highest (most
   negative) wall top among the walls within CEILING_PROBE_R of the point, so a
   ceiling-dweller placed in a tall hall hangs from that hall's ceiling and not
   from a low connector's soffit next door. Falls back to the room's highest
   wall top, then to CEILING_DEFAULT_Y for a room with no usable Y data.

   The result is the ceiling SURFACE in mesh/world Y — the same space sprite
   vertices are drawn in, so a caller positions against it directly. Do NOT
   subtract GROUND_FLOOR_Y: that conversion turns a FloorZone's surface into an
   entity's standing anchor (see apply_ddog_height), which is a different job.
   Subtracting it here put a ceiling-hanging spider a whole 149 units up through
   the roof. */
#define CEILING_PROBE_R   1200   /* how far around the point to look for walls */
#define CEILING_DEFAULT_Y (-375)  /* typical room height above a y=0 floor */
int32_t collision_ceiling_y(int32_t x, int32_t z);

/* State the current room's drawn ceiling height (0 = derive it from the wall
   tops). Call right after the room's *_collision_init(). */
void    collision_set_ceiling_y(int32_t y);

/* ---- Player wall standoff --------------------------------------------------
   How far apply_collision_reception() holds the player off a wall. It is a
   standoff, not a body radius: the player is stopped well back so wall polys
   never reach the GTE's near-plane clamp and clip. 195 suits the ordinary
   interior rooms; a room with tall wall faces the player can stand right under
   needs more, so it is settable per room.

   Call collision_set_wall_radius() from the room's *_init(), next to
   collision_set_ceiling_y(). main.c resets it to the default before every room
   init, so a room that says nothing gets 195 rather than inheriting whatever
   the last room asked for. Pass 0 for the default. */
#define COLLISION_WALL_RADIUS 195
void    collision_set_wall_radius(int32_t r);

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
