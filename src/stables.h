#ifndef STABLES_H
#define STABLES_H

#include <stdint.h>
#include "render.h"

/* The Stables: the walled yard WEST of the Rear Gate, through the gate in that
   room's west hedge — the second of the Rear Gate's three modelled gates to be
   connected, and the last of them that leads anywhere new.

   Modelled in its OWN coordinate space, like every other room here. Its floor is
   at y=0 where the Rear Gate's lawn is, and its connecting gate is on the EAST
   wall where the Rear Gate's is on the west. Only the gate pairing links the
   two, so no offset is applied anywhere.

   Bounds x[-3400,100] z[-1600,1600] — 3500 x 3200, and 945 prims. The mesh is
   53 KB, well inside the arena Maze One's 117 KB sizes, so nothing there had to
   change.

   THE LAYOUT, east to west:

     GATE      a grdn_gte leaf at x=100, z[-400,400], y[-600,0], in the YZ plane,
     ALCOVE    at the end of a short alcove x[-100,100] cut through the east
               hedge. Collision FLOOR 1 is that alcove, x(-100,100) z(-400,400),
               which is what fixes the gate's centre at z=0. Wall 10 runs across
               the opening at x=100 with nx = -4096, so the walkable side is -X
               and the player approaches from inside this room — which for a YZ
               sign is mirror=1 and a sign on the x-11 side. That is the
               OPPOSITE hand from the Rear Gate's side of the same gate.
     YARD      the open ground, x[-3400,-100] z[-1600,1600], inside a 500-tall
               perimeter of hedge (east) and brick wall (north, south, west).
               All of it at y=0.
     BLOCKS    the two stable buildings, 600 tall and boxed by collision walls
               0-5 and 13-14: x[-2799,-700] z[400,1200] and the same x span at
               z[-1200,-400]. Both are solid — there is no interior — and the
               800-wide lane between them at z[-400,400] is the way through to
               the west end. Their long faces carry the STABLE GLYPHS.
     WEST END  the strip x[-3400,-2799], with the GREENHOUSE DOOR in the brick
               wall at x=-3400, z[-133,133]. It LEADS SOMEWHERE now: the
               Greenhouse (src/greenhouse.h) is on the other side of it, and the
               two rooms' meshes line up exactly - greenhouse_x = stables_x +
               3500, greenhouse_z = stables_z - 100. Collision wall 15 still runs
               the full west side and is not touched: the door is shut as far as
               collision is concerned, and it is the trigger that lets the player
               through, the same arrangement as the east gate. The greenhouse
               building itself is still DRAWN beyond the wall at x[-4010,-3400]
               and 1100 tall, so the door reads as leading somewhere from across
               the yard.

   ONE FLAT FLOOR. Both of the generator's planes are genuinely at y=0 (the yard
   and the gate alcove), so unlike the Rear Gate this room needs no FLOOR_RAMP
   and one zone over the collision bounds covers both.

   MUSIC: FOUNTAIN SQUARE'S TRACK, restarted on arrival — the user asked for it
   explicitly. Played in main's STATE_LOADING branch rather than on the gate
   trigger so every route in gets it (the gate, a title-screen load, a debug
   level-select jump). The Rear Gate next door is SILENT, so unlike the
   Catacombs/Square pair there is no question of carrying a track across the
   gate: the transition's own cdaudio_stop has already killed whatever the Rear
   Gate had going, which in that room is only ever Hadad's.

   FOG AND CULL are FOUNTAIN SQUARE'S exactly — 575/2500 in the garden's purple,
   which is also what the Rear Gate, Maze One and Maze Two run. Also the user's
   explicit ask.

   >>> THE GARDEN-WEST VRAM BANK STARTS HERE. <<< This room and the Greenhouse
   behind it are reached ONLY through the Rear Gate's west gate, and nothing else
   is drawn while the player is in them. That makes the whole room-art half of
   VRAM theirs to re-lay-out rather than something to squeeze into, and
   tools/VRAM_MAP_GARDEN_WEST.txt is the map of it: the fixed regions and the
   globals that must survive (HUD, weapons, collectibles, door panels, and the
   Rafflesia and Mushroom Head sprite sets), this room's eight textures, and what
   is left free for the Greenhouse.

   EIGHT mesh textures, of which this room owns FOUR — the highest owned count of
   any room in the game, and the point of having a bank at all. The other four
   are the garden chain's and cost nothing (hedge, grdn_gte, grss_gs through the
   Garden Courtyard's uploader, brick_wall with them via the Garden Stairs').
   Note that this is the FIRST room to draw brick_wall and grss at once: their
   own TIMs are both at x768 y0, which is why 'grss' resolves to the grss_gs
   clone here as it does everywhere else in the garden. See the slot table in
   stables.c and KNOWN_STREAM_PAIRS in tools/vram_map.py.

   >>> ITS EXPORTS LIVE IN assets/garden/, NOT assets/. <<< "Stables.smx" and
   "Stables mesh.smx" are both in that subdirectory, like the rest of the
   garden's; gen_stables_tex_map.py defaults to that path. */
void stables_load_assets(void);     /* startup: register this room's textures */
void stables_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void stables_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void stables_init(void);            /* set collision/floor zones + spawn */
void stables_draw(RenderContext *ctx);

/* The east-wall gate back to the Rear Gate. */
void stables_gate_arm(void);        /* seed the Circle edge state */
int  stables_gate_triggered(void);  /* 1 on a fresh Circle press in range */

/* The west-wall greenhouse door, on into the Greenhouse. Uses its own transition
   panel, DOOR_PANEL_GREENHOUSE - see src/door_anim.h. */
void stables_gdoor_arm(void);
int  stables_gdoor_triggered(void);

/* Arriving through the gate: just inside it, facing west into the yard. */
void stables_spawn_east(void);
/* Arriving back out of the Greenhouse: just inside the west door, facing east.
   Both spawns arm BOTH openings. */
void stables_spawn_west(void);

#endif
