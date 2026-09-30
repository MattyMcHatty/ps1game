#ifndef CRUCIFIX_CORRIDOR_H
#define CRUCIFIX_CORRIDOR_H

#include <stdint.h>
#include "render.h"

/* The Crucifix Corridor: Chapter 3's TENTH room, through the single door at the
   north-east corner of the Up Down Maze's LOWER storey.

   THE SHAPE is the name: a cross laid flat. One corridor runs east from the
   door, x[0,3600] z[-300,300], and an arm crosses it at x[2400,3000], running
   z[-1500,1500]. The corridor's stub past the arm, x[3000,3600], is the head of
   the cross — an ALCOVE at the east end, 600 square. Everything is flat at y=0
   under a vault at y=-800. The collision proxy is exactly that outline: twelve
   walls and three floor faces of one plane, so this is the single-floor case.

   THE DOORS. Three are drawn and ONE is wired up:

     WEST   x=0     z[-100,100]   -> the Up Down Maze, lower storey, at its
                                     north-east door. YZ plane, approached from
                                     +X: mirror=0.
     north  z=1500  x[2600,2800]  the end of the arm. Not built.
     south  z=-1500 x[2600,2800]  the other end. Not built.

   The two unbuilt ones are drawn and nothing else: no sign, no trigger. They
   read as sealed doors until the rooms behind them exist.

   THE SCONCE. One LIT sconce stands in the centre of the east alcove, at
   (3300,0) — the light at the end of the corridor. Its point light widens the
   view distance around it (src/sconce.h), so the alcove glows out of the fog
   while the corridor between is still dark; the draw loop carries the
   Catacombs Entry's light-aware cull and fog for exactly that.

   TWO TEXTURES, BOTH BORROWED: cobblestone and the inner door through
   src/catacombs_entry.c's narrow uploaders, plus the sconce's own page through
   sconce_upload_texture(). The room registers nothing.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void crucifix_corridor_load_assets(void);     /* startup: two headers, no CD    */
void crucifix_corridor_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void crucifix_corridor_upload_textures(void); /* room entry: pure LoadImage from RAM */
void crucifix_corridor_init(void);            /* collision + floor zones + spawn */
void crucifix_corridor_draw(RenderContext *ctx);

/* Arrival through the west door, from the Up Down Maze: just inside the door,
   facing east down the corridor toward the lit alcove. The only arrival. */
void crucifix_corridor_spawn_west(void);

/* One frame of the west door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range, facing the door. Call
   it every frame and pass `lock` in, so the edge state stays current. */
int  crucifix_corridor_west_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above. */
void crucifix_corridor_arm(void);

#endif
