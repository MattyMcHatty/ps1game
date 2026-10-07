#ifndef THE_HEAD_H
#define THE_HEAD_H

#include <stdint.h>
#include "render.h"

/* The Head: Chapter 3's THIRTY-SECOND room, through the Neck's east door.

   THE SHAPE. A wide hall, x[0,2400] z[0,3000] as drawn, flat at y=0, walls
   and ceiling at y=-800. Its EAST wall (x=2400) is a relief of Lamashtu
   figures ("lamashtu pearl") from z=200 to z=2800, two registers high. The
   collision proxy's east wall stands at x=2210, so the player stops short of
   the relief. A catacomb inner door at z[1400,1600] in each end wall:

     WEST  x=0     -> the Neck, its east door
     EAST  x=2400  -> drawn and sealed: no sign, no trigger, nothing behind it
                      yet

   THE ARM SWITCHES: eight stone arms reaching out of the relief, one over
   each Lamashtu elbow in its bottom register, in one straight north-south
   row (src/arm_switch.h). Drawn only; nothing is wired to them yet.

   Nothing is seeded in it.

   THREE TEXTURES: cobblestone and the inner door, borrowed through
   src/catacombs_entry.c's narrow uploaders, and the relief, owned here. The
   arm switches' own texture comes through src/arm_switch.c.

   Its exports live in assets/catacombs/, beside the other Chapter 3 rooms'. */

void the_head_load_assets(void);     /* startup: three headers, one deferred registration */
void the_head_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void the_head_upload_textures(void); /* room entry: pure LoadImage from RAM      */
void the_head_init(void);            /* collision + floor zone + spawn + switches */
void the_head_draw(RenderContext *ctx);

/* Arrival through the west door, from the Neck - the only arrival, and
   the_head_init()'s spawn: just inside it, facing east toward the relief. */
void the_head_spawn_west(void);

/* One frame of the west door's Circle test. `lock` is main's usual
   suppression. Returns 1 on a fresh press made in range and facing the door. */
int  the_head_west_door_triggered(int lock);

/* Arm every interaction in the room (the door). Called by the spawn above. */
void the_head_arm(void);

#endif
