#ifndef HEAVENS_GATE_H
#define HEAVENS_GATE_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   HEAVENS GATE - Chapter 4's eleventh room
   =========================================================================
   Behind the Lava Maze's SOUTH-EAST door.

   THE SHAPE. One storey of grass at y=0, x[0,5599] z[0,5400]: an octagon
   inside 600-tall proxy walls that are not drawn - the lawn stands open in
   the chapter's sky (src/underworld_sky.h), centred on it. Against the south
   wall, a brick GATEHOUSE: two gate leaves facing north onto the lawn, and
   behind them a flight of stairs rising south to y=-1000. The gate is shut
   and the proxy boxes the gatehouse in, so the stairs are scenery for now.
   One flat floor zone. No lava, no props, no enemies.

   TWO DOORS:
     - NORTH-WEST: a greenhouse door, on the diagonal, distorted (it flares
       as it rises). It leads to the LAVA MAZE's south-east door, a wooden
       door. Per direction (the user's choice, Nov 2026: the transition is
       the door on the side being left): out of here DOOR_PANEL_GREENHOUSE,
       out of the Lava Maze DOOR_PANEL_WOOD.
     - EAST: a catacomb inner door, 150 tall, at x=5600. It leads nowhere
       yet - no prompt and no trigger until its room exists.

   THE ARRIVAL. One: through the north-west door from the Lava Maze, just
   inside it facing south-east, towards the gate (heavens_gate_spawn_nw, the
   init's spawn and where a debug jump lands).

   THE MUSIC is the chapter's (CDAUDIO_UNDERWORLD_TRACK, main.c's Chapter 4
   block).

   Textures: seven, five owned - see heavens_gate.c. Chapter rules (the fog,
   the sky): src/underworld.h. */

void heavens_gate_load_assets(void);     /* startup: five registrations + TIM_SLOTs */
void heavens_gate_load_geometry(void);   /* ROOM ENTRY: mesh + the sky              */
void heavens_gate_upload_textures(void); /* room entry: pure LoadImage              */
void heavens_gate_init(void);            /* collision, floor, spawn                 */
void heavens_gate_draw(RenderContext *ctx);

/* Seed the door's Circle edge state "held", so a press carried in through
   the transition cannot fire on the arrival frame. */
void heavens_gate_doors_arm(void);

/* The NORTH-WEST door, to the Lava Maze: a fresh Circle within reach and
   facing it. The edge state is kept current while locked. */
int  heavens_gate_nw_door_triggered(int lock);

/* Arrival through the north-west door: just inside it, facing south-east. */
void heavens_gate_spawn_nw(void);

#endif
