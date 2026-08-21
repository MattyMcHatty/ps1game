#ifndef MAZE_TWO_H
#define MAZE_TWO_H

#include <stdint.h>
#include "render.h"

/* Maze Two: the second hedge maze, north of Maze One through the gate in that
   room's north hedge.

   Modelled in its OWN coordinate space, like every other room here. Its floor is
   at y=0, the same as Maze One's, and its connecting gate is on the SOUTH wall
   where Maze One's is on the north. Only the gate pairing links the two, so no
   offset is applied anywhere. (The two leaves are the same width to within four
   units — 595 in Maze One, 599 here — which is what pins the pairing; the
   implied shift, maze_two_x ~= maze_one_x - 3098, is not a round number and is
   not used for anything.)

   Bounds x[-4000,6200] z[0,4600] — 10200 x 4600, wider than Maze One but much
   shallower, so a slightly smaller footprint overall and a 107 KB mesh against
   Maze One's 117 KB. The arena is therefore unchanged: Maze One still sizes it.

   One flat plane at y=0 throughout (all seventeen collision floor planes agree)
   under a 500-tall hedge, cut into a maze by 500-tall hedge runs:

     PERIMETER  hedge around the whole footprint, broken by two gates.
     MAZE       forty-odd hedge runs dividing the interior into corridors. The
                narrowest is the 600-wide lane along the south wall, the same
                width as Maze One's; there is no second storey and nothing to
                climb, which is why multi_level is 0 and one floor zone does.
     FLOWERS    three poison_flower_base beds laid into the paths. ART ONLY for
                now — no rafflesia is seeded on them (rafflesia.c places Maze
                One's five and the Catacombs' three by name, and this room is not
                in either list).
     PIPE       a standpipe at x[4667,4733] z[2271,2329], 230 tall — the same
                borrowed texture Maze One's standpipe uses.
     PLINTH     a block at x[593,777] z[2238,2422], 131 tall, the room's one
                piece of set dressing and the one texture it owns.
     Neither the pipe nor the plinth has collision of its own: both stand hard
     against a hedge run, far inside its 195 wall push radius, so the player
     cannot reach the ground they occupy anyway. Same arrangement as Maze One's
     pipe.

   TWO gates are modelled; BOTH are connected... almost:

     SOUTH   the grdn_gte leaf at z=0, x[-1600,-1000], y[-600,0], in the XY
     WALL    plane. The far side of the gate in Maze One's north hedge. Its
             alcove is collision FLOOR 1, x(-1599,-1000) z(0,202), which fixes
             the centre at x=-1300. Collision wall 30 runs across the opening at
             z=0 with nz = +4096, so it is approached from +Z — from inside this
             maze — which for an XY sign is mirror=1 and a sign on the z+11 side.
             Note that is the OPPOSITE hand from Maze One's side of the same
             gate, whose wall faces -Z.

     EAST    the leaf at x=6200, z[3400,4000], y[-600,0], in the YZ plane. Drawn
     WALL    shut and backing onto solid collision (walls 24/28) — it is there
             for a room that does not exist yet.

   Rendered the same way as Maze One (per-poly tex map + one 128 texture window +
   purple outdoor fog + the cull-key reject path and side-plane frustum cull),
   with that room's fog distances exactly, and it borrows five of its six
   textures from three other modules' uploaders.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "Maze Two.smx" and
   "Maze Two mesh.smx" are both in that subdirectory, as Maze One's, Fountain
   Square's and the Outside Catacombs' are; gen_maze_two_tex_map.py defaults to
   that path and the two conversion commands in tools/ADDING_A_ROOM.txt STEP 4
   need it spelled out. Nothing else in the build reads them. */
void maze_two_load_assets(void);     /* startup: register streamed textures */
void maze_two_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void maze_two_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void maze_two_init(void);            /* set collision/floor zones + spawn */
void maze_two_draw(RenderContext *ctx);

/* The south-wall gate back to Maze One. */
void maze_two_gate_arm(void);        /* seed the Circle edge state */
int  maze_two_gate_triggered(void);  /* 1 on a fresh Circle press in range */

/* The room's only spawn, just inside the south gate. */
void maze_two_spawn_south(void);

#endif
