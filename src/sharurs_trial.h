#ifndef SHARURS_TRIAL_H
#define SHARURS_TRIAL_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   SHARUR'S TRIAL - Chapter 4's second room
   =========================================================================
   Behind Hell Hath No Fury's SOUTH-WEST door (a catacomb inner door); the
   player comes in through this room's NORTH-EAST door (a wooden one).

   THE SHAPE. A lava sea, x[-3300,3300] z[-2700,2700], floor at y=0, with
   volcanic paths across it, a field of rock outcrops in the west and
   cobblestone ruins through the middle, under the Underworld's sky
   (src/underworld_sky.h), which is centred on it. The collision proxy's rim
   and outcrops stop the player; one flat floor zone.

   THREE DOORS, all in the rim:
     - NORTH-EAST, wooden, on the 45-degree rim (wall 38) at (3000,2200): back
       to Hell Hath No Fury's south-west door. The ONLY door that leads
       anywhere yet.
     - NORTH-WEST, wooden, at (-2400,2400), and SOUTH, a catacomb inner door,
       at (0,-1500): no prompt and no trigger until their rooms exist.

   >>> THE TRANSITION DEPENDS ON THE DIRECTION, NOT THE DOORWAY. <<< The
   Underworld is a distorted hellscape: a door need not look the same from its
   two sides. Hell Hath No Fury -> here plays DOOR_PANEL_CATACOMB (that side
   is a catacomb inner door); here -> Hell Hath No Fury plays DOOR_PANEL_WOOD
   (this side is a wooden one). Each side's transition is the door the player
   actually walked up to. See the Chapter 4 block of tools/ADDING_A_ROOM.txt.

   No enemies, no props. THE MUSIC is the chapter's (main.c's Chapter 4
   block). Chapter rules (the fog, the sky): src/underworld.h. */

void sharurs_trial_load_assets(void);     /* startup: one registration        */
void sharurs_trial_load_geometry(void);   /* ROOM ENTRY: mesh + the sky       */
void sharurs_trial_upload_textures(void); /* room entry: pure LoadImage       */
void sharurs_trial_init(void);            /* collision, floor, default spawn  */
void sharurs_trial_draw(RenderContext *ctx);

/* The NORTH-EAST door, back to Hell Hath No Fury. A fresh Circle within
   reach and facing it. THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED,
   so a Circle held across a menu closing does not read as a fresh press when
   the lock lifts. */
void sharurs_trial_doors_arm(void);
int  sharurs_trial_ne_door_triggered(int lock);

/* Arrival through the north-east door (also the default spawn). */
void sharurs_trial_spawn_ne(void);

#endif
