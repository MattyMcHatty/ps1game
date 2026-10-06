#ifndef ROOM_OF_BABY_NAMES_H
#define ROOM_OF_BABY_NAMES_H

#include <stdint.h>
#include "render.h"

/* The Room of Baby Names: Chapter 3's TWENTY-THIRD room, through the catacomb
   door in the Gaol Cells' east wall (x=5000, z[500,700]).

   THE SHAPE. The Nursery's octagon again: 2586 across the flats, x/z
   [-1293,1293], flat and single-storey, its stone rising to y=-800. Cobblestone
   throughout, with a GOLD PLINTH in the middle — a lectern, x[-107,107]
   z[-99,99], its top sloping from y=-120 on the west edge up to y=-230 on the
   east — and a catacomb inner door in every one of the eight faces.

   THE NAME PLATES. Over each door, y[-500,-400], a gold plate with a name on
   it, all eight cut from one 128x128 sheet (`baby names`, a 2x4 grid):

       door         plate                door         plate
       north        Matthew              south        Antoni
       north-east   (blank)              south-west   Mark
       east         Christof             west         JOHN
       south-east   Luke                 north-west   Benj

   (read off each plate's UVs in Room of Baby Names.smx against the sheet:
   column u[0,64) is Matthew/Mark/John/Christof top to bottom, u[64,128) is
   Benj/Luke/Antoni/blank.)

   THE DOORS. Eight are drawn and ONE is wired up:

     WEST ("John")  x=-1293 z[-107,107] y[-400,0]  -> Gaol Cells, east door

   The west door is in the YZ plane approached from +X (wall 6 runs x=-1293
   with nx=+4096), so TEXT_PLANE_YZ with mirror=0 — the Gaol Cells' west
   door's terms. The other seven stand in solid octagon walls: no sign, no
   trigger.

   THE PLINTH. "Press O to read" over it, turned to face the player from any
   side, and a Circle press posts its inscription to the log
   (room_of_baby_names_update).

   FOUR TEXTURES, AND THE ROOM OWNS ONE. Cobblestone and the catacomb inner
   door come through src/catacombs_entry.c's narrow uploaders and the plinth's
   gold (`sconce`) through src/sconce.c's. `baby names` is this room's own:
   4bpp on the Room of Arms' page (x640 y0), its own CLUT at (608,502).

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void room_of_baby_names_load_assets(void);     /* startup: one deferred reg + headers */
void room_of_baby_names_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void room_of_baby_names_upload_textures(void); /* room entry: pure LoadImage from RAM  */
void room_of_baby_names_init(void);            /* collision + floor zone + spawn        */
void room_of_baby_names_draw(RenderContext *ctx);

/* Arrival through the west door, from the Gaol Cells, and the only arrival
   there is: just inside it, facing east toward the plinth. */
void room_of_baby_names_spawn_west(void);

/* One frame of the west door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range and facing the door —
   the frame main.c starts the transition on. */
int  room_of_baby_names_west_door_triggered(int lock);

/* One frame of the plinth: its Circle test and the second half of the
   inscription, which follows the first after a beat. Returns 1 if it CONSUMED
   this frame's Circle tap — the veto main.c hands the door trigger, so one
   press can never both read the plinth and leave the room. */
int  room_of_baby_names_update(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way can still ensure a Circle held
   through the transition does not fire on the arrival frame. */
void room_of_baby_names_arm(void);

#endif
