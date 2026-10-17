#ifndef SEAL_H
#define SEAL_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   THE SEAL - Chapter 3's THIRTY-FIFTH room, and the last thing in the build.
   =========================================================================
   No door leads here. When Lamashtu dies her arena fades out around a rooted
   player (THE VOID, src/lamashtu_boss.h) and main.c carries them in under the
   black - no door animation and no red LOADING screen.

   THE SHAPE. A black void with the seal lying in it: the visual mesh is ONLY
   the seal's 107 polys, x[-871,871] z[-931,1138] at y=0, and there is no fog
   and no distance cull, so it can be seen from anywhere in the room. The
   collision proxy is a floor 6000 square, x[-3000,3000] z[-3000,3000], and
   four walls 400 tall.

   THE BEATS, all owned here:

     BLACK     1 s more of the dark the arena ended on, still ROOTED - the
               other second was the arena's. The player arrives in the SOUTH-
               EAST corner whatever corner of the arena they died in, turned
               to face the seal - from there nothing at all is on screen, so
               the facing is the only clue which way to walk.
     FADE IN   6 s up from black. Walls and floor are live from its first
               frame and the player can walk again.
     READY     a "Press O" sign floats over the centre of the seal, the doors'
               green and their fade, and a tap within SL_TRIGGER_RADIUS (seal.c)
               starts the scene.
     THE SCENE (a cutscene: no control, no HUD), ~17.5 s, timed to its two
               clips - the lines over the emerge, the blasts over the explode:
               the camera rises from the player to look down on the seal, then
               circles it, faster and faster. The lines of misc/seal light map
               light red and orange, flowing out from the bird like a fuse;
               fully lit, five explosions go off across the seal and the
               screen fades to white.
     WHITE     the room holds full white for half a second, and then
               seal_leaving() has main.c load DESCENT TO HELL under it - no
               door, no LOADING screen, the white held across the load. That
               room ends in the sign-off screen now (src/descent_to_hell.h).

   ONE TEXTURE, AND THE ROOM OWNS IT: seal.tim, 4bpp at x576 y0 on the crib's
   page with Lamashtu's palette line (640,502). Only her arena draws either of
   those, and it puts both back on entry.

   SILENT throughout: no music line (main.c's Catacombs block says so).

   Its exports live in assets/bosses/Lamashtu/, beside the arena's. */

void seal_load_assets(void);     /* startup: one registration + its header     */
void seal_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena   */
void seal_upload_textures(void); /* room entry: pure LoadImage from RAM        */
void seal_init(void);            /* collision + floor zone + spawn + BLACK     */
void seal_draw(RenderContext *ctx);
void seal_reset(void);           /* new game / title: forget everything        */

/* Free play: the fade-in clock and the centre's prompt. `lock` is main's
   usual suppression. */
void seal_update(int lock);

int  seal_roots_player(void);    /* 1 during BLACK: turn and attack, no walking */
int  seal_cutscene(void);        /* 1 from the scene's first frame to the end   */
int  seal_leaving(void);         /* 1 once the white has been held: go to Ch 4  */
void seal_cutscene_update(void); /* the scene's tick, in main's cutscene branch */

#endif
