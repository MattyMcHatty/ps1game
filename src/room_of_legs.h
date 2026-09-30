#ifndef ROOM_OF_LEGS_H
#define ROOM_OF_LEGS_H

#include <stdint.h>
#include "render.h"

/* The Room of Legs: Chapter 3's TWELFTH room, through the NORTH of the two doors
   in the Sliding Bars Room's east wall (x=4200, z[3800,4000]).

   THE SHAPE. The Room of Arms' octagon again: 2586 across the flats, x/z
   [-1293,1293], flat and single-storey under a y=-800 vault, one door in the
   east face. In it lies ONE PILE OF LEGS, a C open to the east, ~200 high
   (drawn), wrapping round the room's west half:

       the spine     x[-1104,-725]  z[-526,526]       (the back of the C)
       the arms      north z[298,1094] and south z[-1094,-298], each running
                     east to its END at x~914, the tip at (725,+-507)

   The proxy walls in only the pile's INNER edge, at y[-599,0] (walls 3-25), so
   what the player walks on is a cross: the corridor z[-304,304] from the spine
   (x=-724) to the door, and a north-south arm x[-321,321] out to z=+-696. The
   ring between the pile's back and the octagon's walls is unreachable, sealed at
   the pile's two ends by a pair of funnel walls to the door's chamfers. The
   drawn pile is only 200 tall, so room_of_legs_init() makes all of those walls
   shoot-over: a shot, and the Creeps rising off the pile ends, pass above.

   THE CRIB. The chapter's THIRD crib encounter stands at the far end of the
   corridor, in front of the pile's spine, its long axis along Z and its back to
   the legs. It pours its Creeps from its own centre and from the two ENDS of the
   pile, north and south of the player as they come in. Its solved flag is this
   room's own bit — crib_room_solved(STATE_ROOM_OF_LEGS) — separate from the
   Room of Arms' and the Room of Heads' (src/crib.h).

   THE DOOR. ONE, and it is wired up:

     EAST   x=1293  z[-107,107] y[-400,0]  -> Sliding Bars Room, north-east door

   A door in the YZ plane approached from -X (wall 1 runs x=1293 with
   nx=-4096), so TEXT_PLANE_YZ with mirror=1 — the Room of Heads' east door
   exactly. No storey test: the room is flat.

   THREE TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the catacomb inner door
   come through src/catacombs_entry.c's narrow uploaders. `legs` is new art only
   this room draws, registered DEFERRED on the ROOM OF ARMS' page and palette
   (x640 y0, CLUT (672,501)) — the page the Room of Heads' `heads` already
   time-shares. None of the three rooms is ever drawn with another, and each
   room's uploader puts its own art back on entry. See room_of_legs_load_assets().

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void room_of_legs_load_assets(void);     /* startup: one deferred reg + headers */
void room_of_legs_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void room_of_legs_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void room_of_legs_init(void);            /* collision + floor zone + spawn        */
void room_of_legs_draw(RenderContext *ctx);

/* Arrival through the east door, from the Sliding Bars Room, and the only
   arrival there is: just inside it, facing west down the corridor to the crib. */
void room_of_legs_spawn_east(void);

/* One frame of the east door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door — the frame
   main.c starts the transition on. */
int  room_of_legs_east_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void room_of_legs_arm(void);

#endif
