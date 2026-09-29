#ifndef ROOM_OF_HEADS_H
#define ROOM_OF_HEADS_H

#include <stdint.h>
#include "render.h"

/* The Room of Heads: Chapter 3's EIGHTH room, through the WEST door of the North
   Chamber — the one on the gallery's west arm, y=-1000 over that room's ground.

   THE SHAPE. The Room of Arms' octagon again: 2586 across the flats, x/z
   [-1293,1293], flat and single-storey under a y=-800 vault, one door in the
   east face. Where that room had a screen wall, this one has FOUR PILES OF
   HEADS, one per quadrant, each an L of heaped heads ~160 high (drawn) around a
   corner of the central cross:

       SE  x[321,914]   z[-895,-298]      NE  x[321,914]   z[298,895]
       SW  x[-914,-321] z[-895,-298]      NW  x[-914,-321] z[298,895]

   The proxy walls them in at y[-185,0] (walls 0-36): the player walks round
   them, never over them, but they are LOW, and room_of_heads_init() makes them
   shoot-over so a shot passes above. What is left is a cross-shaped floor — a corridor
   z[-298,298] east-west and x[-321,321] north-south — plus the ring between the
   piles and the octagon's outer walls (37-44).

   >>> THE DOOR CORRIDOR IS NARROW. <<< From the door to x=535 the corridor is
   the gap between the two eastern piles, z[-298,298]; with the chapter's 195
   standoff that leaves a walkable band about 200 wide down its middle. It is
   straight and the spawn is on its centre line, so nothing snags, but a prop
   or an enemy placed in it will block it.

   THE CRIB. The chapter's second crib encounter stands in the gap between the
   two WESTERN piles, closing the west arm of the cross, and pours its Creeps
   from its own centre and from the tops of the piles either side of it. Its
   solved flag is this room's own bit — crib_room_solved(STATE_ROOM_OF_HEADS) —
   separate from the Room of Arms' (src/crib.h).

   THE DOOR. ONE, and it is wired up:

     EAST   x=1293  z[-107,107] y[-400,0]  -> North Chamber, west door

   A door in the YZ plane approached from -X (wall 38 runs x=1293 with
   nx=-4096), so TEXT_PLANE_YZ with mirror=1 — the Room of Arms' east door
   exactly. No storey test: the room is flat.

   THREE TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the catacomb inner door
   come through src/catacombs_entry.c's narrow uploaders. `heads` is new art only
   this room draws, registered DEFERRED on the ROOM OF ARMS' page and palette
   (x640 y0, CLUT (672,501)) — the two rooms are never drawn together and each
   room's uploader puts its own art back on entry. See room_of_heads_load_assets().

   Its exports live in assets/catacombs/, beside the other seven Chapter 3
   rooms'. */

void room_of_heads_load_assets(void);     /* startup: one deferred reg + headers */
void room_of_heads_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void room_of_heads_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void room_of_heads_init(void);            /* collision + floor zone + spawn        */
void room_of_heads_draw(RenderContext *ctx);

/* Arrival through the east door, from the North Chamber, and the only arrival
   there is: just inside it, facing west down the corridor between the piles. */
void room_of_heads_spawn_east(void);

/* One frame of the east door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door — the frame
   main.c starts the transition on. */
int  room_of_heads_east_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void room_of_heads_arm(void);

#endif
