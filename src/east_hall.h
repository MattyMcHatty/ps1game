#ifndef EAST_HALL_H
#define EAST_HALL_H

#include "render.h"

/* East Hall, entered through the double door on the EAST wall of reception's
   upper floor (reception x=1500, z=1071). A long east-west hall (x[19,2671],
   z[21,721]) with a wing running south down its west end. Flat floor at y=0
   throughout. Modelled on master_bedroom.c / conservatory.c.

   Three doors are modelled into the mesh; only the west double door is wired
   up so far. The east double door (x=2672, z=372) and the south single door
   (z=-992, x=220) are geometry only until the rooms behind them exist. */
void east_hall_load_assets(void);     /* startup: geometry + texture registration */
void east_hall_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void east_hall_init(void);            /* set collision/floor zones + spawn */
void east_hall_draw(RenderContext *ctx);

/* The west double door back to reception's upper floor. */
void east_hall_doors_arm(void);        /* seed the door's Circle edge state */
int  east_hall_wdoor_triggered(void);  /* west double door (reception z=1071) */

/* Spawn the player just inside the west door after arriving from reception. */
void east_hall_spawn_west(void);

#endif
