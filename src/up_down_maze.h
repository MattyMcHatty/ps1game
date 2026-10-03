#ifndef UP_DOWN_MAZE_H
#define UP_DOWN_MAZE_H

#include <stdint.h>
#include "render.h"

/* Up Down Maze: Chapter 3's second room, through the inner door at the east end
   of the Catacombs Entry's burial hall.

   >>> IT IS A MAZE IN TWO STOREYS OVER ONE FOOTPRINT, which is what the name
   means and what everything below follows from. <<< The room is a single box,
   x[-300,3900] y[-1800,0] z[-2100,3900], containing ten solid stone BLOCKS. The
   gaps between the blocks are the LOWER maze, walked at y=0. The TOPS of the
   blocks are the UPPER maze, walked at y=-1000 (-Y is up, so that is 1000 above
   the lower floor). The same wall face is therefore a corridor wall down below
   and a sheer drop up top, and the two mazes have completely different shapes.

   THERE IS NO STAIR, NO RAMP AND NO LADDER. Going DOWN is walking off an edge —
   apply_height() has no zone under the player and gravity does the rest. Going
   back UP is not modelled, because nothing in the mesh models it: the two levels
   are joined only by their doors. Anyone re-exporting this room with a ramp in
   it wants a FLOOR_RAMP zone in up_down_maze_floor_zones_init(), and that is the
   whole change.

   THE DOORS. Six are drawn into the outer walls, and ALL SIX are wired up:

     WEST, UPPER   x=-300  z[-100,100]  y[-1400,-1000]   -> Catacombs Entry
     SOUTH, LOWER  z=-2100 x[500,700]   y[-400,0]        -> Incinerator Room
     NORTH, UPPER  z=3900  x[1100,1300] y[-1400,-1000]   -> Cleaver Corridor
                   (locked from the corridor side until FLAG_CLEAVER_CORR_DOOR)
     EAST, LOWER   x=3900  z[3500,3700] y[-400,0]        -> Crucifix Corridor
                   (the single door in the lower maze's north-east corner)
     SOUTH, UPPER  z=-2100 x[1700,1900] y[-1400,-1000]   -> The Shelf
                   (the south end of the south block's walkway; a cold
                   sconce with a Blood Pearl on it stands in the middle of
                   the walkway's west square, at (1200,-1800), on the
                   arriving player's left)
     EAST, UPPER   x=3900  z[-100,100]  y[-1400,-1000]   -> Gaol Entry
                   (the east end of the east spur's walkway)

   >>> AND WITH TWO DOORS ON TWO STOREYS, THE STOREY TEST IS NOT THE UPSTAIRS
   DOOR'S QUIRK ANY MORE. <<< Both triggers and both signs take it, because what
   makes it necessary is a walkway and a corridor sharing a footprint and not
   which of the two the door is on — the south-lower door has the south block's
   walkway ending 499 Manhattan away in plan and a whole storey up. See
   UDM_STOREY_REACH in the .c.

   TWO TEXTURES, AND THE ROOM OWNS NEITHER. Cobblestone and the catacomb inner
   door are both already registered by src/catacombs_entry.c in
   TEXBANK_CATACOMBS, so this room holds compile-time headers (TIM_SLOT) and
   calls that module's two NARROW uploaders. It costs zero texmgr registrations
   and zero permanent RAM — see up_down_maze_load_assets().

   Its exports live in assets/catacombs/, beside the Catacombs Entry's. */

void up_down_maze_load_assets(void);     /* startup: headers only, no CD, no regs */
void up_down_maze_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void up_down_maze_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void up_down_maze_init(void);            /* collision + floor zones + spawn */
void up_down_maze_draw(RenderContext *ctx);

/* The two arrivals. WEST is on the UPPER floor — standing on the landing just
   inside the door, facing east into the maze — and is still the default
   up_down_maze_init() places. SOUTH is on the LOWER floor, just inside the south
   door, facing north up the corridors, for the way back out of the Incinerator
   Room. */
void up_down_maze_spawn_west(void);
void up_down_maze_spawn_south(void);
/* NORTH is on the UPPER floor at the end of the north stub, facing south into
   the maze, for the way back from the Cleaver Corridor. */
void up_down_maze_spawn_north(void);
/* EAST is on the LOWER floor in the north-east corner, just inside the east
   door, facing west, for the way back from the Crucifix Corridor. */
void up_down_maze_spawn_east(void);
/* SOUTH-UPPER is on the UPPER floor at the south end of the south block's
   walkway, facing north along it, for the way back from The Shelf. */
void up_down_maze_spawn_south_upper(void);
/* EAST-UPPER is on the UPPER floor at the east end of the east spur, facing
   west along it, for the way back from the Gaol Entry. */
void up_down_maze_spawn_east_upper(void);

/* One frame of each door's Circle test. `lock` is main's usual suppression (a
   menu is up, a cutscene owns the camera). Returns 1 on a fresh press made in
   range, on the door's own storey and facing it — the frame main.c starts the
   transition on.

   CALL BOTH EVERY FRAME AND PASS `lock` IN rather than testing it outside: each
   keeps its own Circle edge state current while locked and returns 0, so a press
   held across a menu closing cannot read as a fresh one on the frame the lock
   lifts, and skipping one would leave its `prev` stale. */
int  up_down_maze_west_door_triggered(int lock);
int  up_down_maze_south_door_triggered(int lock);
int  up_down_maze_north_door_triggered(int lock);
int  up_down_maze_east_door_triggered(int lock);
int  up_down_maze_south_upper_door_triggered(int lock);
int  up_down_maze_east_upper_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way (a debug jump) can still ensure a
   Circle held through the transition does not fire on the arrival frame. */
void up_down_maze_arm(void);

#endif
