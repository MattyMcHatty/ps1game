#ifndef ROOM_OF_TORSOS_H
#define ROOM_OF_TORSOS_H

#include <stdint.h>
#include "render.h"

/* The Room of Torsos: Chapter 3's EIGHTEENTH room, through the H Corridor's
   south door (z=0, x[2000,2200], the foot of its east leg).

   THE SHAPE. The Room of Arms' octagon again: 2586 across the flats, x/z
   [-1293,1293], flat and single-storey under a y=-800 vault, one door in the
   east face. On its floor stand THREE PILES OF TORSOS, each a low mound that
   rises to ONE pointed tip 300 high (drawn):

       south-west   x[-914,-321]  z[-696,-99]   tip (-725,-304)
       south-east   x[107,725]    z[-696,-99]   tip (536,-497)
       north        x[-321,536]   z[298,895]    tip (107,497)

   The proxy walls each pile in at y[-299,0] (walls 0-9 and 18-31), so the
   player walks round them; room_of_torsos_init() makes all of those walls
   shoot-over, so a shot, and the Creeps rising off the tips, pass above.

   THE CRIB. The chapter's FIFTH crib encounter stands in the corner opposite
   the door — the north-west one, against the chamfer that runs between the
   south-west pile and the north pile — its long axis along that wall. It pours
   its Creeps from FOUR places: its own centre and the tip of each of the three
   piles. Its solved flag is this room's own bit —
   crib_room_solved(STATE_ROOM_OF_TORSOS) — separate from the other four rooms'
   (src/crib.h).

   THE DOOR. ONE, and it is wired up:

     EAST   x=1293  z[-107,107] y[-400,0]  -> H Corridor, south door

   A door in the YZ plane approached from -X (wall 10 runs x=1293 with
   nx=-4096), so TEXT_PLANE_YZ with mirror=1 — the Room of Legs' east door
   exactly. No storey test: the room is flat.

   THREE TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the catacomb inner door
   come through src/catacombs_entry.c's narrow uploaders. `torsos` is new art
   only this room draws, registered DEFERRED on the ROOM OF ARMS' page and
   palette (x640 y0, CLUT (672,501)) — the page the Rooms of Heads, Legs and
   Bones already time-share. None of those rooms is ever drawn with another, and
   each room's uploader puts its own art back on entry. See
   room_of_torsos_load_assets().

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void room_of_torsos_load_assets(void);     /* startup: one deferred reg + headers */
void room_of_torsos_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void room_of_torsos_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void room_of_torsos_init(void);            /* collision + floor zone + spawn        */
void room_of_torsos_draw(RenderContext *ctx);

/* Arrival through the east door, from the H Corridor, and the only arrival
   there is: just inside it, facing west across the room to the crib. */
void room_of_torsos_spawn_east(void);

/* One frame of the east door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door — the frame
   main.c starts the transition on. */
int  room_of_torsos_east_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void room_of_torsos_arm(void);

#endif
