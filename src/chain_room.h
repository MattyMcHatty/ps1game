#ifndef CHAIN_ROOM_H
#define CHAIN_ROOM_H

#include <stdint.h>
#include "render.h"

/* Chain Room: the short walled yard that joins Maze Two to the Keystone Maze,
   through Maze Two's east gate and the Keystone Maze's north gate. It is the
   first room in the garden that is not itself a maze — a single open rectangle
   with two necks off it — and it is by far the smallest, 243 primitives against
   Maze One's 1894.

   Modelled in its OWN coordinate space, like every other room here. Its floor is
   at y=0, the same as both mazes' and the Keystone Maze's, and only the gate
   pairings link the three; no offset is applied anywhere. Both leaves are 600
   wide and so are the openings they answer to (Maze Two's east gate spans
   z[3400,4000] at x=6200; the Keystone Maze's north gate spans x[900,1500] at
   z=5900), which is what pins the pairing.

   Bounds x[-200,1800] z[-1100,900] — 2000 x 2000 including both necks, so the
   whole room is inside the 2500 cull from anywhere in it and the mesh is 14 KB.
   The room arena is sized by Maze One's 118 KB and is untouched by this.

   One flat plane at y=0 throughout (all three collision floor planes agree),
   which is why multi_level is 0 and one floor zone does:

     YARD       x(0,1800) z(-900,900), gravel underfoot with a grass border,
                walled by hedge north and south and by a brick wall on the east.
     WEST NECK  x(-200,0) z(-300,300), the alcove behind the west gate.
     SOUTH NECK x(600,1200) z(-1100,-900), the alcove behind the south gate.
     PIPE       a standpipe at x[1683,1767] z[556,644] in the north-east corner,
                running the full 700 to the top of the brick wall. It has
                collision of its own — walls 13/14/15 box it in on three sides,
                the fourth being the wall it stands against.
     CHAINS     four quads at y=-700, strung diagonally across the yard between
                the tops of the walls. They are what the room is named for and
                the one thing in it that is not at ground level; nothing about
                them is collidable and nothing hangs from them.

   >>> THE HEDGE IS NOT THE TALLEST THING HERE. <<< The perimeter hedge is drawn
   to y=-500 and collides to -500, and collision_set_ceiling_y says -500 because
   that is the roofline over the walkable yard. The brick wall on the east and
   the chains above are drawn to -700. Anything ever hung in this room wants the
   -700, not the ceiling value and not the collision proxy — see
   tools/ADDING_A_ROOM.txt on visual-vs-collision heights.

   TWO gates, and BOTH are connected — the first room in the garden of which
   that is true from the day it landed:

     WEST    the grdn_gte leaf at x=-200, z[-300,300], y[-600,0], in the YZ
     WALL    plane. The far side of the gate in Maze Two's east wall. Collision
             wall 12 runs across the opening with nx = +4096, so the walkable
             side is +X and the player approaches from inside this room heading
             WEST — which for a YZ sign is mirror=0 and a sign on the x+11 side.
             That is the OPPOSITE hand from Maze Two's side of the same gate,
             whose wall (28) faces -X.

     SOUTH   the leaf at z=-1100, x[600,1200], y[-600,0], in the XY plane. The
     WALL    far side of the gate in the Keystone Maze's north hedge. Collision
             wall 9 runs across the opening with nz = +4096, so the walkable side
             is +Z and the player approaches from inside this room heading SOUTH
             — which for an XY sign is mirror=1 and a sign on the z+11 side.
             Again the opposite hand from the Keystone Maze's side, whose wall
             (13) faces -Z.

   Both walls STAY in the collision list: the leaves are shut as far as collision
   is concerned and it is the trigger, not a hole, that lets the player through.

   Rendered the same way as the three mazes (per-poly tex map + one 128 texture
   window + purple outdoor fog + the cull-key reject path and side-plane frustum
   cull), with Maze One's fog distances exactly, and it borrows five of its seven
   textures from the Garden Courtyard's uploader.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "Chain Room.smx" and
   "Chain Room mesh.smx" are both in that subdirectory, as both mazes', the
   Keystone Maze's, the Rear Gate's, Fountain Square's and the Outside
   Catacombs' are; gen_chain_room_tex_map.py defaults to that path and the two
   conversion commands in tools/ADDING_A_ROOM.txt STEP 4 need it spelled out.
   Nothing else in the build reads them. */
void chain_room_load_assets(void);     /* startup: register streamed textures */
void chain_room_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void chain_room_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void chain_room_init(void);            /* set collision/floor zones + spawn */
void chain_room_draw(RenderContext *ctx);

/* The west-wall gate, back into Maze Two. */
void chain_room_wgate_arm(void);        /* seed the Circle edge state */
int  chain_room_wgate_triggered(void);  /* 1 on a fresh Circle press in range */

/* The south-wall gate, into the Keystone Maze. */
void chain_room_sgate_arm(void);        /* seed the Circle edge state */
int  chain_room_sgate_triggered(void);  /* 1 on a fresh Circle press in range */

/* One spawn per gate; main.c picks between them on the arriving area. */
void chain_room_spawn_west(void);   /* arriving from Maze Two, facing +X */
void chain_room_spawn_south(void);  /* arriving from the Keystone Maze, facing +Z */

#endif
