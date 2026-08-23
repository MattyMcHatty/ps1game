#ifndef LIBRARY_DESTROYED_H
#define LIBRARY_DESTROYED_H

#include "render.h"

/* Library Destroyed — the Library after the Attic Exit's exit door gives up its
   four keystones. It does not sit anywhere new on the map: from the moment
   FLAG_HADAD_TWO is set it TAKES THE PLACE of STATE_LIBRARY, and the two doors
   that used to lead into the Library lead here instead (east_hall_edoor and
   east_stairwell_edoor pick their destination out of the flag — see
   library_destroyed_active() below).

   Same shell as the Library, same coordinate space, same two doors in the same
   two places: the vestibule's west double door back to the East Hall (x=-350,
   z=0) and the single wooden door in the south wall onto the East Stairwell's
   east landing (z=-2080, x=-1400). What changed is the inside — the reading
   room's shelving has come down and fills x[-1303,-350] z[-2080,-759] as one
   solid block, so the walkable floor is a U: the vestibule, south down the east
   aisle, west along the z=-550 corridor, then south down the west aisle to the
   stairwell door.

   It is EMPTY. None of the Library's spiders, zombies or pickups are seeded
   here (world.c seeds this room with nothing), which is the whole point of it
   being a separate room rather than a mesh swap: the Library keeps its own
   record of what the player killed and took, and this room keeps its own. */
void library_destroyed_load_assets(void);     /* startup: texture slots (all owned elsewhere) */
void library_destroyed_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void library_destroyed_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void library_destroyed_init(void);            /* set collision/floor zones + spawn */
void library_destroyed_draw(RenderContext *ctx);

/* Which of the two rooms the Library's doors currently open onto. Returns 1
   once FLAG_HADAD_TWO is set. Both neighbours and the save menu read this, so
   the rule lives in exactly one place. */
int  library_destroyed_active(void);

/* The west double door back to the East Hall, and the south single door onto
   the East Stairwell — the same two the Library has. */
void library_destroyed_doors_arm(void);        /* seed both doors' Circle edge state */
int  library_destroyed_wdoor_triggered(void);  /* west double door (east hall x=2672) */
int  library_destroyed_sdoor_triggered(void);  /* south single door (stairwell x=503) */

/* Spawn the player just inside one of the doors on arrival. */
void library_destroyed_spawn_west(void);
void library_destroyed_spawn_south(void);

#endif
