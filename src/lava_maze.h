#ifndef LAVA_MAZE_H
#define LAVA_MAZE_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   LAVA MAZE - Chapter 4's ninth room
   =========================================================================
   Between Ribcage's NORTH door and A Woman Scorned's SOUTH-EAST door.

   THE SHAPE. One storey at y=0, x[0,7800] z[-2400,4200], inside a 600-tall
   rim (the collision proxy's only walls). The floor is LAVA, and the maze is
   a cobblestone path through it on the same plane: there are no walls to
   find the way by, only the burn (its ROOMS line in tools/gen_lava_floors.py,
   whose map is the maze). It hangs in the chapter's sky
   (src/underworld_sky.h), centred on it. One flat floor zone. No props.

   ONE ENEMY: an ALIVE COLUMN (src/column.h) in the rectangular lava pool all
   the paths run round, x[4000,5800] z[600,2000] - seeded in world.c's
   world_seed_room, facing north/south and stepping east/west. Plus eight
   more Columns round the maze (two alive, six static walls) - the list is
   in world_seed_room.

   THREE DOORS:
     - WEST: a gaol door, distorted - it leans as it rises. It leads back to
       RIBCAGE's north door, a catacomb inner door. So, per direction (the
       user's choice, Nov 2026): out of here DOOR_PANEL_GAOL_UW (the gaol
       door's own transition, on this room's copy of the art); out of Ribcage
       DOOR_PANEL_CATACOMB.
     - NORTH: a catacomb inner door. It leads to A WOMAN SCORNED's south-east
       door, an inner double door. Out of here DOOR_PANEL_CATACOMB; out of A
       Woman Scorned DOOR_PANEL_INNER.
     - SOUTH-EAST: a wooden door (wd_dr, resident) bent round the south-east
       corner. It leads to HEAVENS GATE's north-west door, a greenhouse
       door. Out of here DOOR_PANEL_WOOD; out of Heavens Gate
       DOOR_PANEL_GREENHOUSE (the user's choice, per direction).

   THE ARRIVALS. Three: through the west door from Ribcage (lava_maze_spawn_w,
   also the init's default and where a debug jump lands), through the north
   door from A Woman Scorned (lava_maze_spawn_n), and through the south-east
   door from Heavens Gate (lava_maze_spawn_se) - main.c's overrides.

   THE MUSIC is the chapter's (CDAUDIO_UNDERWORLD_TRACK, main.c's Chapter 4
   block).

   Textures: five, one owned (the gaol door's copy) - see lava_maze.c.
   Chapter rules (the fog, the sky): src/underworld.h. */

void lava_maze_load_assets(void);     /* startup: one registration + TIM_SLOTs */
void lava_maze_load_geometry(void);   /* ROOM ENTRY: mesh + the sky            */
void lava_maze_upload_textures(void); /* room entry: pure LoadImage            */
void lava_maze_init(void);            /* collision, floor, spawn               */
void lava_maze_draw(RenderContext *ctx);

/* Seed every door's Circle edge state "held", so a press carried in through
   the transition cannot fire on the arrival frame. */
void lava_maze_doors_arm(void);

/* The WEST door, to Ribcage, and the NORTH door, to A Woman Scorned: a fresh
   Circle within reach and facing it. The edge state is kept current while
   locked. */
int  lava_maze_w_door_triggered(int lock);
int  lava_maze_n_door_triggered(int lock);
int  lava_maze_se_door_triggered(int lock);   /* ...and the SE door, to Heavens Gate */

/* Arrival through the west door (just inside it, facing east) and the north
   door (just inside it, facing south). */
void lava_maze_spawn_w(void);
void lava_maze_spawn_n(void);
void lava_maze_spawn_se(void);   /* just inside the SE door, facing north-west */

#endif
