#ifndef TOMB_H
#define TOMB_H

#include <stdint.h>
#include "render.h"

/* The Tomb: Chapter 3's fourth room, through the WEST door of the Incinerator
   Room — the door that room's header has been listing as "drawn, not built"
   since it landed.

   THE SHAPE. One square chamber, 4200 by 4200, flat and single-storey: the
   collision proxy found exactly ONE floor plane, at y=0 over the whole
   footprint, x[-4199,0] z[0,4200]. The vault is at y=-800 throughout, the
   Incinerator Room's low ceiling rather than the Up Down Maze's 1800, and the
   visual mesh agrees with the proxy on it — both stop there — so it is the
   height the player actually sees.

   >>> WHAT IS IN IT IS NINE FREE-STANDING BLOCKS OF LOCULI, ON A 3x3 GRID. <<<
   Each block is 600 square and solid, walled on all four sides by the proxy
   (36 of the 40 walls are these; the other four are the chamber's outer walls),
   and the aisles between and around them are 600 wide:

       blocks at x[-3600,-3000] [-2400,-1800] [-1200,-600]
              by z[  600, 1200] [ 1800, 2400] [ 3000, 3600]

   That is a grid on the same 600 pitch as the Up Down Maze's blocks, and it is
   NOT the same kind of room — see the view-distance note in the .c, which is
   the one place that difference has to be argued rather than assumed.

   THE DOORS. Three are drawn, and TWO of them are wired up:

     EAST   x=0      z[1400,1600] y[-400,0]  -> Incinerator Room, west door
     WEST   x=-4200  z[2600,2800] y[-400,0]  -> Room of Arms, east door
     north  z=4200   x[-2200,-2000] y[-400,0] not built

   The north one is drawn and nothing else: no sign, no trigger. It reads as a
   sealed door, which is what it is until the room behind it exists, and wiring
   it up is the block of #defines in the .c plus the STEP 6 edits in
   tools/ADDING_A_ROOM.txt.

   >>> THE TWO LIVE DOORS TAKE OPPOSITE MIRRORS AND THAT IS NOT A DETAIL. <<<
   Both are YZ-plane doors, but the east one is approached from -X (wall 39,
   nx=-4096) and the west one from +X (wall 38, nx=+4096), so their signs take
   mirror=1/-11 and mirror=0/+11 respectively. The .c passes the pair at each
   call site rather than deriving it, so the difference is visible where the two
   are drawn. A door whose sign reads backwards has this wrong.

   NO STOREY TEST ON ANY OF THEM, which is the normal case and not an omission.
   This room is flat, so the walkable surface is a function of XZ — the
   assumption every trigger in the engine makes — and a plain Manhattan test is
   correct. The Up Down Maze's two doors are the only ones in the game that are
   not in that position.

   THREE TEXTURES, AND THE ROOM OWNS NONE OF THEM. Cobblestone, the loculus and
   the catacomb inner door are all already registered by src/catacombs_entry.c
   in TEXBANK_CATACOMBS, so this room holds compile-time headers (TIM_SLOT) and
   calls that module's three NARROW uploaders — the Up Down Maze's and the
   Incinerator Room's arrangement, one texture wider. It costs zero texmgr
   registrations and zero permanent RAM; see tomb_load_assets().

   Its exports live in assets/catacombs/, beside the other three Chapter 3
   rooms'. */

void tomb_load_assets(void);     /* startup: headers only, no CD, no regs */
void tomb_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void tomb_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void tomb_init(void);            /* collision + floor zones + spawn */
void tomb_draw(RenderContext *ctx);

/* Arrival through the east door: standing just inside it, facing west into the
   chamber. */
void tomb_spawn_east(void);

/* Arrival through the west door, back from the Room of Arms: standing just
   inside it, facing east down the aisle between the west wall and the western
   column of loculus blocks. */
void tomb_spawn_west(void);

/* One frame of a door's Circle test. `lock` is main's usual suppression (a menu
   is up, a cutscene owns the camera). Returns 1 on a fresh press made in range
   and facing that door — the frame main.c starts the transition on. Both share
   one body in the .c; only the coordinates and the edge-state differ. */
int  tomb_east_door_triggered(int lock);
int  tomb_west_door_triggered(int lock);

/* Arm every interaction in the room — BOTH doors' edge states, not just the one
   the player arrived through. Called by the spawn above; exported so a
   caller that places the player some other way (a debug jump) can still ensure a
   Circle held through the transition does not fire on the arrival frame. */
void tomb_arm(void);

#endif
