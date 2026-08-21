#ifndef MAZE_ONE_H
#define MAZE_ONE_H

#include <stdint.h>
#include "render.h"

/* Maze One: the hedge maze east of Fountain Square, through the gate in that
   room's east hedge.

   Modelled in its OWN coordinate space, like every other room here. Its floor is
   at y=0, the same as Fountain Square's paving, and its connecting gate is on
   the WEST wall where the square's is on the east. Only the gate pairing links
   the two, so no offset is applied anywhere. (The meshes ARE consistent about
   it: the two gate leaves have the identical arch profile — 570/600/600/570/
   547/523 deep — which is what pinned the pairing, and it works out to
   maze_z = fountain_z + 100.)

   Bounds x[-100,7500] z[-2095,4496] — by a wide margin the largest room in the
   game, about 7600 x 6600 against Fountain Square's 4000 x 4000. One flat plane
   at y=0 throughout (all twenty-seven collision floor planes agree) under a
   500-tall hedge, cut into a maze by 333-tall hedge runs:

     PERIMETER  hedge around the whole footprint, broken by three gates.
     MAZE       seventy-odd hedge runs dividing the interior into corridors
                roughly 600 units wide. There is no second storey and nothing
                to climb, which is why multi_level is 0 and one floor zone does.
     FLOWERS    five poison_flower_base beds laid into the paths.
     PIPE       a standpipe at x[5332,5382] z[973,1025], 209 tall — the room's
                one piece of set dressing and the one texture it owns. It has
                no collision of its own: at 50 x 52 it is far inside the wall
                push radius of the hedge run it stands against, so the player
                cannot reach the spot it occupies anyway.
     DRAIN      a channel across the paths at z[367,833]; art only, no collision.

   THREE gates are modelled; TWO are connected:

     WEST    the grdn_gte leaf at x=-100, z[-300,500], y[-600,0], in the YZ
     WALL    plane. The far side of the gate in Fountain Square's east hedge.
             Collision wall 73 here has nx = +4095, so it is approached from +X
             — from inside the maze — which for a YZ sign is mirror=0 and a sign
             on the x+11 side. Note that is the OPPOSITE hand from the square's
             side of the same gate, whose wall faces -X.

     NORTH   the leaf at z=4497, x[1501,2096], y[-600,0], in the XY plane, on to
     WALL    MAZE TWO's south gate. Its alcove is collision FLOOR 4,
             x(1501,2095) z(4300,4496), which fixes the centre at x=1798.
             Collision wall 19 runs across the opening at z=4496 with
             nz = -4096, so it is approached from -Z — again from inside the maze
             — which for an XY sign is mirror=0 and a sign on the z-11 side.
             The remaining one (east x=7100 z[-900,-312]) is drawn shut and backs
             onto solid collision — it is there for a room that does not exist
             yet.

   Rendered the same way as Fountain Square (per-poly tex map + one 128 texture
   window + purple outdoor fog), with that room's fog distances exactly, and it
   borrows five of its six textures from three other modules' uploaders.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "Maze One.smx" and
   "Maze One mesh.smx" are both in that subdirectory, as Fountain Square's and
   the Outside Catacombs' are; gen_maze_one_tex_map.py defaults to that path and
   the two conversion commands in tools/ADDING_A_ROOM.txt STEP 4 need it spelled
   out. Nothing else in the build reads them.

   >>> ITS MESH SIZES THE ROOM ARENA. <<< maze_one.smd is 117 KB, nearly twice
   the previous largest (outside_catacombs.smd, 66 KB), so tools/gen_room_arena.py
   now derives src/room_arena_size.h from THIS file. Re-export the room smaller
   and the arena shrinks with it; grow it further and the arena must be
   regenerated or room_arena_load will refuse the read and the room draws
   empty. */
void maze_one_load_assets(void);     /* startup: register streamed textures */
void maze_one_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void maze_one_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void maze_one_init(void);            /* set collision/floor zones + spawn */
void maze_one_draw(RenderContext *ctx);

/* Just the pipe, for Maze Two — which draws the same standpipe but no drain, so
   it must NOT call maze_one_upload_textures() wholesale (that would put the
   drain on x832 y0, where Maze Two's plinth lives). Run the courtyard's uploader
   first. */
void maze_one_upload_pipe(void);

/* The west-wall gate back to Fountain Square. */
void maze_one_gate_arm(void);        /* seed the Circle edge state */
int  maze_one_gate_triggered(void);  /* 1 on a fresh Circle press in range */

/* The north-wall gate on to Maze Two. Its own edge state, seeded separately. */
void maze_one_ngate_arm(void);
int  maze_one_ngate_triggered(void);

/* One spawn per connected gate. Either arms BOTH gates. */
void maze_one_spawn_west(void);      /* arriving from Fountain Square */
void maze_one_spawn_north(void);     /* arriving back from Maze Two   */

#endif
