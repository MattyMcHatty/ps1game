#ifndef CATACOMB_DOORS_H
#define CATACOMB_DOORS_H

#include <stdint.h>
#include "render.h"

/* THE CATACOMB DOORS: the two leaves standing in the mouth of the catacomb
   facade at the north end of Outside Catacombs. Modelled on src/hatch_doors.c —
   the same "pair of .smd leaves, loaded on room entry, solid off their own
   vertices" prop — with ONE difference that runs through the whole file:

   >>> THERE IS NO CLIP YET. <<< These are STATIC. Every leaf is drawn and
   collided on its .smd's bind pose, which is the shut pose. The scaffolding an
   animated pair needs (a per-leaf vertex block, a bounding box scanned off that
   block every call rather than baked into a constant) is already here and is
   deliberately NOT collapsed into two literal boxes, so adding the .pva clips
   later is a matter of filling in cd_verts() and a playback clock — not of
   rewriting the draw and the collision. See tools/ANIMATING_A_3D_MODEL.txt and
   hatch_doors.c when that day comes; the note in catacomb_doors_collide() says
   what has to be revisited about the push direction at the same time.

   >>> THAT WAS TRUE UNTIL THE DOORS OPENED, AND THE WAY THEY OPEN IS WHY IT
   COST SO LITTLE. <<< They SLIDE apart — the left leaf due west, the right leaf
   due east, by CD_SLIDE_FULL each — rather than swinging on a hinge. A slide is
   one integer added to every vertex's X, which the draw can do through the GTE's
   translation and the collision can do with a `+ off` on two comparisons; a
   swing would have needed the .pva clips this file was scaffolded for, a back
   face on each leaf, and the push-direction rewrite the collision note below
   still describes. See src/catacomb_open.h for the scene that drives it.

   ---- WHERE THEY SIT: NOWHERE. THEY ARE ALREADY IN WORLD SPACE --------------
   >>> DO NOT ADD A TRANSLATION. <<< The Hatch's pair is baked about the pit's
   centre and carried into the world by a matrix, so that file has three
   HATCH_DOORS_* constants and applies them by hand in the collision as well as
   through the GTE. These two were exported IN PLACE, at the coordinates they
   occupy in the room:

     "Outside Catacombs - Door Left.smx"   x[-500,   0]  y[-910,0]  z[3787,3847]
     "Outside Catacombs - Door Right.smx"  x[   0, 500]  y[-910,0]  z[3787,3847]

   Those are room coordinates, not model-local ones — the room's ground is y=0
   and its facade runs z[3786,4637], which is exactly where these land. So the
   draw uses the plain view matrix with no world matrix composed onto it, and
   the collision compares the vertices against cam_x/cam_z directly. A
   translation added "to place them" would move them off the doorway.

   ---- WHAT THEY REPLACED ----------------------------------------------------
   The lamashtu tablet used to be a flat plane at z=3786 textured into the
   facade. In the current mesh that plane has moved back to z=3850 and lost its
   material — it is the 15 untextured polys the tex map now reports, the dark
   backing behind the doorway — and the tablet ART lives on these two leaves
   instead. That is why the room mesh no longer references 'lamashtu tablet'
   while src/outside_catacombs.c still OWNS the registration and the upload of
   LMSHTBLT.TIM: the room pays for the texture, these leaves are the only thing
   that draws it. Do not "clean up" slot 5 out of outside_catacombs.c.

   The leaves have NO REAR FACE (z=3847 is open — check_model_winding reports 14
   open edges, and its inside-out verdict is meaningless on an open shell). They
   are backed against that z=3850 plane and nothing can see behind them while
   they are shut. A swing will expose the gap, so the clip has to come with a
   back face on the leaves.

   ---- TEXTURE: THE ROOM'S SLOT 5, AND IT NEEDS THE 128 WINDOW ---------------
   Both leaves are UV'd against `lamashtu tablet`, which Outside Catacombs
   streams to the brick_wall page (x768 y0) on entry. The tpage/clut smxlink
   baked in are ignored and TIM_TPAGE_LMSHTBLT / TIM_CLUT_LMSHTBLT used instead
   — the valve handle's and the hatch doors' trick, for the same reason.

   Their UVs RUN PAST 128 (u to 156, v to 228) because the art tiles: a face
   spanning u[28,156] is one full wrap of a 128x128 texture. They therefore
   depend on the 128x128 texture window outside_catacombs_draw already sets for
   the whole frame. Drawing them anywhere that window is not set gives garbage.

   ---- MEMORY: LOADED PER ROOM ----------------------------------------------
   Two .smd files, 1164 bytes each, which read_file rounds to one 2048-byte
   sector apiece — 4 KB while the player is in the room and nothing at all
   anywhere else. Loaded from outside_catacombs_load_geometry() and freed from
   the top of main.c's load_area_geometry(), which runs on every transition, the
   same arrangement hatch_doors.h explains at length. Nothing here is resident
   at startup. */

void catacomb_doors_load(void);     /* ROOM ENTRY: read both .smd            */
void catacomb_doors_unload(void);   /* leaving: free both. Safe if not loaded */

/* ---- THE SLIDE ------------------------------------------------------------
   How far apart the two leaves have been drawn, in world units, applied
   OUTWARD: the left leaf moves by -slide in X and the right by +slide. 0 is the
   shut pose the .smd files are baked in.

   CD_SLIDE_FULL is one leaf's own width, which is exactly what it takes to
   clear the doorway: the leaves span x[-500,0] and x[0,500] against a mouth at
   x[-450,450], so at 500 each the opening is completely uncovered and the
   leaves are standing over the facade mass either side of it. They are drawn
   against its face rather than inside it — they sit at z[3787,3847] and the
   facade front is at z=3786 — so what the player sees is two slabs of tablet
   art that have slid onto the stone, which is the read that was wanted.

   catacomb_doors_init() is the one to call on ROOM ENTRY, after the flag
   restore: it poses the pair shut or fully open to match FLAG_ASAG_DEAD, so a
   player coming back to this room finds the doors the way the scene left them.
   catacomb_doors_set_slide() is for the scene itself, a frame at a time. */
#define CD_SLIDE_FULL  500

void    catacomb_doors_init(void);                  /* pose from FLAG_ASAG_DEAD */
void    catacomb_doors_set_slide(int32_t slide);
int32_t catacomb_doors_slide(void);

void catacomb_doors_draw(RenderContext *ctx);

/* Push the player out of whichever leaf they are standing in. Area-gated to
   Outside Catacombs, so the shared reception collision routine calls it
   unconditionally like every other prop family in there. */
void catacomb_doors_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

#endif
