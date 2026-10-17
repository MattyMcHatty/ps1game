#ifndef DESCENT_TO_HELL_H
#define DESCENT_TO_HELL_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   DESCENT TO HELL - Chapter 4's first room, and a CUTSCENE end to end.
   =========================================================================
   No door leads here. The Seal's scene ends in full white (src/seal.h), and
   main.c carries the player in under that white - no door animation and no
   red LOADING screen, the Lamashtu Arena -> Seal hand-off's silent transition
   with the loading frame painted white instead of black.

   THE SHAPE. The visual mesh is a box 3600 square, y[-7000, 2000]: the four
   walls are the hell backdrop and the floor, far below, is lava. There is NO
   COLLISION MESH and the player never has control here; the zero-wall
   src/descent_to_hell_mesh_collision.c exists only because the room packer
   expects one.

   THE TUBE is a prop of its own (assets/underworld/descent_tube.smd, \TEXUNDW\
   DSCTUBE.SMD): a 12-sided lava shaft, radius 400, y[-6000, 0], centred on the
   room's origin. It is drawn where its model puts it and SPINS about its own
   axis, fast. It is read on room entry and handed back on the way out
   (descent_to_hell_unload_tube, from main.c's load_area_geometry), the
   catacomb doors' terms.

   THE BEATS, all owned here, one clock:

     FADE IN   2 s up out of the white the Seal ended on.
     THE FALL  the camera starts at the very top of the tube, looking straight
               down it, and falls at ONE STEADY SPEED until full white: it
               reaches the tube's mouth as DEATH AWAITS YOU... comes up and is
               still falling, ~150 above the lava, when the screen goes white.
               The tube spins the whole way.
     THE LINES seven sentences, one at a time, each at its own place on the
               screen, each fading quickly in as the one before fades out. The
               last, DEATH AWAITS YOU ALL!, arrives in the centre as the white
               starts to come up, and stays.
     TO WHITE  the screen fades to white with the last line still up, turning
               it deep red so it reads against the white.
     WHITE     the last line held 4 s in it, then trial_end_start(): the thank-you screen,
               which dissolves this room (white, red line and all) to its
               purple. The sign-off is where the build ends.

   Its two textures are its own, both 4bpp: LAVA.TIM at x576 y0 on Lamashtu's
   CLUT line (the Seal's terms) and HELLBG.TIM at x640 y0 on the gaol door's.

   THE WIND (CD track 10, CDAUDIO_WIND_TRACK) plays once from the first frame
   of the fade in; the sign-off brings its own track in over it. */

void descent_to_hell_load_assets(void);     /* startup: two registrations      */
void descent_to_hell_load_geometry(void);   /* ROOM ENTRY: the mesh + the tube */
void descent_to_hell_unload_tube(void);     /* every transition: the tube back */
void descent_to_hell_upload_textures(void); /* room entry: pure LoadImage      */
void descent_to_hell_init(void);            /* camera at the top, the clock    */
void descent_to_hell_draw(RenderContext *ctx);
void descent_to_hell_reset(void);           /* new game / title                */

/* The scene's tick, in main's cutscene branch. The whole visit is one. */
void descent_to_hell_update(void);

#endif
