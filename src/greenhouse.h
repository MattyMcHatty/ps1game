#ifndef GREENHOUSE_H
#define GREENHOUSE_H

#include <stdint.h>
#include "render.h"

/* The Greenhouse: the glasshouse WEST of the Stables, through the greenhouse
   door in that room's west wall. The second and last room of the garden-west
   VRAM bank, and the end of the line — nothing leads on from it.

   Modelled in its OWN coordinate space, like every other room here, but this
   pair WAS modelled in a shared world and the offset is exact: the Stables'
   door stands at x=-3400 z=0 and this room's at x=100 z=-100, so

       greenhouse_x = stables_x + 3500,   greenhouse_z = stables_z - 100

   maps one onto the other. Nothing in the code applies that offset — the two
   rooms' collision and spawns are authored in their own spaces and only the
   door pairing links them — but it is what makes the pairing unambiguous, and
   it is worth stating because the door is the only opening either room has.

   Bounds x[-4400,100] z[-3900,1400] — 4500 x 5300, the largest footprint of any
   room in the game, and 1025 prims. The mesh is 58 KB, well inside the arena
   Maze One's 117 KB sizes, so nothing there had to change.

   >>> IT WAS 1230 PRIMS AND 69 KB UNTIL THE Aug 2026 DECIMATION. <<< The room
   was modelled on a ~200-unit grid and its shell was cut into five horizontal
   bands, which cost the frame rate rather than the eye — see STEP 6 of
   tools/DIAGNOSING_FRAME_RATE.txt for the count that led to the re-export. Two
   things in the CODE were keyed to the old mesh and had to move with it: the
   ten button primitive indices in src/greenhouse_puzzle.c, and the roof height
   the flood's water, mushrooms and camera hang from (the apex ridge at y=-1205
   is gone; nothing is above y=-900 now). Read both before the next re-export.

   THE LAYOUT, east to west:

     DOOR      the greenhouse door at x=100, z[-300,100], y[-675,0], in the YZ
               plane — the only way in or out. Collision wall 29 runs the full
               east side at x=100 with nx = -4096, so the walkable side is -X
               and the player approaches from inside this room: for a YZ sign
               that is mirror=1 and a sign on the x-11 side, the same hand as
               the Stables' east gate and the OPPOSITE of the Stables' side of
               this same door. The wall STAYS in the collision list; it is the
               trigger, not a hole, that lets the player through.
     NAVE      the long hall, x[-3100,100] z[-2600,1400], under the full-height
               glass. The BEDS stand in it: waist-high runs at y[-225,0]
               (collision walls 0-11 and 15) with the flower-bed soil and the
               planting on top.
     WEST      the brick annexe behind the nave's west wall, x[-4400,-3200]
     ANNEXE    z[-2600,-800], reached through the gap at x=-3200 z[-2000,-1400].
     NORTH     two bays off the far end at z < -2600: the wide one at
     BAYS      x[-2400,-500] z[-3900,-2700], and the slot between x=-1766 and
               x=-1099 that joins them to the nave. The standing PIPE and its
               button are in the wide bay at x~-1435 z~-3800.

   ONE FLAT FLOOR. All five of the collision generator's planes are genuinely at
   y=0, so unlike the Rear Gate this room needs no FLOOR_RAMP and one zone over
   the collision bounds covers them all.

   >>> TWO WALL HEIGHT BANDS, AND THEY ARE THE ROOM, NOT A BUG. <<< Walls 0-11
   and 15 are y[-225,0] — the waist-high beds — and everything else is
   y[-675,0], the shell. collide_wall_frontonly_y gates on Y either way, so the
   beds stop the player without stopping a shot fired over them.

   MUSIC: FOUNTAIN SQUARE'S TRACK, restarted on arrival, exactly as the Stables
   does it and for the same reasons — it is what the rest of the garden runs, and
   the transition's own cdaudio_stop has already killed whatever was playing.
   Played in main's STATE_LOADING branch rather than on the door trigger so every
   route in gets it (the door, a title-screen load, a debug level-select jump).

   FOG AND CULL are the bank's and the garden's: 575/2500 in the purple
   SKY_FOG_* colour, the same as the Stables, the Rear Gate and Fountain Square.
   The footprint is half again as large as the Stables', so the far end of the
   nave is properly out in the fog from the door — which is the point of a long
   glasshouse.

   >>> THE GARDEN-WEST VRAM BANK ENDS HERE. <<< TEN mesh textures, the most of
   any room, and FIVE of them cost nothing: grss_gs and brick_wall come through
   garden_courtyard_upload_textures(), and greenhouse, stables wood and the
   greenhouse door through stables_upload_textures(), which this room calls
   wholesale. The five it OWNS all land on pages that chain has just stamped and
   this room draws nothing from (hedge, grdn_gte, stable glyphs) or on 4bpp
   left-halves of mansion pages. Two of them are CLONES rather than new art:
   poison_flower_base and pipe both exist already but sit on x640 y0 and x768 y0,
   which is where `greenhouse` and `brick_wall` live and this room draws both.
   See the slot table in greenhouse.c and tools/VRAM_MAP_GARDEN_WEST.txt.

   >>> AND IT REGISTERS NOTHING WITH texmgr, WHICH IS ALSO NEW. <<< Its five are
   read off the CD on the transition into a scratch buffer that is freed again,
   rather than held resident from startup like every other room's art. That is a
   main-RAM decision, not a style one: the permanent heap was within ~140 KB of
   full when this room was added, and five more resident TIMs crossed the line —
   which on this machine is not an allocation failure but a malloc handing back
   stack memory, because InitHeap is given everything up to 0x801FFFF8 and the
   stack grows down into it with no guard. Read tools/HEAP_BUDGET.txt before
   adding a registration anywhere.

   >>> AND ITS CLUTS TIME-SHARE TOO, WHICH IS NEW. <<< There is exactly ONE free
   256-word CLUT run left in the whole map. Rather than spend it, the two 8bpp
   textures take the palette row of the very texture whose pixels they replace —
   flowerbed sits on hedge's page AND hedge's CLUT, cuniform pipes on grdn_gte's
   — which restores for free the moment that uploader runs again, because every
   path that puts a texture up here puts its CLUT up with it. See greenhouse.c.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "Greenhouse.smx" and
   "Greenhouse mesh.smx" are both in that subdirectory, like the rest of the
   garden's; gen_greenhouse_tex_map.py defaults to that path. */
void greenhouse_load_assets(void);     /* startup: TIM_SLOT constants only, no CD */
void greenhouse_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void greenhouse_upload_textures(void); /* room entry: borrows the Stables' set, then
                                          STREAMS its own five off the CD (the only
                                          room uploader that touches the drive) */
void greenhouse_init(void);            /* set collision/floor zones + spawn */
void greenhouse_draw(RenderContext *ctx);

/* The east-wall door back to the Stables. The only way in or out. */
void greenhouse_door_arm(void);        /* seed the Circle edge state */
int  greenhouse_door_triggered(void);  /* 1 on a fresh Circle press in range */

/* The Valve Handle on the standing pipe, and the flood that taking it starts:
   src/greenhouse_flood.c. greenhouse_init() runs its init and greenhouse_draw()
   its prompt; main.c owns the update and the camera gate. */

/* Arriving through that door: just inside it, facing west down the nave. */
void greenhouse_spawn_east(void);

#endif
