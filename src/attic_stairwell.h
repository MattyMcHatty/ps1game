#ifndef ATTIC_STAIRWELL_H
#define ATTIC_STAIRWELL_H

#include <stdint.h>
#include "render.h"

/* Attic Stairwell: the floor above the East Stairwell, reached by the stairs at
   the top of that room's east landing. A flat y=0 attic with a drawn ceiling at
   y=-520, laid out as three connected spaces:

     CHAMBER   x[-350,350] z[-350,350] — the stair head. Its west wall (x=-350)
               backs onto the modelled stair alcove (three treads dropping west
               to x=-831 and the "upstairs" image on the alcove's back wall).
               That alcove is scenery OUTSIDE the walkable floor, exactly like
               the East Stairwell's; the descent itself is the stair-climb
               transition, not real geometry. A doorway in the chamber's south
               wall (x[-100,200], z=-350..-397) leads on.
     MAIN HALL x[-1608,351] z[-1399,-397], with a west corridor running north up
               to z=349 at x[-1608,-1009]. The "trck_clue" picture hangs on its
               far south wall (z=-1400, x[-403,200]).
     WEST ROOM x[-2611,-1655] z[-1400,349], through the doorway in the partition
               at x[-1655,-1608], z[-1200,-900]. It holds a con_tile block at
               x[-2420,-2229] z[-543,-7], and its north wall carries a wd_dr
               door at x[-2037,-1846] z=350 (centre x=-1942) through to the
               Attic Exit.

   Two interactions: the stairs back down to the East Stairwell's east landing
   at the chamber's west wall, and that west-room door. Rendered the same way as
   the East Stairwell (per-poly tex map + 128 texture window + fog). */
void attic_stairwell_load_assets(void);     /* startup: register streamed textures */
void attic_stairwell_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void attic_stairwell_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void attic_stairwell_init(void);            /* set collision/floor zones + spawn */
void attic_stairwell_draw(RenderContext *ctx);

/* The stairs down to the East Stairwell (same stair-climb transition the
   conservatory and the 2F hall use). */
void attic_stairwell_stairs_arm(void);      /* seed the Circle edge state */
int  attic_stairwell_stairs_triggered(void);/* 1 on a fresh Circle press in range */

/* The west room's north-wall door through to the Attic Exit. */
void attic_stairwell_exit_door_arm(void);       /* seed the Circle edge state */
int  attic_stairwell_exit_door_triggered(void); /* 1 on a fresh Circle press in range */

/* Spawn at the stair head on arrival from below, or just inside the west room's
   north door on arrival from the Attic Exit. */
void attic_stairwell_spawn_stairs(void);
void attic_stairwell_spawn_exit_door(void);

/* The altar (the con_tile block in the west room). It is drawn as part of the
   room mesh but collided as a PROP, so the player can walk right up to it and
   reach the pickups on its top face — the 195 wall standoff put them outside
   ITEM_PICKUP_RADIUS. Both are gated to this room, so the shared collision
   routine calls them unconditionally. */
void attic_stairwell_altar_collide(int32_t *px, int32_t py, int32_t *pz,
                                   int32_t radius);
int  attic_stairwell_altar_point_solid(int32_t x, int32_t y, int32_t z,
                                       int32_t slack);

#endif
