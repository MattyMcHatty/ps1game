#ifndef INCINERATOR_ROOM_H
#define INCINERATOR_ROOM_H

#include <stdint.h>
#include "render.h"

/* Incinerator Room: Chapter 3's third room, through the SOUTH door on the LOWER
   floor of the Up Down Maze.

   THE SHAPE. One L in plan, flat, single storey, and after the Up Down Maze that
   is worth saying plainly: every floor plane in the collision proxy is at y=0,
   the vault is at y=-800 throughout, and nothing here is stacked over anything
   else. The long hall runs x[-4200,600] z[-3600,0]; a short ALCOVE opens off its
   north-east corner at x[600,1000] z[-1800,-600]. That alcove is the only thing
   the room's outline does that a rectangle would not.

   >>> AND -800 IS A LOW VAULT, half the Up Down Maze's 1800. <<< It is read off
   the VISUAL mesh, which agrees with the proxy here — both stop at -800 — so it
   is the height the player actually sees. Anything hung from this ceiling has
   611 units between it and a standing eye, not 1611.

   THE DOORS. Two are drawn and BOTH are now wired up:

     NORTH  z=0      x[200,400]     y[-400,0]  -> Up Down Maze, lower floor
     WEST   x=-4200  z[-1800,-1600] y[-400,0]  -> the Tomb, east door

   The west door was drawn and nothing else until the Tomb landed - no sign, no
   trigger, reading as a sealed door, which is what it was until the room behind
   it existed. Wiring it up was the block of #defines in the .c plus the STEP 6
   edits in tools/ADDING_A_ROOM.txt, and it is the worked example for anyone
   doing the same to one of the chapter's other sealed doors.

   NEITHER DOOR TAKES A STOREY TEST, and the west one's far side does not need
   one either - the Tomb is flat. The note below is about the NORTH door's far
   side, which is the one exception in the game.

   >>> THE NORTH DOOR'S NEIGHBOUR NEEDS A STOREY TEST AND THIS SIDE DOES NOT.
   <<< The far end of this doorway is the Up Down Maze's south-lower door, which
   sits under that room's south walkway and therefore takes the Y test every
   trigger up there takes (UDM_STOREY_REACH in src/up_down_maze.c). This side is
   a flat room, the walkable surface IS a function of XZ, and a plain Manhattan
   trigger is correct — the same as every door in the game bar that room's two.

   TWO TEXTURES, AND THE ROOM OWNS NEITHER. Cobblestone and the catacomb inner
   door are both already registered by src/catacombs_entry.c in TEXBANK_CATACOMBS,
   so this room holds compile-time headers (TIM_SLOT) and calls that module's two
   NARROW uploaders — the Up Down Maze's arrangement exactly. It costs zero
   texmgr registrations and zero permanent RAM; see incinerator_room_load_assets().

   Its exports live in assets/catacombs/, beside the other two Chapter 3 rooms'. */

void incinerator_room_load_assets(void);     /* startup: headers only, no CD, no regs */
void incinerator_room_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void incinerator_room_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void incinerator_room_init(void);            /* collision + floor zones + spawn */
void incinerator_room_draw(RenderContext *ctx);

/* Arrival through the north door: standing just inside it, facing south down
   the hall. This is also incinerator_room_init()'s DEFAULT spawn, so a
   title-screen load or a debug jump lands here. */
void incinerator_room_spawn_north(void);

/* Arrival through the west door, back out of the Tomb: standing just inside it
   on the +X side, facing east up the hall. main.c overrides the default with
   this one, keyed on current_area. */
void incinerator_room_spawn_west(void);

/* One frame of the north door's Circle test. `lock` is main's usual suppression
   (a menu is up, a cutscene owns the camera). Returns 1 on a fresh press made in
   range and facing the door — the frame main.c starts the transition on. */
int  incinerator_room_north_door_triggered(int lock);

/* The same, for the WEST door into the Tomb. The two doors are 4500 apart in x
   against a 500 reach and can never both be in range, so they need no veto
   between them - but main.c still folds their results into the machine's
   `lock`, which is the room's one real ordering rule. */
int  incinerator_room_west_door_triggered(int lock);

/* One frame of the Incinerator's BUTTON, plus the tick of the three-grind cycle
   a press starts and the log line it owes when it stops. `lock` is main's usual
   suppression, with the north door's result folded into it. Returns 1 on the
   frame a press was consumed. The machine's other interaction — the conveyor —
   is a camera-locked board and belongs to src/incinerator_panel.h. */
int  incinerator_room_machine_update(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way (a debug jump) can still ensure a
   Circle held through the transition does not fire on the arrival frame. */
void incinerator_room_arm(void);

#endif
