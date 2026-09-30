#ifndef SLIDING_BARS_ROOM_H
#define SLIDING_BARS_ROOM_H

#include <stdint.h>
#include "render.h"

/* The Sliding Bars Room: Chapter 3's ELEVENTH room, through the north door at
   the end of the Crucifix Corridor's cross arm.

   THE SHAPE. A square, x[0,4200] z[0,4200], flat at y=0 under a vault at
   y=-800, filled with a grid of 600-square stone blocks on a 600 pitch, the
   gaps between them corridors 600 wide. Some of those gaps are closed by BARS:
   a pair of barred screens 100 apart, each a thin partition in the collision
   proxy. Loculi are cut into the blocks' faces and four incinerator panels are
   set into the walls at y[-300,-100] (the four gate buttons). The proxy is 58 walls and ONE floor face,
   so this is the single-floor case.

   THE MESH'S BARS ARE STATIC: drawn and colliding, and nothing moves them.
   THE FOUR GATES MOVE. They are Bars props (src/bars.h), each slid two cells
   along its line by one of the four incinerator panels in the mesh; see THE
   GATES in sliding_bars_room.c for the grid, the table and the save.

   THE DOORS. Three are drawn and ONE is wired up:

     SOUTH-WEST   z=0     x[200,400]   -> the Crucifix Corridor, at the north
                                          door of its cross arm. XY plane,
                                          approached from +Z: mirror=1.
     east, south  x=4200  z[200,400]   not built
     east, north  x=4200  z[3800,4000] not built

   The two unbuilt ones are drawn and nothing else: no sign, no trigger. They
   read as sealed doors until the rooms behind them exist.

   FIVE TEXTURES, ALL BORROWED: cobblestone, the inner door and the loculus
   through src/catacombs_entry.c's narrow uploaders, the incinerator panel
   through src/incinerator.c's and the bars through src/bars.c's. The room
   registers nothing.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void sliding_bars_room_load_assets(void);     /* startup: five headers, no CD   */
void sliding_bars_room_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void sliding_bars_room_upload_textures(void); /* room entry: pure LoadImage from RAM */
void sliding_bars_room_init(void);            /* collision + floor zones + spawn */
void sliding_bars_room_draw(RenderContext *ctx);

/* Arrival through the south-west door, from the Crucifix Corridor: just inside
   the door, facing north into the room. The only arrival. */
void sliding_bars_room_spawn_south(void);

/* One frame of the south-west door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range, facing the door. Call
   it every frame and pass `lock` in, so the edge state stays current. */
int  sliding_bars_room_south_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above. */
void sliding_bars_room_arm(void);

/* One frame of the four gates: their slides and their panels. Call every frame
   in the room with main's `lock`, so the edge state stays current. */
void sliding_bars_room_update(int lock);

/* Re-place the gates from the saved state. main calls it after world_enter and
   savegame_apply_pending, since a load installs the state AFTER the area init. */
void sliding_bars_room_apply_flags(void);

/* THE GATE STATE: bit i = gate i+1 is at its MOVED spot. Saved as
   SaveData.sb_gates. reset_gates puts all four back to their start spots; it
   takes effect the next time the room is entered (every entry re-places them),
   so another room can call it. A new game calls it too. */
int  sliding_bars_room_gates(void);
void sliding_bars_room_set_gates(int bits);
void sliding_bars_room_reset_gates(void);

#endif
