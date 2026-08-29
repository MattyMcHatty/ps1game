#ifndef THE_HATCH_H
#define THE_HATCH_H

#include <stdint.h>
#include "render.h"

/* The Hatch: the walled lawn east of the Keystone Maze, through the gate in
   that room's east hedge. The end of the garden's eastern line — nothing leads
   on from it, and its west gate is the only way in or out.

   Modelled in its OWN coordinate space, like every other room in this chain. Its
   floor is at y=0, the same as all three mazes' and the Chain Room's, and its
   connecting gate is on the WEST wall where the Keystone Maze's is on the east.
   Only the gate pairing links the two, so no offset is applied anywhere. (The
   two leaves are the same 600 wide, which is what pins the pairing; the implied
   shift is not a round number and is not used for anything.)

   Bounds x[-200,4800] z[-1499,3900] — 5000 x 5400, between the Keystone Maze's
   6200 x 6200 and the Chain Room's 2000 x 2000. 661 prims and a 37 KB mesh, so
   the room arena is unchanged and Maze One (118 KB) still sizes it, as it sizes
   the cull arena at 2056 prims against this room's 661.

   ONE flat plane at y=0 throughout — all EIGHT collision floor planes agree —
   under a 500-tall hedge. Four rooms' worth of ground, in a line:

     CORRIDOR   FLOOR 0, x(-200,1800) z(-300,300). The 600-wide neck in from the
                west gate. It runs dead straight at the pit below.
     THE YARD   FLOORS 3-7, five planes tiling x(1800,4800) z(-1499,1500) with
                its four corners bitten out. A 3000 x 3000 lawn, hedged on all
                four sides, with four plinths and one hole in it. The generator
                emits it as five rectangles rather than one because the corner
                blocks interrupt it — see SET DRESSING below — and every one of
                the five is at y=0 like the rest.
     PASSAGE    FLOOR 1, x(3000,3600) z(1500,2100). A second 600-wide neck, out
                of the yard's north hedge.
     CHAMBER    FLOOR 2, x(2400,4199) z(2100,3900). The north room, and what the
                whole place is for: the brick well and its hatch stand in it.

   >>> THE PIT IS NOT A FLOOR ZONE AND MUST NOT BECOME ONE. <<< x(3000,4200)
   z(-300,300), straight ahead as the player comes down the corridor: the lawn
   simply stops and grass-textured sides fall 1200 units to a floor of eighteen
   flat-BLACK quads at y=1200. There is no collision floor down there and no
   FLOOR_* zone over it, because the player never reaches it — walls 20..23 fence
   the hole on all four sides, and every one of the four faces AWAY from the
   interior (20 at x=4200 nx=+4096, 21 at x=3000 nx=-4096, 22 at z=-300
   nz=-4096, 23 at z=300 nz=+4096), so the push is outward from every approach.
   The single FLOOR_FLAT zone this room installs spans the whole footprint,
   including the hole; that is correct precisely because the walls make the hole
   unreachable, and it is what keeps the player at lawn height while they stand
   at the lip and look in.

   THE HATCH ITSELF, in the north chamber:
     WELL     a brick_wall box, x[3000,3600] z[2700,3300], drawn 200 tall
              (y[-200,0]). Collision walls 3, 5, 6 and 14 stand around it, all
              facing outward, so the player can walk up to it but not into it.
     LID      the `hatch` leaf, x[3202,3584] y[-639,-200] z[2963,3037]: thirteen
              polys standing ON TOP of the well and reaching 639 above the lawn —
              139 higher than the hedge line and the tallest thing in the room.
     CHAINS   four chain_128 polys, x[3202,3270] y[-570,-178], holding it up.
              The same 4x4-tiled art the Chain Room and Maze One use, so the
              links keep their 32-texel period.
     >>> collision_set_ceiling_y() is set to the DRAWN HEDGE ROOFLINE at -500,
     not to the lid at -639. That is the height over the walkable ground, which
     is what a ceiling probe wants; the lid stands over a footprint the player is
     fenced out of. Anything ever hung off the hatch wants -639 by hand — see
     tools/ADDING_A_ROOM.txt on visual-vs-collision heights. <<<

   SET DRESSING, AND IT IS SOLID: four plinth blocks 200 x 200 and 130 tall, at
   x[2000,2200] and x[4400,4600] crossed with z[-1300,-1100] and z[1100,1300] —
   the yard's four corners. Unlike the Keystone Maze's, which are buried in hedge
   and unreachable, all four of these stand on open lawn, so the collision mesh
   gives them proxies (walls 4, 7, 8, 9, 11, 24, 26, 27, 28, 29, 30, 31).

   >>> THE PROXY IS THE WHOLE CORNER, NOT THE BLOCK. <<< Each plinth sits 200
   units off both hedges that meet beside it, and what the walls actually fence
   is the full 400 x 400 corner square containing it — x(1800,2200) or
   x(4400,4800), crossed with z(1099,1500) or z(-1499,-1099). The L-shaped strip
   between block and hedge is blocked along with the block. That is deliberate
   and not a mis-export: there is nothing in that strip, and squaring the corner
   off keeps the player from wedging into a 200-unit slot they cannot turn round
   in. It is why the yard's floor comes out as five rectangles instead of one.
   Do not read 400 as the DRAWN size of anything — the blocks are 200 x 200 and
   130 tall, and the drawn height is the one to hang anything from.

   ONE gate, and it is connected:

     WEST    the grdn_gte leaf at x=-200, z[-300,300], y[-600,0], in the YZ
     WALL    plane. The far side of the gate in the Keystone Maze's east hedge,
             which was drawn shut and backed onto solid collision until this room
             was built (src/keystone_maze.h). Collision wall 0 runs across the
             opening at x=-200 with nx = +4096, so the walkable side is +X — from
             inside this room — which for a YZ sign is mirror=0 and a sign on the
             x+11 side. That is the OPPOSITE hand from the Keystone Maze's side
             of this same gate, whose wall 28 faces -X.

   Rendered like the rest of the garden (per-poly tex map + one 128 texture
   window + purple outdoor fog + the cull-key reject path and side-plane frustum
   cull), with Maze One's fog distances exactly, and it borrows six of its seven
   textures from three other modules' uploaders. The seventh, `hatch`, is the
   only one it owns and it is STREAMED on entry rather than registered — see the
   heap note in src/the_hatch.c.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "The Hatch.smx" and
   "The Hatch mesh.smx" are both in that subdirectory, as all three mazes',
   the Chain Room's, the Rear Gate's, Fountain Square's and the Outside
   Catacombs' are; gen_the_hatch_tex_map.py defaults to that path and the two
   conversion commands in tools/ADDING_A_ROOM.txt STEP 4 need it spelled out.
   Nothing else in the build reads them. */
void the_hatch_load_assets(void);     /* startup: tpage/clut constants only */
void the_hatch_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void the_hatch_upload_textures(void); /* room entry: borrowed uploads + one stream */
void the_hatch_init(void);            /* set collision/floor zones + spawn */
void the_hatch_draw(RenderContext *ctx);

/* The west-wall gate, back to the Keystone Maze. */
void the_hatch_gate_arm(void);        /* seed the Circle edge state */
int  the_hatch_gate_triggered(void);  /* 1 on a fresh Circle press in range */

/* The room's only arrival. Arms every interaction in the room. */
void the_hatch_spawn_west(void);      /* arriving from the Keystone Maze, facing +X */

#endif
