#ifndef MASTER_BEDROOM_H
#define MASTER_BEDROOM_H

#include "render.h"

/* Master Bedroom, entered through either of the two doors in the 2F hall
   corridor's south wall (z=-656, at x=-400 and x=-1889). An H-shaped room: a
   central bed chamber (x[-500,500], z[-379,319]) joined by short passages to a
   west and an east wing, each with its own door back out to the corridor.
   Flat floor at y=0 throughout. Modelled on conservatory.c / hall_2f.c. */
void master_bedroom_load_assets(void);     /* startup: register streamed textures */
void master_bedroom_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void master_bedroom_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void master_bedroom_init(void);            /* set collision/floor zones + spawn */
void master_bedroom_draw(RenderContext *ctx);

/* The two doors back to the 2F hall. "west"/"east" name the wing they sit in;
   each maps to the corridor door directly above it in the hall. */
void master_bedroom_doors_arm(void);       /* seed both doors' Circle edge state */
int  master_bedroom_wdoor_triggered(void); /* west wing door  (hall x=-1889) */
int  master_bedroom_edoor_triggered(void); /* east wing door  (hall x=-400)  */

/* Spawn the player just inside a door after arriving from the hall. */
void master_bedroom_spawn_west(void);
void master_bedroom_spawn_east(void);

#endif
