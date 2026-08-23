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
void east_hall_load_assets(void);     /* startup: register streamed textures */
void east_hall_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void east_hall_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void east_hall_init(void);            /* set collision/floor zones + spawn */
void east_hall_draw(RenderContext *ctx);

/* ---- The hall after the keystones come out ---------------------------------
   FLAG_HADAD_TWO retires this room. Two things follow from it, and they are
   deliberately in two places:

     THE MONSTERS DIE — east_hall_apply_flags() below. Every zombie, spider and
     Rabisu still alive in here is marked dead, silently and permanently, on
     every entry from that point on. Nothing new spawns in the East Hall again.

     THE EAST DOOR IS BURIED — but not by this flag and not on entry. That is the
     collapse's doing and it has its own flag; see east_hall_quake.h.

   Called from main.c's post-entry re-derive block, after world_enter and after
   the saved flags are installed — the same slot as attic_exit_apply_flags, for
   the same reason. See the full note on the definition in east_hall.c. */
void east_hall_apply_flags(void);

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
