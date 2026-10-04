#ifndef NURSERY_H
#define NURSERY_H

#include <stdint.h>
#include "render.h"

/* The Nursery: Chapter 3's TWENTY-SECOND room, through the catacomb door in
   the south-east corner of the Gaol Cells (their south wall, z=-500,
   x[4600,4800]).

   THE SHAPE. The Room of Arms' octagon again: 2586 across the flats, x/z
   [-1293,1293], flat and single-storey, its stone rising to y=-1200 with no
   drawn ceiling. Cobblestone throughout, and SIX gula-tablet tiles let into
   the floor in a ring about the centre.

   THE SIX COTS. One stands on each tile, its long axis pointing at the centre
   of the room. They are MIRRORS, not encounters (src/crib.h, THE NURSERY'S
   MIRRORS): each is lit — the encounter's beam, held still — while one of the
   chapter's five crib encounters is solved, and dark otherwise. Struck, they
   rock once and nothing else. The pairing (src/nursery.c, THE SIX COTS):

       north        Room of Arms        south        Room of Bones
       north-east   Room of Heads       south-west   Room of Torsos
       south-east   Room of Legs        north-west   THE SIXTH CRIB — still to
                                                     come; dark until it exists

   THE GULA TABLET. A stone slab standing across the west door, placed where
   its own export puts it (src/gula_tablet.h).

   THE DOORS. Two are drawn and ONE is wired up:

     EAST   x=1293  z[-107,107] y[-400,0]  -> Gaol Cells, south door
     west   x=-1293 z[-107,107] y[-400,0]     sealed behind the Gula Tablet

   The east door is in the YZ plane approached from -X (wall 6 runs x=1293
   with nx=-4095), so TEXT_PLANE_YZ with mirror=1 — the Room of Torsos' east
   door exactly.

   THREE TEXTURES, AND THE ROOM OWNS NONE. Cobblestone and the catacomb inner
   door come through src/catacombs_entry.c's narrow uploaders; `gula tablet`
   (4bpp, x640 y0, its own CLUT at (592,502)) through src/gula_tablet.c's.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void nursery_load_assets(void);     /* startup: three headers, no CD           */
void nursery_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void nursery_upload_textures(void); /* room entry: pure LoadImage from RAM      */
void nursery_init(void);            /* collision + floor zone + spawn + props   */
void nursery_draw(RenderContext *ctx);

/* Arrival through the east door, from the Gaol Cells, and the only arrival
   there is: just inside it, facing west into the ring of cots. */
void nursery_spawn_east(void);

/* One frame of the east door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door — the frame
   main.c starts the transition on. */
int  nursery_east_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void nursery_arm(void);

#endif
