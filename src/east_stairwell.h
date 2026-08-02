#ifndef EAST_STAIRWELL_H
#define EAST_STAIRWELL_H

#include "render.h"

/* East Stairwell: a long east-west service passage (x[-1750,1750], z[-349,349],
   flat floor at y=0, drawn ceiling at y=-520) with the stairwell shaft running
   up through the middle of it. The shaft is caged behind two chain-link fences
   at x=-44 and x=+44, and those fences are SOLID — so the room is really two
   separate landings that only see each other through the wire:

     WEST landing  x[-1750,-44]  — its north-wall door (x=-1380, z=349) leads
                   back to the single wooden door in the East Hall's south wall
                   (east hall x=220, z=-992).
     EAST landing  x[ 44, 1750]  — its north-wall door (x=503, z=349) leads back
                   to the single wooden door in the Library's south wall
                   (library x=-1400, z=-2080). Past its east wall (x=1750) is a
                   modelled stair alcove — three treads climbing east to
                   x=2106, then the "upstairs" image on the alcove's back wall
                   rising to y=-663. All of it is OUTSIDE the collision bounds
                   (max_x 1750), so it is scenery seen through the opening; the
                   climb itself is the stair-climb transition (STATE_STAIR_ANIM)
                   up to the Attic Stairwell, triggered at that wall.

   Both doors sit in the z=349 wall and are approached from -Z, so both signs
   lie in the XY plane with mirror=0 (same orientation as the master bedroom's).
   Modelled on library.c / master_bedroom.c. */
void east_stairwell_load_assets(void);     /* startup: geometry + texture registration */
void east_stairwell_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
/* Narrow upload of this module's chnlnk copy alone, for the Attic Exit's cage
   (see the comment on the definition). */
void east_stairwell_upload_chnlnk(void);
void east_stairwell_init(void);            /* set collision/floor zones + spawn */
void east_stairwell_draw(RenderContext *ctx);

/* The two north-wall doors, one per landing. */
void east_stairwell_doors_arm(void);         /* seed both doors' Circle edge state */
int  east_stairwell_wdoor_triggered(void);   /* west landing -> East Hall */
int  east_stairwell_edoor_triggered(void);   /* east landing -> Library   */

/* The east landing's stairs up to the Attic Stairwell. Its Circle edge state is
   seeded by east_stairwell_doors_arm() along with the two doors'. */
int  east_stairwell_stairs_triggered(void);

/* Spawn the player just inside one of the doors on arrival. */
void east_stairwell_spawn_west(void);
void east_stairwell_spawn_east(void);
void east_stairwell_spawn_stairs(void);      /* back down from the attic */

#endif
