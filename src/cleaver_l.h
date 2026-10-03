#ifndef CLEAVER_L_H
#define CLEAVER_L_H

#include <stdint.h>
#include "render.h"

/* Cleaver L: Chapter 3's FIFTEENTH room, through the door at the back of the
   Meat Plant's south alcove (z=0, x[-100,100]).

   THE SHAPE. An L of two 600-wide shafts, flat and single-storey, walled to
   y=-800 and open above that into the dark, as the Cleaver Corridor is:

       NORTH-SOUTH shaft  x[0,600]      z[-2400,600]   the door at its north end
       EAST-WEST shaft    x[-2400,600]  z[-2400,-1800]
       the CORNER         x[0,600]      z[-2400,-1800] where the two meet

   THE CLEAVERS (src/cleaver.h). FOUR, two to an arm, and the two nearest the
   corner hang ON the corner's edges — its north edge z=-1800 across the N-S
   shaft, its west edge x=0 across the E-W one — so the corner square between
   them is a pocket the player can stand in while both are down. The other two
   are set back from the doors. See cll_place_cleavers().

   THE DOORS. Both are wired up:

     NORTH  z=600    x[200,400]      y[-400,0]  -> Meat Plant, south-alcove door
     WEST   x=-2400  z[-2200,-2000]  y[-400,0]  -> Zig Zag Tomb, east door

   The north door is in the XY plane approached from -Z (wall 0 runs z=600 with
   nz=-4096), so TEXT_PLANE_XY with mirror=0. The west door is in the YZ plane
   approached from +X, so TEXT_PLANE_YZ with mirror=0 — and it is LOCKED FROM
   THIS SIDE: the first Circle unlocks it (FLAG_ZIG_ZAG_DOOR), the Cleaver
   Corridor's south door's mechanic.

   TWO TEXTURES, BOTH BORROWED — cobblestone and the catacomb inner door,
   through src/catacombs_entry.c's narrow uploaders — plus The Pit's rusty
   ironwork for the blades, through the_pit_upload_rusty(). The room owns none.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void cleaver_l_load_assets(void);     /* startup: two compile-time headers     */
void cleaver_l_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void cleaver_l_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void cleaver_l_init(void);            /* collision + floor zones + spawn + blades */
void cleaver_l_draw(RenderContext *ctx);

/* Arrival through the north door, from the Meat Plant: just inside it,
   facing south down the shaft. cleaver_l_init()'s default. */
void cleaver_l_spawn_north(void);
/* Arrival through the west door, from the Zig Zag Tomb: just inside it,
   facing east up the shaft. */
void cleaver_l_spawn_west(void);

/* One frame of the north door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range and facing the door —
   the frame main.c starts the transition on. */
int  cleaver_l_north_door_triggered(int lock);
/* The west door's. The FIRST qualifying press unlocks it (FLAG_ZIG_ZAG_DOOR,
   SFX_UNLOCK) and returns 0; presses after that return 1. */
int  cleaver_l_west_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void cleaver_l_arm(void);

#endif
