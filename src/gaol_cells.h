#ifndef GAOL_CELLS_H
#define GAOL_CELLS_H

#include <stdint.h>
#include "render.h"

/* The Gaol Cells: Chapter 3's TWENTY-FIRST room, through the Gaol Entry's
   gaol door (x=899, z[300,700], in its east wall).

   THE SHAPE. x[0,4999] z[-1200,2399], flat at y=0 under a y=-800 ceiling. An
   entry corridor, z[233,966], runs east from the west gaol door, and barred
   cells divide up the rest (thin walls in the proxy), down to a strip along
   the south wall at z[-1200,-500]. Cobblestone for the corridors, mud for the
   cells' floors and walls. The visual mesh also carries a copy of the Gaol Entry west of
   x=0 (the shared world's 900-unit offset: Cells x = Entry x - 900), which is
   what is seen back through the door's bars; it is outside the collision.

   THE DOORS. Six are drawn and ONE is wired up:

     WEST   x=0     z[300,700]    y[-400,0]  -> Gaol Entry, gaol door
                                                LOCKED: takes the Gaol Key
     south  x[4600,4800] z=-500   y[-400,0]     not built (catacomb inner door)
     east   x=5000  z[500,700]    y[-400,0]     not built (catacomb inner door)
     cells  three barred cell doors             drawn in solid walls

   The west door is in the YZ plane approached from +X (wall 43 runs x=0 with
   nx=+4095), so TEXT_PLANE_YZ with mirror=0. ITS LOCK IS THE GAOL ENTRY'S:
   gaol_door_press() / gaol_door_sign() in src/gaol_entry.h, on
   FLAG_GAOL_DOOR. Using the Gaol Key on either face uses it up and opens both.

   FIVE TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the catacomb inner door
   come through src/catacombs_entry.c's narrow uploaders, the bars through
   src/bars.c's and the gaol door through src/gaol_entry.c's. `mud` is Asag's
   art converted again for this chapter, registered DEFERRED on the CRIB'S page
   and palette (x576 y0, CLUT (256,484)). See gaol_cells_load_assets().

   WHAT IS IN IT. A cold sconce with the chapter's third Blood Pearl on it, in
   the north-east cell; two Lumberers, one down the north corridor and one
   along the south strip (src/world.c), routing on this room's own nav table
   (src/lumberer.c); and a once-per-playthrough MAGGOT DROP — five out of the
   dark over the east door when the player crosses x=2637 in the centre
   corridor (gaol_cells_update below).

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void gaol_cells_load_assets(void);     /* startup: one deferred reg + headers */
void gaol_cells_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void gaol_cells_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void gaol_cells_init(void);            /* collision + floor zones + spawn       */
void gaol_cells_draw(RenderContext *ctx);

/* Arrival through the west gaol door, from the Gaol Entry, and the only
   arrival there is: just inside it, facing east down the entry corridor. */
void gaol_cells_spawn_west(void);

/* One frame of the west door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door ON THE UNLOCKED
   DOOR — the frame main.c starts the transition on. A press that unlocks it
   (Gaol Key carried) uses the key and returns 0. */
int  gaol_cells_west_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void gaol_cells_arm(void);

/* One frame of the maggot drop: the tripwire across the centre corridor, and
   then the five appearing over the east door one at a time. Called from
   main.c's Gaol Cells update branch; ONCE PER PLAYTHROUGH on
   FLAG_GAOL_CELLS_MAGGOTS, set on the frame the wire trips. */
void gaol_cells_update(void);

#endif
