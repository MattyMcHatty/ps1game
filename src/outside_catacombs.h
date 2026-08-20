#ifndef OUTSIDE_CATACOMBS_H
#define OUTSIDE_CATACOMBS_H

#include <stdint.h>
#include "render.h"

/* Outside Catacombs: the hedge-walled approach to the catacomb mouth, north of
   Fountain Square through the gate in that square's north hedge.

   Modelled in its OWN coordinate space, like every other room here. Its ground
   is flat at y=0 — all sixteen of the generated floor planes are — and its gate
   is on the SOUTH wall where Fountain Square's is on the north. Only the door
   pairing links the two, so no offset is applied anywhere.

   Bounds x[-4396,4399] z[-2000,4637], and that is the largest footprint in the
   game: roughly 8800 x 6600, twice Fountain Square's each way. The layout is a
   long central avenue running north from the gate, opening into side lawns east
   and west, and ending at the catacomb facade:

     PERIMETER  1100-tall hedge around the whole footprint, broken only by the
                south gate. Fifty-six collision walls, all of them y[-500,0]
                proxies for it and for the hedge masses inside.
     AVENUE     the walk north from the gate at x[-750,750], widening into the
                lawns at z>1714.
     FACADE     the con_tile mass at z[3786,4637], reaching up to y=-1141 — the
                catacomb entrance itself, drawn but not yet enterable.
     TABLET     the lamashtu tablet set into that facade at x[-450,450],
                z=3786, hanging from y=-905 down to the ground.
     FLOWERS    the poison-flower bed, a ground decal at y=0 spanning
                x[-750,2504] z[0,1714]; art only, no collision of its own.

   ONE gate is modelled and it is connected:

     SOUTH   the grdn_gte leaf at z=-2000, x[-450,450], y[-600,0]. The far side
     WALL    of the gate in Fountain Square's north hedge, and the only way in
             or out. Collision wall 17 runs across it with nz = +4096, so the
             walkable side is +Z and the player approaches from inside the room.

   Rendered the same way as Fountain Square (per-poly tex map + one 128 texture
   window + purple outdoor fog at the same near/far), and it reuses the Garden
   Courtyard's texture uploads wholesale — four of this mesh's seven textures
   are the courtyard's and a fifth is the conservatory's.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "Outside
   Catacombs.smx" and "Outside Catacombs mesh.smx" are both in that
   subdirectory, as Fountain Square's are;
   gen_outside_catacombs_tex_map.py defaults to that path and the two conversion
   commands in tools/ADDING_A_ROOM.txt STEP 4 need it spelled out. */
void outside_catacombs_load_assets(void);     /* startup: register streamed textures */
void outside_catacombs_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void outside_catacombs_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
/* Just this room's flower bed, for a room that draws poison_flower_base but not
   the tablet or the facade — Maze One. Run the courtyard's uploader first. */
void outside_catacombs_upload_flowers(void);
void outside_catacombs_init(void);            /* set collision/floor zones + spawn */
void outside_catacombs_draw(RenderContext *ctx);

/* The south-wall gate back to Fountain Square. */
void outside_catacombs_gate_arm(void);        /* seed the Circle edge state */
int  outside_catacombs_gate_triggered(void);  /* 1 on a fresh Circle press in range */

/* Spawn just inside the south gate — the only arrival. */
void outside_catacombs_spawn_south(void);

#endif
