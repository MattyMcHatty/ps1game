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

   FOUR gates are modelled, one per side, and ALL FOUR are now connected:

     SOUTH   the grdn_gte leaf at z=-1822, x[-364,364], y[-600,0]. The far side
     WALL    of the gate in the Garden Courtyard's north hedge. Collision wall
             79 there has nz = +4096, so it is approached from +Z (mirror=1).
     NORTH   the leaf at z=2177, same x span. It opens on the Outside
     WALL    Catacombs' south gate. Collision wall 66 has nz = -4096, so it is
             approached from -Z — the opposite face, hence mirror=0 and a sign
             on the z-11 side.
     EAST    the leaf at x=2005, z[-364,364] — this one in the YZ plane. It
     WALL    opens on Maze One's west gate at that room's x=-100. Collision wall
             54 has nx = -4096, so it is approached from -X, which for a YZ sign
             is mirror=1 and a sign on the x-11 side.
     WEST    the leaf at x=-1994, z[-364,364], the mirror of the east one and
     WALL    also in the YZ plane. It opens on the Rear Gate's east gate at that
             room's x=2200. Collision wall 81 has nx = +4096 — the OPPOSITE face
             from the east gate — so it is approached from +X, which for a YZ
             sign is mirror=0 and a sign on the x+11 side. Its alcove is
             collision FLOOR 0, x(-1994,-1820) z(-364,364).

   Four gates means FOUR independent Circle edge states, and every spawn below
   arms all of them: a press carried in through any transition would otherwise
   fire whichever interaction was left unarmed and bounce the player straight
   back out of the room they just entered.

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

/* The north-wall gate on to the Outside Catacombs. Its own edge state, so a
   press consumed by one gate cannot re-arm the other. */
void fountain_square_ngate_arm(void);
int  fountain_square_ngate_triggered(void);

/* The east-wall gate on to Maze One. Its own edge state again, for the same
   reason. Unlike the other two this one is in the YZ plane (fixed X). */
void fountain_square_egate_arm(void);
int  fountain_square_egate_triggered(void);

/* The west-wall gate on to the Rear Gate. Its own edge state again, for the same
   reason; in the YZ plane like the east one, but approached from the other side. */
void fountain_square_wgate_arm(void);
int  fountain_square_wgate_triggered(void);

/* Just the drain, for a room that draws the channel but not the fountain — Maze
   One, whose own pipe occupies the fountain's page, and the Rear Gate, whose
   brick wall does. Run the courtyard's uploader first. */
void fountain_square_upload_drain(void);

/* Spawns, one per connected gate. The south one is the room's default; main.c
   overrides with the north one when arriving from the catacombs, the east one
   when arriving from Maze One and the west one when arriving from the Rear
   Gate. */
void fountain_square_spawn_south(void);
void fountain_square_spawn_north(void);
void fountain_square_spawn_east(void);
void fountain_square_spawn_west(void);

#endif
