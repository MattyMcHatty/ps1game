#ifndef ROOM_OF_ARMS_H
#define ROOM_OF_ARMS_H

#include <stdint.h>
#include "render.h"

/* The Room of Arms: Chapter 3's fifth room, through the WEST door of the Tomb —
   the second of the three doors that room's header has been listing as
   "drawn, not built" since it landed.

   THE SHAPE. One OCTAGONAL chamber, 2586 across the flats, flat and
   single-storey: the collision proxy found three floor faces and all three are
   at y=0, so they are three slabs of one plane and the room has one storey (see
   the floor-zone note in the .c). The vault is at y=-800 throughout, the Tomb's
   and the Incinerator Room's headroom rather than the Up Down Maze's 1800, and
   the visual mesh agrees with the proxy on it — every one of the thirteen proxy
   walls runs y[-800,0] and the drawn vaulting stops there too — so it is the
   height the player actually sees.

   It is still the SMALLEST room in the chapter, but no longer by much: 578
   primitives against the Incinerator Room's 708, the Tomb's 936 and the Up Down
   Maze's 1990.

   >>> IT WAS 290 AND THE MESH WAS SUBDIVIDED TO STOP FACES CLIPPING. <<< The
   draw loop drops any primitive with a screen vertex outside ±1023, so a face
   big enough to overrun that guard band disappears whole when the camera comes
   near it — a wall blinking out at close range, not a gradual artefact. Cutting
   the large faces up makes each one fail that test less often, at the cost of
   twice the primitives per frame. If this room is ever re-exported again, the
   thing to preserve is the face SIZE, not the count.

   >>> WHAT IS IN IT IS A WALL WITH A HOLE IN IT, AND WHAT IS BEHIND THE WALL. <<<
   A cobblestone SCREEN runs across the octagon's north-west third in four
   segments, cutting off a pocket the player can never enter:

       (630,1198) -> (-144,424) -> (-403,-155) -> (-483,-263) -> (-1293,401)

   Those are proxy walls 6, 12, 10 and 11, all full height. THE SECOND SEGMENT,
   (-144,424) to (-403,-155), IS WALLED IN THE PROXY AND NOT DRAWN IN THE VISUAL
   MESH. That is not an export miss: it is a 634-wide gap the player stands at
   and looks through. Everything else about this room is that gap.

   Behind it, filling the pocket out to the octagon's far corner at
   (-1293,1293), is a FIELD OF GRASPING ARMS — 52 near-horizontal polys lying
   between y=0 and y=-110, the room's one texture, and the only content it has.
   The far end of the field is about 2400 from the mouth of the gap, which is
   well past the room's 1600 resting view distance; see the view-distance note in
   the .c for why that is the point rather than a problem, and what the
   Helluminator does about it.

   ONE MORE SOLID: a plinth 178 wide standing off the south wall at
   x[357,535] z[-1293,-251] (proxy walls 0, 8 and 9). The player walks around it.

   THE DOOR. ONE, and it is wired up:

     EAST   x=1293  z[-107,107] y[-400,0]  -> Tomb, west door

   NO STOREY TEST ON IT, which is the normal case and not an omission. This room
   is flat, so the walkable surface is a function of XZ — the assumption every
   trigger in the engine makes — and a plain Manhattan test is correct. The Up
   Down Maze's two doors are the only ones in the game that are not in that
   position.

   >>> THREE TEXTURES, AND THIS IS THE FIRST CHAPTER 3 ROOM THAT OWNS ONE. <<<
   Cobblestone and the catacomb inner door are registered by
   src/catacombs_entry.c in TEXBANK_CATACOMBS and reached through that module's
   two narrow uploaders, the way the Up Down Maze, the Incinerator Room and the
   Tomb reach theirs. `arms` is new art that nothing else in the game draws, so
   this room registers it — deferred, in the same bank, on the same page-sharing
   terms as the rest of the chapter (x640 y0, asag.tim's CLUT). It is the 69th of
   the 72 texmgr registrations the game has; the .c says what to do instead of
   spending the seventieth.

   Its exports live in assets/catacombs/, beside the other four Chapter 3
   rooms'. */

void room_of_arms_load_assets(void);     /* startup: one deferred reg + headers */
void room_of_arms_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void room_of_arms_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void room_of_arms_init(void);            /* collision + floor zones + spawn */
void room_of_arms_draw(RenderContext *ctx);

/* Arrival through the east door, and the only arrival there is: standing just
   inside it, facing west across the chamber at the screen wall. */
void room_of_arms_spawn_east(void);

/* One frame of the east door's Circle test. `lock` is main's usual suppression
   (a menu is up, a cutscene owns the camera). Returns 1 on a fresh press made in
   range and facing the door — the frame main.c starts the transition on. */
int  room_of_arms_east_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way (a debug jump) can still ensure a
   Circle held through the transition does not fire on the arrival frame. */
void room_of_arms_arm(void);

#endif
