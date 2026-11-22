#ifndef CHAMBER_OF_DESTINY_H
#define CHAMBER_OF_DESTINY_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   THE CHAMBER OF DESTINY - Chapter 4's sixth room, behind Destiny's Door
   =========================================================================
   Reached ONLY through Destiny's Door on Hero's Plinth's west wall, which
   opens once the jewel ceremony has run with all three mini-bosses dead
   (src/heros_plinth.h). ONE DOOR: the same Destiny's Door, on this room's
   east wall, back to the frame.

   THE SHAPE. x[-6600,0] z[-1800,1800] (the collision proxy), two storeys:
     - a CORRIDOR from the door (x=0, z[-400,400]) west to x=-3000, 1200
       wide, marble floor;
     - the CHAMBER, x[-6600,-3000] z[-1800,1800], its floor the night sky;
     - a DAIS, x[-6000,-4800] z[-1200,1200] at y=-200, up a RAMP from the
       east, x[-4800,-4000] z[-400,400]; on it an ALTAR block, x[-5800,
       -5400] z[-600,600], 150 tall and solid;
     - the NINURTA TABLET on the west wall (x=-6600), in four quarters.
   The dais has no railing: the walls round it are Y-gated (multi_level),
   so the player walks off its edges and climbs it only by the ramp -
   Wingscape's terrace, on a small scale.

   >>> ITS OWN SKY, AND A BLACK BACKGROUND. <<< The user's brief (Nov 2026):
   built like every Underworld room but with a skybox of its own and black
   where the chapter is red. So:
     - THE SKY is "Chamber of Destiny Skybox.smx", a dome of night sky drawn
       by src/underworld_sky.c (underworld_sky_load_destiny), not the ring.
     - THE CLEAR COLOUR IS BLACK, and so is the FOG COLOUR: the chapter's
       rule is that the two are one colour, so a face fading out at the cull
       line goes to what is behind it.
     - THE FOG IS SHORTER: far 2400, not the chapter's 3500 - the user's
       order, "only far enough to see the Ninurta tablet from the top of the
       ramp" (CD_FOG_FAR in the .c says how it was measured). Near is the
       chapter's 800.

   THE TRANSITION both ways is DOOR_PANEL_DESTINY (src/door_anim.h): the door
   on each side is Destiny's Door. No enemies, no lava; FOUR LIT SCONCES
   (src/sconce.h), one 200 in from each corner of the square chamber. They
   publish NO point light here (the user's order): the fog is untouched, and
   the stands fog and cull like any other prop. THE MUSIC
   is its own (CDAUDIO_DESTINY_TRACK), not the chapter's; THE SOUND BANK is Hero's Plinth's (SND_BANK_PLINTH), so
   the door's sound, when it comes, plays from either side. */

void chamber_of_destiny_load_assets(void);     /* startup: six registrations */
void chamber_of_destiny_load_geometry(void);   /* ROOM ENTRY: mesh + the dome */
void chamber_of_destiny_upload_textures(void); /* room entry: pure LoadImage  */
void chamber_of_destiny_init(void);            /* collision, floors, spawn    */
void chamber_of_destiny_draw(RenderContext *ctx);

/* The one door, Destiny's Door, back to Hero's Plinth. A fresh Circle within
   reach and facing it; the edge state is kept up to date even while locked. */
void chamber_of_destiny_door_arm(void);
int  chamber_of_destiny_door_triggered(int lock);

/* The one arrival, through Destiny's Door - also the init's default. */
void chamber_of_destiny_spawn_door(void);

#endif
