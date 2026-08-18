#ifndef FOUNTAIN_SQUARE_H
#define FOUNTAIN_SQUARE_H

#include <stdint.h>
#include "render.h"

/* Fountain Square: the hedge-walled parterre north of the Garden Courtyard,
   through the gate set into that courtyard's north hedge.

   Modelled in its OWN coordinate space, like every other room here. Its floor
   is at y=0 where the courtyard's lawn is y=900, and its gate is on the SOUTH
   wall where the courtyard's is on the north. Only the door pairing links the
   two, so no offset is applied anywhere.

   Bounds x[-1994,2005] z[-1822,2177], one flat paved plane at y=0 under a
   500-tall hedge perimeter. The plan is a formal parterre:

     PERIMETER  a continuous 500-tall hedge at x=+/-1820 and z=+/-1822, broken
                at the middle of each side by a gate 728 wide.
     PARTERRE   twelve 200-tall hedge blocks in the middle, laid out as four
                L-shaped masses about the centre. The paths between them are the
                central cross (|x| < 364 and |z| < 364) and the 728-wide ring
                inside the perimeter.
     FOUNTAIN   an octagonal stone basin at the origin, radius 182 and 149 tall,
                where the two arms of the cross meet.
     DRAIN      a channel cut across the paving at z[243,304]; art only, no
                collision of its own.

   FOUR gates are modelled, one per side, but only the SOUTH one is connected:

     SOUTH   the grdn_gte leaf at z=-1822, x[-364,364], y[-600,0]. The far side
     WALL    of the gate in the Garden Courtyard's north hedge, and today the
             only way in or out. The three others (west x=-1994, east x=2005,
             north z=2177) are drawn shut and back onto solid collision — they
             are there for rooms that do not exist yet.

   Rendered the same way as the Garden Courtyard (per-poly tex map + one 128
   texture window + purple outdoor fog), and it reuses that room's texture
   uploads wholesale — four of this mesh's six textures are the courtyard's.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "Fountain Square.smx"
   and "Fountain Square mesh.smx" are both in that subdirectory, which is where
   they were dropped; gen_fountain_square_tex_map.py defaults to that path and
   the two conversion commands in tools/ADDING_A_ROOM.txt STEP 4 need it spelled
   out. Nothing else in the build reads them. */
void fountain_square_load_assets(void);     /* startup: register streamed textures */
void fountain_square_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void fountain_square_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void fountain_square_init(void);            /* set collision/floor zones + spawn */
void fountain_square_draw(RenderContext *ctx);

/* The south-wall gate back to the Garden Courtyard. */
void fountain_square_gate_arm(void);        /* seed the Circle edge state */
int  fountain_square_gate_triggered(void);  /* 1 on a fresh Circle press in range */

/* Spawn just inside the south gate — the only arrival. */
void fountain_square_spawn_south(void);

#endif
