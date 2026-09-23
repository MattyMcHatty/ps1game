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

   THE DOORS. Six are drawn into the outer walls, and exactly ONE of them is
   wired up:

     WEST, UPPER   x=-300  z[-100,100]  y[-1400,-1000]   -> Catacombs Entry
     east, upper   x=3900  z[-100,100]                   not built
     north, upper  z=3900  x[1100,1300]                  not built
     south, upper  z=-2100 x[1700,1900]                  not built
     south, lower  z=-2100 x[500,700]   y[-400,0]        not built
     east, lower   x=3900  z[3500,3700] y[-400,0]        not built

   The five unbuilt ones are drawn and nothing else: no sign, no trigger, no
   collision gap. They read as sealed doors, which is what they are until the
   rooms behind them exist, and wiring one up is the block of #defines below
   plus the STEP 6 edits in tools/ADDING_A_ROOM.txt.

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

/* Arrival through the west door on the UPPER floor, and the only arrival there
   is: standing on the landing just inside it, facing east into the maze. */
void up_down_maze_spawn_west(void);

/* One frame of the west door's Circle test. `lock` is main's usual suppression
   (a menu is up, a cutscene owns the camera). Returns 1 on a fresh press made in
   range and facing the door — the frame main.c starts the transition on. */
int  up_down_maze_west_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way (a debug jump) can still ensure a
   Circle held through the transition does not fire on the arrival frame. */
void up_down_maze_arm(void);

#endif
