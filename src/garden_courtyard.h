#ifndef GARDEN_COURTYARD_H
#define GARDEN_COURTYARD_H

#include <stdint.h>
#include "render.h"

/* Garden Courtyard: the walled garden at the foot of the Garden Stairs.

   Modelled in its OWN coordinate space, not the Attic Exit / Garden Stairs one:
   its floors sit at POSITIVE y (800..900) where the stairs' bottom landing is
   y=0, and its door is on the east wall where the stairs' is on the west. Only
   the door pairing links the two, so no offset is applied anywhere.

   Bounds x[-2579,2000] z[-2000,2000], drawn up to y=-800 — a 1700-tall brick
   and chainlink perimeter around an open garden. Three terrace levels, each a
   short step below the last (-Y is up, so a LARGER y is LOWER):

     PERIMETER  y=800, the raised walk: the south strip z[-2000,-1142] full
                width, plus the west strip x[-2579,-1722] and the east strip
                x[1142,2000], both running z[-1142,2000].
     STEP BAND  y=850, x[-1722,1142] z[-1142,-857] — the 50-unit tread between
                the south walk and the lawn, cut diagonally at its NW corner.
     LAWN       y=900, x[-1722,1142] z[-857,2000] — the sunken middle.

   The three retaining walls around the lawn (collision walls 0, 4, 5 and 6) are
   only 100 tall, so they block from BELOW but are stepped off from above, which
   is what a knee-high garden wall should do.

     EAST   the xt_dr_cg door at x=2000, y[114,800], z[-1429,-857]. The far
     WALL   side of the identical door on the Garden Stairs' bottom landing.

     NORTH  the grdn_gte leaf at z=2000, x[-1437,-580], y[300,900], set into
     WALL   the hedge that replaced most of the chainlink run along this side.
            It opens onto Fountain Square, and it is the one door in the game
            whose transition is DOOR_PANEL_GATE — a single leaf swinging over
            the length of SFX_GATE rather than the usual door slam.

   Both are sealed while the Rabisu encounter has the arena shut
   (rabisu_boss_seals_door): the prompt is withheld and the trigger refuses.

   Rendered the same way as the Garden Stairs (per-poly tex map + one 128
   texture window + purple outdoor fog), and it reuses that room's texture
   uploads wholesale — the two share all five of this mesh's textures. */
void garden_courtyard_load_assets(void);     /* startup: register streamed textures */
void garden_courtyard_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void garden_courtyard_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void garden_courtyard_init(void);            /* set collision/floor zones + spawn */
void garden_courtyard_draw(RenderContext *ctx);

/* The east-wall door back to the Garden Stairs. */
void garden_courtyard_door_arm(void);        /* seed the Circle edge state */
int  garden_courtyard_door_triggered(void);  /* 1 on a fresh Circle press in range */

/* The north-wall gate through the hedge, into Fountain Square. */
void garden_courtyard_ngate_arm(void);
int  garden_courtyard_ngate_triggered(void);

/* Spawn just inside one of the two doors; each arms every interaction here. */
void garden_courtyard_spawn_east(void);
void garden_courtyard_spawn_north(void);

#endif
