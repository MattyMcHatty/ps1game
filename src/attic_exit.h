#ifndef ATTIC_EXIT_H
#define ATTIC_EXIT_H

#include <stdint.h>
#include "render.h"

/* Attic Exit: the room north of the Attic Stairwell's west room, reached
   through the single wooden door in that room's north wall. A plain rectangle,
   x[-1500,1500] z[-999,999], flat floor at y=0 with a drawn ceiling at y=-560:

     CAGE      a chainlink enclosure filling x[-543,544] z[31,999] against the
               north wall, entered through the gap at x[-300,300], z~15. Its
               fence panels are collision walls like any other, so the only way
               in is that gap.
     SOUTH     the wd_dr door at x[-500,-300], z=-1000 — back to the Attic
     WALL      Stairwell's west room (its north-wall door at x=-1942, z=350).
     NORTH     the xt_dr_lckd exit door at x[-225,225], y[-467,0], z=1000,
     WALL      inside the cage. It carries the exit-door keystone puzzle (see
               exit_door_puzzle.c); solving it swaps the art to xt_dr_cmplt. No
               room is wired behind it yet — the open door TEMPORARILY sends the
               player back to this room's own entrance.
     ENEMIES   Four tentacles, two in front of each of the two north-wall
               levers, in the corridors either side of the cage (placed in
               tentacles_init).

   Rendered the same way as the Attic Stairwell (per-poly tex map + 128 texture
   window + fog). */
void attic_exit_load_assets(void);     /* startup: geometry + texture registration */
void attic_exit_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */

/* Exit-door puzzle solved: put the unlocked xt_dr_cmplt art up straight away.
   The caller must have set FLAG_EXIT_DOOR_UNLOCKED first — that flag is what
   picks the art on every subsequent room entry too. */
void attic_exit_unlock_door(void);
void attic_exit_init(void);            /* set collision/floor zones + spawn */

/* Place the cage gate and the four lightswitch levers according to the CURRENT
   game_flags. attic_exit_init calls it; main.c calls it again after a Load Game
   installs the saved flags, since the init above ran on the pre-load values. */
void attic_exit_apply_flags(void);

void attic_exit_draw(RenderContext *ctx);

/* The south-wall door back to the Attic Stairwell. */
void attic_exit_door_arm(void);        /* seed the Circle edge state */
int  attic_exit_door_triggered(void);  /* 1 on a fresh Circle press in range */

/* Spawn just inside the south door on arrival from the Attic Stairwell. */
void attic_exit_spawn_south(void);
/* ...and just inside the north exit door on arrival back from the Garden Stairs. */
void attic_exit_spawn_north(void);

#endif
