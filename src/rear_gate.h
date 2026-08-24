#ifndef REAR_GATE_H
#define REAR_GATE_H

#include <stdint.h>
#include "render.h"

/* Rear Gate: the walled rear lawn WEST of Fountain Square, through the gate in
   that room's west hedge — the fourth and last of the square's four modelled
   gates to be connected.

   Modelled in its OWN coordinate space, like every other room here. Its floor is
   at y=0 where the square's is, and its connecting gate is on the EAST wall
   where the square's is on the west. Only the gate pairing links the two, so no
   offset is applied anywhere. (The leaves are not the same width — 800 here
   against the square's 728 — which is normal for this chain; the two meshes are
   independent and it is the pairing, not a shared origin, that joins them.)

   Bounds x[-2200,2200] z[-3200,4106] — 4400 x 7306, and 1198 prims. The mesh is
   68 KB, comfortably inside the arena Maze One's 117 KB sizes.

   THE LAYOUT, north to south. This is the first garden room that is NOT one flat
   plane, so the shape matters:

     LAWN      the open north half, z[1500,4106], inside a 500-tall perimeter
               hedge. It narrows twice going north — x[-2000,2000] up to
               z=2500, x[-1799,1800] from z=3100, x[-1300,1300] from z=3600 —
               finishing at the north gate. All of it at y=0.
     PLINTH    a 200-tall block at x[-300,300] z[2500,3100], standing in the
               middle of the lawn. Collision walls 2/34/35/36 box it, so it is a
               real obstacle to walk around rather than set dressing. HADAD
               stands on it (src/hadad.h) — 600 x 600, so his crown reaches
               y=-800 and clears the perimeter hedge by 300. The inscription on
               the block's south face is his epitaph.
     ALCOVES   two shallow dead ends off the lawn's south edge at z[1299,1500],
               x[900,2000] and x[-2000,-900].
     PATH      a hedged corridor running south from the lawn at x[-300,300]. Its
               mouth is the 600-wide gap at z=1500 between walls 14 and 15; walls
               0 and 1 hem it in on both sides from z=1500 down to z=-1500.
               TWO GRINDERS (src/grinder.h) are set into its side walls at
               z=-85, one east and one west, each buried 320 of its 400 depth in
               the hedge so only the textured plate shows. They are the room's
               only props, and the hook for a puzzle that will move them.
     COURTS    two side courtyards, x[-900,-300] and x[300,899], both spanning
               z[-3200,-1500]. The ONLY way into either is the 600-deep opening
               at z[-2100,-1500] where the path's side walls have ended and the
               ramp's have not yet begun (walls 24/26 pick up at z=-2100). Both
               are at y=0 and both are dead ends.
     RAMP      the gravel path continuing south from z=-2100, climbing 500 units
               over 1100 to z=-3200. Walls 24 and 26 rail it on both sides. THIS
               IS WHY THE ROOM HAS TWO FLOOR ZONES: the collision generator
               reports it as "FLOOR 9: y=-250", which is the average of a slope,
               not a plane — see rear_gate_floor_zones_init.
     DOOR      a double_door at the top of the ramp, in the 1500-tall brick wall
               at z=-3200, x[-300,300], drawn y[-900,-500]. Its sill is at
               y=-500, exactly the ramp's top, so it reads as a door you can walk
               up to and not as art floating over the lawn. IT LEADS TO THE WEST
               CORRIDOR (src/west_corridor.h) — trigger, sign and spawn are
               rear_gate_sdoor_* and rear_gate_spawn_south below. What closes
               that end is collision WALL 39, the brick wall's upper band at
               y[-1237,-500] — it stops the player at z=-2994, standing on the
               ramp at y=-410 and looking at the door from 200 away. Walls 27 and
               33 alone would not have done it: they run only x[-900,-300] and
               x[300,899], so the door's own 600-wide opening had nothing across
               it and the player walked out of the room and fell 500 units.
               (Wall 39 exists only because smx_to_collision.py was fixed to
               treat the HIGH end of a ramp as a floor level — see the note at
               the top of rear_gate_mesh_collision.c.) The room behind the
               mansion it was the hook for is the West Corridor.

   THREE gates are modelled; ONE is connected:

     EAST    the grdn_gte leaf at x=2200, z[1500,2300], y[-500,0], in the YZ
     WALL    plane. The far side of the gate in Fountain Square's west hedge. Its
             alcove is collision FLOOR 6, x(2000,2200) z(1500,2300), which fixes
             the centre at z=1900. Collision wall 17 runs across the opening at
             x=2200 with nx = -4096, so it is approached from -X — from inside
             this room — which for a YZ sign is mirror=1 and a sign on the x-11
             side. That is the OPPOSITE hand from Fountain Square's side of the
             same gate, whose wall faces +X.

     WEST    the leaf at x=-2200, z[1500,2300], the mirror of the east one, with
     WALL    its own alcove (FLOOR 7) behind collision wall 37. IT NOW OPENS ON
             THE STABLES (src/stables.h) — the "there for a room that does not
             exist" note above it is out of date. Wall 37's normal is nx = +4096,
             the OPPOSITE hand from wall 17 on the east side, so this gate is
             approached from +X and its sign is mirror=0 on the x+11 side where
             the east one is mirror=1 on x-11. The two leaves are identical in
             the mesh and differ only in that.

     NORTH   the leaf at z≈4100, x[-1300,1300], behind collision wall 13. Still
     WALL    drawn shut, still unconnected — the last one.

   NO ROOM MUSIC. cdaudio_stop() on arrival, the Garden Stairs' and the Garden
   Courtyard's arrangement rather than Fountain Square's — stopped rather than
   merely not started, so a title-screen load or a debug level-select jump
   (neither of which passes through the gate transition's own stop) also arrives
   in silence. HADAD BRINGS HIS OWN (CD-DA track 8), started and stopped by
   update_hadads and cut by world_silence_monsters on the way out; the silence
   above is this room's baseline, not a guarantee. Fog and cull are MAZE ONE'S exactly, 575/2500 in the garden's
   purple, so the step through the square's west gate changes the plan of the
   place but not the weather.

   NINE mesh textures, the most of any room in the game, and it owns two — both
   byte-for-byte retargets rather than new art, because it is the first room to
   draw drain, plinth AND double_door at once and all three live on the opn_drwr
   page. See the slot table in rear_gate.c and KNOWN_STREAM_PAIRS in
   tools/vram_map.py.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "Rear Gate.smx" and
   "Rear Gate mesh.smx" are both in that subdirectory, as Maze One's, Maze Two's,
   Fountain Square's and the Outside Catacombs' are; gen_rear_gate_tex_map.py
   defaults to that path and the two conversion commands in
   tools/ADDING_A_ROOM.txt STEP 4 need it spelled out. Nothing else in the build
   reads them. */
