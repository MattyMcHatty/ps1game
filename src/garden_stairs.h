#ifndef GARDEN_STAIRS_H
#define GARDEN_STAIRS_H

#include <stdint.h>
#include "render.h"

/* Garden Stairs: the chainlink-caged stairway behind the Attic Exit's unlocked
   exit door. Modelled in the SAME world offset as the Attic Exit — that room's
   north wall is z=1000, this one's south wall is z=1117 — so the two meshes'
   coordinates are directly comparable and no offset is applied.

   Bounds x[-2139,712] z[1116,2317], and it is TALL: y[-2100,0]. The floor plan
   is three columns wrapped around a shaft:

     WEST landings  x[-2140,-1242], flat at y=0, -600 and -1200
     FLIGHTS        x[-1242,-267], five sloped runs. The SOUTH half
                    (z[1117,1717]) climbs eastward, the NORTH half
                    (z[1717,2317]) climbs westward — a switchback.
     EAST landings  x[-267,713], flat at y=-300, -900 and -1500

   so the route from the bottom is: west y=0 -> south flight -> east y=-300 ->
   north flight -> west y=-600 -> ... -> east y=-1500, the top landing.

     SOUTH   the xt_dr_outr exit door at x[-104,386], y[-2100,-1500], z=1117.
     WALL    The OUTER face of the Attic Exit's exit door, and the only way in
             or out. The player arrives here, at the TOP of the stairs.
     WEST    the xt_dr_cg door at x=-2140, y[-573,0], z[1717,2167], on the
     WALL    bottom landing, out into the Garden Courtyard.

   Rendered the same way as the Attic Exit (per-poly tex map + 128 texture
   window + fog). */
void garden_stairs_load_assets(void);     /* startup: geometry + texture registration */
void garden_stairs_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void garden_stairs_init(void);            /* set collision/floor zones + spawn */
void garden_stairs_draw(RenderContext *ctx);

/* The south-wall door back to the Attic Exit, at the TOP of the stairs. */
void garden_stairs_door_arm(void);        /* seed the Circle edge state */
int  garden_stairs_door_triggered(void);  /* 1 on a fresh Circle press in range */

/* The west-wall cage door onto the Garden Courtyard, at the BOTTOM. */
void garden_stairs_cg_door_arm(void);
int  garden_stairs_cg_door_triggered(void);

/* Spawn just inside one of the two doors; each arms every interaction here. */
void garden_stairs_spawn_top(void);
void garden_stairs_spawn_bottom(void);

#endif
