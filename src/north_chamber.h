#ifndef NORTH_CHAMBER_H
#define NORTH_CHAMBER_H

#include <stdint.h>
#include "render.h"

/* The North Chamber: Chapter 3's SEVENTH room, through the NORTH door of The Pit
   — the door in the alcove the bars seal until the ambush is dead.

   THE SHAPE, AND IT IS THE UP DOWN MAZE'S CASE RATHER THAN THE PIT'S: TWO
   WALKABLE HEIGHTS OVER ONE FOOTPRINT. A box 3600 by 3600 (x[-1200,2400]
   z[0,3600]) under a vault at y=-1800, with two small ground-floor alcoves off
   it. The collision proxy's twelve floor faces split into three kinds:

     THE GROUND    y=0 over the whole box, where the player arrives, plus
                   the WEST alcove x[-1800,-1200] z[3000,3600] and the EAST
                   alcove x[2400,3000] z[2400,3000]. In the middle of it stands
                   a CAGE of bars, x[0,1200] z[1200,2400], walled in the proxy
                   (9, 10, 17, 18) and never entered.

     THE GALLERY   y=-1000, a U round three sides and a platform in the middle,
                   all of it standing DIRECTLY OVER the ground floor:
                     south   x[-1200,2400] z[0,600]      over the arrival door
                     west    x[-1200,-600] z[600,3600]
                     east    x[1800,2400]  z[600,3600]
                     bridge  x[400,800]    z[600,1200]   south gallery -> platform
                     platform x[0,1200]    z[1200,2400]  the cage's roof
                     ladder  x[2400,2800]  z[1400,2000]  an alcove off the east
                                                         arm, with the LADDER on
                                                         its back wall at x=2800
                   No railing on any inner edge, and there is not meant to be:
                   the player walks off and lands on the ground floor.

     THE RAMP      along the north wall, x[-200,1800] z[3000,3600], rising along
                   +X from y=0 at its west foot to y=-1000 at its east head,
                   where it meets the gallery's east arm. That is the only way
                   up. Its south face (wall 22) is walled the whole way, so it is
                   walked onto from its west end and nowhere else.

   >>> BECAUSE THE GALLERY IS OVER THE GROUND, THE FLOOR ZONES' ORDER IS THE
   MECHANISM HERE and not a nicety. <<< apply_height() takes the first zone that
   is not above the player, so the gallery comes first, then the ramp, then the
   ground as a catch-all. See north_chamber_floor_zones_init().

   THE VAULT is at y=-1800 over everything: 1800 over the ground floor, 800 over
   the gallery. Anything hung from it has to say which of the two it is over.

   THE LADDER leads up through a black shaft in the vault over the ladder alcove
   (six untextured polys at y=-1800), to THE CLEAVER CORRIDOR. It is climbed, not
   walked: a Circle at its foot on the gallery starts the ladder transition
   (src/ladder_anim.h), and the player arrives at the top of the corridor's
   shaft. Coming back down lands them in the alcove, facing out of it. Gallery
   only, like the west door — though here the test is insurance rather than
   load-bearing: the nearest ground-floor standing spot, under the east arm at
   x~2205, is ~595 from the ladder in plan, outside the 500 radius.

   THE DOORS. Two, both wired up:

     SOUTH  z=0       x[1000,1200]  y[-400,0]       -> The Pit, north door.
                      On the GROUND floor, under the south gallery.
     WEST   x=-1200   z[1800,2000]  y[-1400,-1000]  -> Room of Heads, east door.
                      On the GALLERY, in the west arm's outer wall. A YZ-plane
                      door approached from +X (wall 3, nx=+4096): mirror=0.
                      It tests the storey the other way round — gallery only,
                      because the ground runs under it.

   The south door is an XY-plane door at fixed Z approached from +Z (wall 0 runs
   z=0 with nz=+4096), so TEXT_PLANE_XY with mirror=1 and its sign 11 proud of
   the wall along +Z. The Pit's north door, the far side of this wall, takes the
   opposite pair.

   >>> THE SOUTH DOOR TESTS THE STOREY, the third trigger in the game that does.
   <<< The south gallery runs z[0,600] straight over it, so a player standing on
   the gallery at z~200 is well inside the door's 500 radius in plan. Without the
   test they would open a ground-floor door through the floor under their feet.
   The sign takes the same test, or it hangs under the gallery for a player
   standing on top of it. See the Up Down Maze's UDM_WEST_Y_REACH for the first
   of these.

   FOUR TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the catacomb inner door
   come through src/catacombs_entry.c's narrow uploaders and the cage's bars
   through src/bars.c's, as The Pit takes them. `ladder` is new art this room
   owns, registered deferred like the rest of the chapter, on the INCINERATOR's
   VRAM page and palette — see north_chamber_load_assets(). The Cleaver Corridor,
   at the other end of the ladder, borrows it through
   north_chamber_upload_ladder().

   Its exports live in assets/catacombs/, beside the other six Chapter 3 rooms'. */

void north_chamber_load_assets(void);     /* startup: one deferred reg + headers */
void north_chamber_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void north_chamber_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void north_chamber_init(void);            /* collision + floor zones + spawn      */
void north_chamber_draw(RenderContext *ctx);

/* Arrival through the south door, from The Pit: on the ground floor just inside
   it, facing north into the room. */
void north_chamber_spawn_south(void);

/* Arrival through the west door, from the Room of Heads: on the gallery's west
   arm just inside it, facing east over the drop. */
void north_chamber_spawn_west(void);

/* Arrival DOWN the ladder, from the Cleaver Corridor: in the ladder alcove on
   the gallery, at its foot, facing west out of the alcove. */
void north_chamber_spawn_ladder(void);

/* One frame of the west door's Circle test: as the south door's below, but on
   the GALLERY. */
int  north_chamber_west_door_triggered(int lock);

/* One frame of the ladder's Circle test: the west door's, at the foot of the
   ladder in the east alcove. Returns 1 on the frame main.c starts the climb. */
int  north_chamber_ladder_triggered(int lock);

/* The ladder's texture alone, for the Cleaver Corridor — the other end of the
   same ladder. The narrow-uploader pattern (conservatory_upload_con_tile):
   north_chamber_upload_textures() would also stamp the bars, which that room
   does not draw. */
void north_chamber_upload_ladder(void);

/* One frame of the south door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range, on the ground floor, facing the door
   — the frame main.c starts the transition on. */
int  north_chamber_south_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void north_chamber_arm(void);

#endif
