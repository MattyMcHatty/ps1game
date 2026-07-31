#ifndef LIBRARY_H
#define LIBRARY_H

#include "render.h"

/* Library, entered through the double door at the EAST end of the East Hall
   (east hall x=2672, z=372). L-shaped: a small concrete entrance vestibule
   x[-350,350] z[-349,349] whose west wall holds the double door, opening south
   through the z=-349 gap into the reading room proper, x[-1780,350]
   z[-2080,-349] (purple wallpaper, wood floor, bookcases along the west side).
   Flat floor at y=0 throughout. Modelled on east_hall.c.

   Two doors are modelled into the mesh, both wired up: the vestibule's west
   double door back to the East Hall, and the single wooden door in the reading
   room's south wall (z=-2080, x=-1400) onto the East Stairwell's east landing. */
void library_load_assets(void);     /* startup: geometry + texture headers */
void library_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void library_init(void);            /* set collision/floor zones + spawn */
void library_draw(RenderContext *ctx);

/* The west double door back to the East Hall, and the south single door onto
   the East Stairwell. */
void library_doors_arm(void);        /* seed both doors' Circle edge state */
int  library_wdoor_triggered(void);  /* west double door (east hall x=2672) */
int  library_sdoor_triggered(void);  /* south single door (stairwell x=503) */

/* Spawn the player just inside one of the doors on arrival. */
void library_spawn_west(void);
void library_spawn_south(void);

#endif
