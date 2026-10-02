#ifndef ROOM_OF_BONES_H
#define ROOM_OF_BONES_H

#include <stdint.h>
#include "render.h"

/* The Room of Bones: Chapter 3's FOURTEENTH room, through the door at the back
   of the Meat Plant's west alcove (x=-3300, z[3200,3400]) — the middle of that
   room's west side.

   THE SHAPE. The Room of Arms' octagon again: 2586 across the flats, x/z
   [-1293,1293], flat and single-storey under a y=-800 vault, one door in the
   east face. In the middle of it stands ONE MOUND OF BONES, an octagon in plan:

       x[-535,535] z[-497,497], the flats at x=+-535 (z[-298,298]) and
       z=+-497 (x[-321,321]), drawn 150-200 high round its rim and rising to a
       peak at y=-550 over its middle (x[-107,107] z[-99,99])

   The proxy walls the mound in at y[-600,0] (walls 0 and 9-15), so what the
   player walks is a ring round it, 758 wide between the mound's flats and the
   octagon's. The drawn mound is under the proxy's 600 everywhere, so
   room_of_bones_init() makes all of those walls shoot-over: a shot, and the
   Creeps coming down off the mound's top, pass above.

   THE CRIB. The chapter's FOURTH crib encounter stands on the room's WEST side,
   in the gap between the mound and the west wall, directly across the mound
   from the door, laid across the ring so it closes it: the player walks round
   either side of the mound to reach it. It pours its Creeps from its own centre,
   from the DOOR, and from the TOP OF THE MOUND. Its solved flag is this room's
   own bit — crib_room_solved(STATE_ROOM_OF_BONES) — separate from the other
   three (src/crib.h).

   THE DOOR. ONE, and it is wired up:

     EAST   x=1293  z[-107,107] y[-400,0]  -> Meat Plant, west-alcove door

   A door in the YZ plane approached from -X (wall 1 runs x=1293 with
   nx=-4096), so TEXT_PLANE_YZ with mirror=1 — the Room of Legs' east door
   exactly. No storey test: the room is flat.

   THREE TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the catacomb inner door
   come through src/catacombs_entry.c's narrow uploaders. `bones` is new art only
   this room draws, registered DEFERRED on the ROOM OF ARMS' page and palette
   (x640 y0, CLUT (672,501)) — the page the Rooms of Heads and Legs already
   time-share. None of the four rooms is ever drawn with another, and each
   room's uploader puts its own art back on entry. See room_of_bones_load_assets().

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void room_of_bones_load_assets(void);     /* startup: one deferred reg + headers */
void room_of_bones_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void room_of_bones_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void room_of_bones_init(void);            /* collision + floor zone + spawn        */
void room_of_bones_draw(RenderContext *ctx);

/* Arrival through the east door, from the Meat Plant, and the only arrival
   there is: just inside it, facing west at the mound. */
void room_of_bones_spawn_east(void);

/* One frame of the east door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door — the frame
   main.c starts the transition on. */
int  room_of_bones_east_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void room_of_bones_arm(void);

#endif
