#ifndef ROOM_OF_GUTS_H
#define ROOM_OF_GUTS_H

#include <stdint.h>
#include "render.h"

/* The Room of Guts: Chapter 3's THIRTIETH room, through the Throat's south
   door (z=0, x[-100,100]).

   THE SHAPE. The Room of Arms' octagon again: 2586 across the flats, x/z
   [-1293,1293], flat and single-storey under a y=-800 vault, one door in the
   east face. The floor is paved with guts (151 floor polys) and nothing stands
   up off it: the collision proxy is the bare eight walls.

   THE CRIB. The chapter's SIXTH and LAST crib encounter, in the corner furthest
   to the player's RIGHT as they walk in: they arrive facing west, so right is
   north and the far right corner is the north-west one — the Room of Torsos'
   cot position in the same octagon. It pours its Creeps from FOUR places: its
   own centre and the other three corners of the room. Its solved flag is this
   room's own bit — crib_room_solved(STATE_ROOM_OF_GUTS) — and it is the one
   that lights the Nursery's north-west mirror cot, the last of the six. With
   all six lit, the Gula Tablet stands raised off the Nursery's west door on
   the next entry there (src/nursery.c).

   THE DOOR. ONE, and it is wired up:

     EAST   x=1293  z[-107,107] y[-400,0]  -> the Throat, south door

   A door in the YZ plane approached from -X (wall 0 runs x=1293 with
   nx=-4096), so TEXT_PLANE_YZ with mirror=1 — the Room of Torsos' east door
   exactly. No storey test: the room is flat.

   THREE TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the catacomb inner door
   come through src/catacombs_entry.c's narrow uploaders. `guts` is new art only
   this room draws, 4bpp, registered DEFERRED on the ROOM OF ARMS' page (x640
   y0, covering x[640,672) only) with a CLUT of its own at (624,502) — the gula
   tablet's and the baby names' terms. See room_of_guts_load_assets().

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void room_of_guts_load_assets(void);     /* startup: one deferred reg + headers */
void room_of_guts_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void room_of_guts_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void room_of_guts_init(void);            /* collision + floor zone + spawn        */
void room_of_guts_draw(RenderContext *ctx);

/* Arrival through the east door, from the Throat, and the only arrival there
   is: just inside it, facing west into the room. */
void room_of_guts_spawn_east(void);

/* One frame of the east door's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range and facing the door — the frame
   main.c starts the transition on. */
int  room_of_guts_east_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void room_of_guts_arm(void);

#endif