void rear_gate_load_assets(void);     /* startup: register streamed textures */
void rear_gate_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void rear_gate_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void rear_gate_init(void);            /* set collision/floor zones + spawn */
void rear_gate_draw(RenderContext *ctx);

/* The east-wall gate back to Fountain Square. */
void rear_gate_gate_arm(void);        /* seed the Circle edge state */
int  rear_gate_gate_triggered(void);  /* 1 on a fresh Circle press in range */

/* The west-wall gate, into the Stables. The mirror of the east one in every
   respect but the collision normal, which is what decides the sign's hand and
   which side of the wall the spawn goes on — see RG_WGATE_X in rear_gate.c. */
void rear_gate_wgate_arm(void);       /* seed the Circle edge state */
int  rear_gate_wgate_triggered(void); /* 1 on a fresh Circle press in range */

/* The plinth's inscription: a floating "Press O to read" on the block's SOUTH
   face, and the Circle press that posts its epitaph to the log. Purely a read —
   it sets no flag, gives nothing and can be read any number of times, so there
   is nothing here for the save blob to carry. Placement and the south-side
   gating are documented at RG_PLINTH_X in rear_gate.c. */
void rear_gate_plinth_arm(void);       /* seed the Circle edge state */
void rear_gate_plinth_update(int lock);/* per-frame; `lock` suppresses the press */

/* The double door at the top of the southern ramp, into the West Corridor. The
   "IT GOES NOWHERE YET" note above is now out of date — it is a normal door
   transition (DOOR_PANEL_OUTER, the Delivery/Kitchen one), and collision wall
   39 still closes the opening. */
void rear_gate_sdoor_arm(void);        /* seed the Circle edge state */
int  rear_gate_sdoor_triggered(void);  /* 1 on a fresh Circle press in range */

/* The three spawns: just inside the east gate, just inside the west gate, and on
   the ramp below the south door. Each arms ALL FOUR interactions in the room
   (both gates, the plinth and the south door), so whichever one main.c runs the
   others are safe. */
void rear_gate_spawn_east(void);
void rear_gate_spawn_west(void);
void rear_gate_spawn_south(void);

#endif
