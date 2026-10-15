#ifndef LAMASHTU_ARENA_H
#define LAMASHTU_ARENA_H

#include <stdint.h>
#include "render.h"

/* The Lamashtu Arena: Chapter 3's THIRTY-FOURTH room, through The Head's east
   door - the one the arm switches unlock (src/arm_puzzle.h). It is where the
   fight with Lamashtu will be. SHE STANDS IN IT NOW (src/lamashtu.h), five
   600-unit squares down the door aisle at (2700, 1500), playing her two clips
   on a loop; the fight itself is not built.

   THE SHAPE. One great hall, x[0,7800] z[-1200,4200], flat at y=0, its stone
   rising to y=-800. TWENTY-FOUR square pillars, 600 across and floor to
   ceiling, in a 6 x 4 grid: columns at x[600,1200] [1800,2400] [3000,3600]
   [4200,4800] [5400,6000] [6600,7200], rows at z[-600,0] [600,1200]
   [1800,2400] [3000,3600]. 600-wide aisles run between them both ways.

   THE DOOR. One, and it is wired:

     WEST  x=0  z[1400,1600]  y[-400,0]  -> The Head, its east door

   In the YZ plane at fixed X, approached from +X (wall 0 runs x=0 with
   nx=+4095), so TEXT_PLANE_YZ with mirror=0 and the sign 11 proud of the wall
   along +X - The Head's own west door's terms. The door sits on the aisle
   between the z[600,1200] and z[1800,2400] pillar rows.

   Nothing is seeded in it by world.c. The room places ONE prop itself: a
   gate (src/bars.h) standing in the gap on her -Z side, at (2700, 900).

   THREE TEXTURES, AND THE ROOM OWNS NONE OF THEM: cobblestone, the loculus and
   the inner door, the Tomb's set exactly, through src/catacombs_entry.c's three
   narrow uploaders.

   Its exports live in assets/bosses/Lamashtu/, not assets/catacombs/. */

void lamashtu_arena_load_assets(void);     /* startup: three headers, no registration */
void lamashtu_arena_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void lamashtu_arena_upload_textures(void); /* room entry: pure LoadImage from RAM      */
void lamashtu_arena_init(void);            /* collision + floor zone + spawn           */
void lamashtu_arena_draw(RenderContext *ctx);

/* Arrival through the west door, from The Head - the only arrival, and
   lamashtu_arena_init()'s spawn: just inside it, facing east down the hall. */
void lamashtu_arena_spawn_west(void);

/* One frame of the west door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range and facing the door. */
int  lamashtu_arena_west_door_triggered(int lock);

/* Arm every interaction in the room (the door). Called by the spawn above. */
void lamashtu_arena_arm(void);

#endif
