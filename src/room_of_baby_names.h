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

   THE DOORS. Eight are drawn and SEVEN are wired up:

     WEST ("John")      x=-1293 z[-107,107]  -> Gaol Cells, east door
     NORTH-WEST (Benj)     (-914,  914)      -> STATE_DEAD_END_BENJ
     NORTH (Matthew)       (   0, 1293)      -> STATE_DEAD_END_MATTHEW
     EAST (Christof)       (1293,    0)      -> STATE_DEAD_END_CHRISTOF
     SOUTH-EAST (Luke)     ( 914, -914)      -> STATE_DEAD_END_LUKE
     SOUTH-WEST (Mark)     (-914, -914)      -> STATE_DEAD_END_MARK
     SOUTH (Antoni)        (   0,-1293)      -> STATE_THROAT, its north door

   The west door is in the YZ plane approached from +X (wall 6 runs x=-1293
   with nx=+4096), so TEXT_PLANE_YZ with mirror=0 - the Gaol Cells' west
   door's terms. The other six are one table in the .c, and their signs are
   drawn on a fixed yaw, because four of the faces are diagonal. The blank
   plate (north-east) will lead to The Neck; that room does not exist yet, so
   its door is drawn and sealed: no sign, no trigger.

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

/* Arrival through the west door, from the Gaol Cells: just inside it, facing
   east toward the plinth. room_of_baby_names_init()'s default. */
void room_of_baby_names_spawn_west(void);

/* Arrival back out of a Dead End or the Throat: just inside the named door
   that room is behind, facing into the room. `from` is its GameState;
   anything without a named door leaves the spawn alone. */
void room_of_baby_names_spawn_from_named_door(int from);

/* One frame of the six named doors' Circle test (five Dead Ends and the
   Throat). Returns the GameState behind the door that was pressed (fresh
   press, in range, facing it), or 0 for none. */
int  room_of_baby_names_named_door_triggered(int lock);

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
