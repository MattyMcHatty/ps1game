#ifndef HEROS_PLINTH_H
#define HEROS_PLINTH_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   HERO'S PLINTH - Chapter 4's fifth room
   =========================================================================
   Between Sharur's Trial's NORTH-WEST door and Wingscape's WEST door.

   THE SHAPE. x[-1900,2100] z[-2100,2100] (the collision proxy), one storey:
   a volcanic floor at y=0 with its four corners cut at 45 degrees, two lava
   pools in the west half (x[-1700,-500], z~+-1100..1700) that burn
   (src/lava.h, the mask from tools/gen_lava_floors.py), and on the WEST wall
   DESTINY'S DOOR's frame (x[-1900,-1700] z[-700,700], 1120 tall) at the top
   of a 70-high ramp, x[-1700,-1500]. The frame is solid: DESTINY'S DOOR is
   its recessed centre, which takes the door's texture once the jewel
   ceremony has run with all three mini-bosses dead (see the bottom of this
   file). Under the Underworld's sky
   (src/underworld_sky.h), centred on the room.

   TWO DOORS:
     - NORTH-EAST, a CATACOMB inner door on the cut corner (1800,1800): to
       WINGSCAPE's WEST door (wooden over there).
     - SOUTH-EAST, a WOODEN door, a block in the corner whose west and north
       faces are both the door, centre (1800,-1800): to SHARUR'S TRIAL's
       NORTH-WEST door (wooden over there too).

   >>> THE TRANSITION DEPENDS ON THE DIRECTION, NOT THE DOORWAY. <<< Each
   side's transition is the door the player walks up to on the side being
   left: here -> Wingscape plays DOOR_PANEL_CATACOMB, Wingscape -> here
   DOOR_PANEL_WOOD; here <-> Sharur's Trial is wooden both ways. See the
   Chapter 4 block of tools/ADDING_A_ROOM.txt.

   No enemies, no props. THE MUSIC is the chapter's (main.c's Chapter 4
   block). Chapter rules (the fog, the sky): src/underworld.h. */

void heros_plinth_load_assets(void);     /* startup: five registrations       */
void heros_plinth_load_geometry(void);   /* ROOM ENTRY: mesh + the sky        */
void heros_plinth_upload_textures(void); /* room entry: pure LoadImage        */
void heros_plinth_init(void);            /* collision, floors, default spawn  */
void heros_plinth_draw(RenderContext *ctx);

/* The doors: NORTH-EAST to Wingscape, SOUTH-EAST to Sharur's Trial. A fresh
   Circle within reach and facing it. THE EDGE STATE IS KEPT UP TO DATE EVEN
   WHILE LOCKED, so a Circle held across a menu closing does not read as a
   fresh press. */
void heros_plinth_doors_arm(void);
int  heros_plinth_ne_door_triggered(int lock);
int  heros_plinth_se_door_triggered(int lock);

/* Arrivals, one per door. The south-east one is also the default spawn. */
void heros_plinth_spawn_ne(void);
void heros_plinth_spawn_se(void);

/* DESTINY'S DOOR (src/heros_plinth.c, tools/DESTINYS_DOOR_CHECKLIST.txt).
   The frame's three jewels resolve the mini-boss flags - Adapa's (blue,
   right), Hadad's (green, top), Gula's (yellow, left). Circle at the frame's
   centre plays the JEWEL CEREMONY if any of them is dead: their spirits fly
   in from the sky, each lights its jewel, and - all three - Destiny's Door
   fades up in the recess and then offers "Press O to enter" (COMING SOON).
   Saved as FLAG_DESTINY_CEREMONY; once only.

   heros_plinth_frame_update   the normal branch, every frame: the prompt's
                               Circle (and the jewels' pulse).
   heros_plinth_scene_active   1 while the ceremony owns the camera: main.c's
                               cutscene branch, menu lock and HUD suppression.
   heros_plinth_scene_update   that branch, every frame.
   heros_plinth_scene_reset    new game / load: drop a half-played one. */
void heros_plinth_frame_update(int lock);
int  heros_plinth_scene_active(void);
void heros_plinth_scene_update(void);
void heros_plinth_scene_reset(void);

#endif
