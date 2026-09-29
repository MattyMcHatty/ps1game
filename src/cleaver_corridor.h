#ifndef CLEAVER_CORRIDOR_H
#define CLEAVER_CORRIDOR_H

#include <stdint.h>
#include "render.h"

/* The Cleaver Corridor: Chapter 3's NINTH room, at the top of the LADDER in the
   North Chamber's east alcove — the first room in the game reached by a ladder
   rather than a door or a stair.

   THE SHAPE: one straight corridor, x[0,4800] z[-300,300], flat at y=0 under a
   vault at y=-800. The collision proxy is that box and nothing else — four walls,
   one floor face — so this is the Room of Heads' single-floor case.

   THE SHAFT. At the WEST end the corridor opens onto a shaft x[-600,0], from the
   vault at -800 straight down to a black floor at y=+1200 (nine untextured polys).
   The LADDER runs down its east wall, the x=0 face, z[-100,100], from the
   corridor floor to the bottom. The shaft is OUTSIDE the proxy: wall 3 runs along
   x=0 the full height of the corridor and holds the player 195 back from the
   edge, so it cannot be fallen into, only looked down. Climbing it is the ladder
   transition (src/ladder_anim.h), triggered from the top.

   The rooms' coordinate spaces are independent. In the North Chamber the ladder
   climbs x=2800 from its gallery (y=-1000) up into the vault; here it arrives
   through the floor. The two ends of the one ladder, joined by the transition.

   THE WAYS OUT:

     WEST   the ladder, x=0 z=0   -> the North Chamber, onto the gallery at the
                                     foot of its ladder. A YZ-plane prompt at the
                                     shaft's edge approached from +X: mirror=0.
     SOUTH  z=-300 x[3800,4000]   a catacomb inner door at the far end. DRAWN AND
                                  NOTHING ELSE: no prompt, no trigger. Whatever
                                  is behind it is a later room.

   THREE TEXTURES, ALL BORROWED: cobblestone and the inner door through
   src/catacombs_entry.c's narrow uploaders, the ladder through
   src/north_chamber.c's. The room registers nothing.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void cleaver_corridor_load_assets(void);     /* startup: three headers, no CD   */
void cleaver_corridor_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void cleaver_corridor_upload_textures(void); /* room entry: pure LoadImage from RAM */
void cleaver_corridor_init(void);            /* collision + floor zone + spawn   */
void cleaver_corridor_draw(RenderContext *ctx);

/* Arrival up the ladder, from the North Chamber: on the corridor floor just off
   the shaft's edge, facing east down the corridor. The only arrival. */
void cleaver_corridor_spawn_ladder(void);

/* One frame of the ladder's Circle test. `lock` is main's usual suppression.
   Returns 1 on a fresh press made in range of the shaft's edge, facing it — the
   frame main.c starts the climb down on. */
int  cleaver_corridor_ladder_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above. */
void cleaver_corridor_arm(void);

#endif
