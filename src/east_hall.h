#ifndef EAST_HALL_H
#define EAST_HALL_H

#include "render.h"

/* East Hall, entered through the double door on the EAST wall of reception's
   upper floor (reception x=1500, z=1071). A long east-west hall (x[19,2671],
   z[21,721]) with a wing running south down its west end. Flat floor at y=0
   throughout. Modelled on master_bedroom.c / conservatory.c.

   Three doors are modelled into the mesh. The west double door goes back to
   reception and the east double door (x=2672, z=372) into the Library; the
   south single door (z=-992, x=220) is the smashable fatdoor. */
void east_hall_load_assets(void);     /* startup: geometry + texture registration */
void east_hall_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void east_hall_init(void);            /* set collision/floor zones + spawn */
void east_hall_draw(RenderContext *ctx);

/* The two double doors: west back to reception's upper floor, east to the
   Library. */
void east_hall_doors_arm(void);        /* seed both doors' Circle edge state */
int  east_hall_wdoor_triggered(void);  /* west double door (reception z=1071) */
int  east_hall_edoor_triggered(void);  /* east double door (library x=-350) */

/* Spawn the player just inside one of the doors on arrival. */
void east_hall_spawn_west(void);
void east_hall_spawn_east(void);

#endif
