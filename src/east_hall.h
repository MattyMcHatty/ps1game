#ifndef EAST_HALL_H
#define EAST_HALL_H

#include "render.h"

/* East Hall, entered through the double door on the EAST wall of reception's
   upper floor (reception x=1500, z=1071). A long east-west hall (x[19,2671],
   z[21,721]) with a wing running south down its west end. Flat floor at y=0
   throughout. Modelled on master_bedroom.c / conservatory.c.

   Three doors are modelled into the mesh, all wired up: the west double door
   goes back to reception, the east double door (x=2672, z=372) into the Library,
   and the south single door (z=-992, x=220), at the bottom of the south
   offshoot, onto the East Stairwell's west landing. */
void east_hall_load_assets(void);     /* startup: geometry + texture registration */
void east_hall_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void east_hall_init(void);            /* set collision/floor zones + spawn */
void east_hall_draw(RenderContext *ctx);

/* The three doors: west back to reception's upper floor, east to the Library,
   south to the East Stairwell. */
void east_hall_doors_arm(void);        /* seed all three doors' Circle edge state */
int  east_hall_wdoor_triggered(void);  /* west double door (reception z=1071) */
int  east_hall_edoor_triggered(void);  /* east double door (library x=-350) */
int  east_hall_sdoor_triggered(void);  /* south single door (stairwell x=-1380) */

/* Spawn the player just inside one of the doors on arrival. */
void east_hall_spawn_west(void);
void east_hall_spawn_east(void);
void east_hall_spawn_south(void);

#endif
