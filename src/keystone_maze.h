#ifndef KEYSTONE_MAZE_H
#define KEYSTONE_MAZE_H

#include <stdint.h>
#include "render.h"

/* Keystone Maze: the third hedge maze, east of Maze One through the gate in
   that room's east hedge.

   Modelled in its OWN coordinate space, like every other room here. Its floor is
   at y=0, the same as Maze One's, and its connecting gate is on the WEST wall
   where Maze One's is on the east. Only the gate pairing links the two, so no
   offset is applied anywhere. (The two leaves are the same width to within
   twelve units — 588 in Maze One, 600 here — which is what pins the pairing; the
   implied shift is not a round number and is not used for anything.)

   Bounds x[-300,5900] z[-300,5900] — a 6200 x 6200 square, a little smaller than
   Maze One's 7600 x 6600 and a much simpler plan: 1439 prims against 2037, and
   an 83 KB mesh against 117 KB, so the room arena is unchanged and Maze One
   still sizes it.

   One flat plane at y=0 throughout (all thirteen collision floor planes agree)
   under a 500-tall hedge, cut into corridors 600 wide by 500-tall hedge runs:

     PERIMETER  hedge around the whole footprint, broken by three gates.
     MAZE       a ring of 600-wide corridors around the outside, opening into
                one large central court. There is no second storey and nothing
                to climb, which is why multi_level is 0 and one floor zone does.
     COURT      the middle of the room, x(900,3899) z(900,3900), paved in gravel
                over its inner x[1300,3500] z[1300,3500].
     PLINTHS    four 200 x 200 blocks 120 tall standing at the court's corners
                (x/z 1500-1700 and 3100-3300 in both combinations), and the
                KEYSTONE itself at x(2300,2500) z(2300,2500), drawn 150 tall and
                faced entirely in plinth_diamond. All five have collision of
                their own — unlike Maze Two's plinth, these stand in open ground
                where the player can walk into them. >>> THE KEYSTONE'S PROXY IS
                TALLER THAN THE BLOCK: the visual mesh was re-exported shorter
                (200 -> 150) while the collision mesh was not, so wall 64..67
                still stand 304 tall around it. Harmless as it stands — this room
                is multi_level 0, so the wall Y is never read and the push is the
                same footprint — but do not take 304 for a drawn height. <<<
     SET        five more plinths of the same pattern are DRAWN into the hedges
     DRESSING   (x[-100,100] z[3500,3700] and z[5300,5500]; x[4700,4900]
                z[1100,1300]; x[5300,5500] z[5300,5500], all four now 175 tall
                with a plinth_diamond cap; and x[4903,5067] z[-267,-103] at 120).
                The first four are buried in hedge blocks with no floor under
                them, so the player cannot reach the ground they occupy — the
                same arrangement as Maze One's standpipe — and they have no
                collision. >>> THE FIFTH, at x[4903,5067] z[-267,-103], IS NOT:
                it stands on collision FLOOR 10, in open corridor, and the
                collision mesh has no walls for it, so the player walks through
                it. Add four faces for it to "Keystone Maze mesh.smx" and
                regenerate if that matters. <<<

   THREE gates are modelled; ONE is connected:

     WEST    the grdn_gte leaf at x=-300, z[-300,300], y[-600,0], in the YZ
     WALL    plane. The far side of the gate in Maze One's east hedge. Collision
             wall 44 runs across the opening at x=-300 with nx = +4096, so it is
             approached from +X — from inside this maze — which for a YZ sign is
             mirror=0 and a sign on the x+11 side. Note that is the OPPOSITE hand
             from Maze One's side of the same gate, whose wall faces -X.

     NORTH   the leaf at z=5900, x[900,1500]. Drawn shut, backing onto collision
     WALL    wall 13 — there for a room that does not exist yet.

     EAST    the leaf at x=5900, z[2100,2700]. Drawn shut, backing onto collision
     WALL    wall 28 — likewise.

   Rendered the same way as both mazes (per-poly tex map + one 128 texture window
   + purple outdoor fog + the cull-key reject path and side-plane frustum cull),
   with Maze One's fog distances exactly, and it borrows five of its six textures
   from two other modules' uploaders.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "Keystone Maze.smx"
   and "Keystone Maze mesh.smx" are both in that subdirectory, as both mazes',
   Fountain Square's and the Outside Catacombs' are; gen_keystone_maze_tex_map.py
   defaults to that path and the two conversion commands in
   tools/ADDING_A_ROOM.txt STEP 4 need it spelled out. Nothing else in the build
   reads them. */
void keystone_maze_load_assets(void);     /* startup: register streamed textures */
void keystone_maze_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void keystone_maze_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void keystone_maze_init(void);            /* set collision/floor zones + spawn */
void keystone_maze_draw(RenderContext *ctx);

/* The west-wall gate back to Maze One — this room's only connection. */
void keystone_maze_gate_arm(void);        /* seed the Circle edge state */
int  keystone_maze_gate_triggered(void);  /* 1 on a fresh Circle press in range */

/* The room's only spawn, just inside the west gate. */
void keystone_maze_spawn_west(void);

#endif
