#ifndef THE_PIT_H
#define THE_PIT_H

#include <stdint.h>
#include "render.h"

/* The Pit: Chapter 3's SIXTH room, through the NORTH door of the Tomb — the last
   of the three doors that room's header has been listing as "drawn, not built".

   THE SHAPE, AND IT IS THE FIRST ROOM IN THE CHAPTER WITH TWO WALKABLE HEIGHTS
   THAT ARE NOT A MAZE. A rectangular shaft 4800 by 3900, open from top to bottom,
   with a GALLERY running round three of its sides at y=-1000 and the PIT FLOOR
   itself 1000 units below at y=0. The collision proxy found nine floor faces and
   they split cleanly in two:

     THE GALLERY   y=-1000, the upper level and the only part the player can
                   reach. A U: a south ledge x[-1200,1800] z[0,600] with the
                   arrival door in it, chamfered corners at both ends, and two
                   ARMS running north up the outside of the shaft —
                   east x[2099,2699] and west x[-2104,-1504], both z[910,3300].
                   Both arms are DEAD ENDS at z=3300.

     THE PIT       y=0, x[-692,1292] z[1099,3300], with a north alcove
                   x[-125,725] z[3300,3900] holding a second drawn door.

   >>> THE TWO DO NOT CONNECT, AND THAT IS KNOWN AND DELIBERATE FOR NOW. <<<
   There is no ramp, no stair and no drop between them: the gallery's rim (walls
   0-4) seals the pit off in plan and the pit's own walls (5, 14-18, 22, 23) stop
   1000 units below the gallery's floor, so nothing in the proxy bridges them.
   The player walks in through the south door, walks the U, looks down into the
   shaft and walks back out. The way down is a later job; everything in this
   header and in the .c is written so that adding it is a floor zone and a wall,
   not a re-think.

   THE VAULT is at y=-1800 — 800 of headroom over the GALLERY, the chapter's
   usual, and 1800 over the pit floor, which is the deepest drawn space in the
   game. The collision proxy's gallery walls stop at -1800 too, so the two agree
   and collision_set_ceiling_y(-1800) is the honest number. >>> ANYTHING HUNG
   FROM IT IS 1800 ABOVE THE PIT AND 800 ABOVE THE LEDGE THE PLAYER IS ACTUALLY
   STANDING ON. <<< Author against whichever of the two the thing is over, not
   against the single value this room reports.

   THE DOORS. Two are drawn, and ONE of them is wired up:

     SOUTH  z=0     x[200,400]    y[-1400,-1000]  -> Tomb, north door.
                    On the GALLERY, in the south ledge.
     north  z=3900  x[158,442]    y[-500,0]       not built.
                    On the PIT FLOOR, in the north alcove — i.e. in the half of
                    the room the player cannot reach yet. It reads as a sealed
                    door, which is what it is twice over.

   Both are XY-plane doors (fixed Z) and both are approached from +Z, so the live
   one takes TEXT_PLANE_XY with mirror=1 and its sign 11 units proud of the wall
   along +Z. Wall 19 runs z=0 with nz=+4096, which is where that comes from. The
   Tomb's north door, the far side of this wall, takes the opposite pair for the
   same reason in reverse (its wall 19 has nz=-4096).

   NO STOREY TEST ON THE SOUTH DOOR, and this is a room where that has to be
   argued rather than assumed. The Up Down Maze's two doors are the only triggers
   in the game that test Y, because there the walkable surface is not a function
   of XZ — a corridor runs directly under a door. Here it is a function of XZ:
   the gallery and the pit have DISJOINT footprints (compare the two lists above
   — the pit starts at z=1099 and the gallery has nothing past z=910 inside
   x[-692,1292]), so no point in the room has two walkable heights and a plain
   Manhattan test is correct. The nearest pit floor is 1099 from the door in
   plan against a 500 trigger radius, so it is not even close.
   >>> RE-READ THAT WHEN THE TWO SECTIONS ARE CONNECTED. <<< A ramp or a stair
   under the south ledge is exactly the thing that would break it, and it would
   break silently: the player would open the Tomb's door from the bottom of the
   shaft.

   THREE TEXTURES, AND THE ROOM OWNS ONE — the Room of Arms' arrangement.
   Cobblestone and the catacomb inner door are registered by
   src/catacombs_entry.c in TEXBANK_CATACOMBS and reached through that module's
   two NARROW uploaders, as every Chapter 3 room since the Up Down Maze reaches
   theirs. `rusty` is the corroded ironwork lining the shaft, new art nothing else
   in the game draws, so this room registers it — deferred, like the rest of the
   chapter. Its VRAM page is x704 y0, the LAST whole mesh-art page in the
   Catacombs bank, and it costs seven other modules' uploaders a promise it did
   not cost the crib. The whole argument is in the_pit_load_assets() and in
   tools/vram_map.py beside the stream pairs.

   Its exports live in assets/catacombs/, beside the other five Chapter 3
   rooms'. */

void the_pit_load_assets(void);     /* startup: one deferred reg + three headers */
void the_pit_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena  */
void the_pit_upload_textures(void); /* room entry: pure LoadImage from RAM       */
void the_pit_init(void);            /* collision + floor zones + spawn          */
void the_pit_draw(RenderContext *ctx);

/* Arrival through the south door, from the Tomb: standing on the gallery's south
   ledge just inside it, facing north across the shaft. */
void the_pit_spawn_south(void);

/* One frame of the south door's Circle test. `lock` is main's usual suppression
   (a menu is up, a cutscene owns the camera). Returns 1 on a fresh press made in
   range and facing the door — the frame main.c starts the transition on. */
int  the_pit_south_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way (a debug jump) can still ensure a
   Circle held through the transition does not fire on the arrival frame. */
void the_pit_arm(void);

#endif
