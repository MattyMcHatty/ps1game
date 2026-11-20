#ifndef RIBCAGE_H
#define RIBCAGE_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   RIBCAGE - Chapter 4's eighth room
   =========================================================================
   Behind Hell Hath No Fury's SOUTH-EAST door.

   THE SHAPE. One storey of volcanic ground at y=0, x[0,7800] z[-1200,2400]:
   a corridor in from the west door (z[0,1200]), the body of the room among
   the ribs (Sharur's rocks art), lava, and a corridor out to the east door.
   Every proxy wall stands 798 tall. It hangs in the chapter's sky
   (src/underworld_sky.h), centred on it. One flat floor zone. The lava
   burns like every Chapter 4 floor's (its ROOMS line in
   tools/gen_lava_floors.py). No props.

   ENEMIES (world.c's world_seed_room): a NESSIE in each of the four lava
   pools - towards the north in the two that reach the north rim, towards the
   south in the two that reach the south - and a GOLEM in front of the east
   double door. It blocks nothing until that door leads somewhere.

   THREE DOORS, two at the ends of the corridors and one in the north
   wall:
     - WEST: a wooden door (wd_dr, resident), distorted - square to y=-400,
       its top rising lopsided to a peak. It leads back to HELL HATH NO
       FURY's south-east door, a wooden door too: DOOR_PANEL_WOOD both ways
       (the user's choice, Nov 2026).
     - EAST: a double door (a copy of the Delivery Area's double_door).
       Leads nowhere yet - no prompt and no trigger until its room exists.
     - NORTH: a catacomb inner door. Leads nowhere yet either.

   THE ARRIVAL. One: through the west door, just inside it facing east
   (ribcage_spawn_w, also the init's default and where a debug jump lands).

   THE MUSIC is the chapter's (CDAUDIO_UNDERWORLD_TRACK, main.c's Chapter 4
   block).

   Textures: six, one owned (the double door's copy) - see ribcage.c. Chapter rules (the fog, the
   sky): src/underworld.h. */

void ribcage_load_assets(void);     /* startup: TIM_SLOTs only        */
void ribcage_load_geometry(void);   /* ROOM ENTRY: mesh + the sky     */
void ribcage_upload_textures(void); /* room entry: pure LoadImage     */
void ribcage_init(void);            /* collision, floor, spawn        */
void ribcage_draw(RenderContext *ctx);

/* Seed every door's Circle edge state "held", so a press carried in through
   the transition cannot fire on the arrival frame. */
void ribcage_doors_arm(void);

/* The WEST door, to Hell Hath No Fury: a fresh Circle within reach and
   facing it. The edge state is kept current while locked. */
int  ribcage_w_door_triggered(int lock);

/* Arrival through the west door: just inside it, facing east. */
void ribcage_spawn_w(void);

#endif
