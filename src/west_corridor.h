#ifndef WEST_CORRIDOR_H
#define WEST_CORRIDOR_H

#include "render.h"

/* West Corridor: the upper-floor passage running west out of Reception and
   north to the door at the top of the Rear Gate's ramp — the first room that
   joins the house to the garden chain without going through the front of the
   mansion.

   Modelled in its OWN coordinate space, like every other room. Its floor is a
   single plane at y=0 and its ceiling at y=-600, so the standing eye height is
   the interior rooms' -189 even though the room is, in fiction, up on
   Reception's second floor. Nothing here is multi-level: the collision
   generator reported four floor planes and every one of them is y=0, so
   west_corridor_mesh_collision.c sets multi_level = 0 and one FLOOR_FLAT zone
   covers the lot.

   THE LAYOUT, in three parts (bounds x[-2700,0] z[-325,2000]):

     SOUTH ARM  the east-west corridor along the room's south edge,
                x[-2100,0] z[-325,325]. The EAST DOOR is at its far end.
     WEST ARM   the north-south corridor up the room's west edge,
                x[-2700,-2100] z[-325,2000]. The NORTH DOOR is at its far end.
     INNER ROOM the large space east of the west arm, x[-2044,-6]
                z[398,1986], reached ONLY through the 300-wide opening in the
                double-thickness wall at x=-2100/-2044, z[1050,1350]. It shares
                no boundary with the south arm: walls at z=325 and z=398 run
                the full width between them. A BREAKABLE FAT DOOR fills that
                opening (x=-2072, z=1200, rotated 90° so it is thin in X) — see
                fatdoors_init() — so the inner room is shut until the player
                smashes it with the crucifaxe.

   TWO DOORS, both connected:

     NORTH  the double_door in the z=2000 wall, x[-2550,-2250], so centre
            x=-2400. Collision wall 7 runs across the opening with nz=-4096, so
            it is approached from -Z — from inside this room — which for an XY
            sign is mirror=0. It leads to the Rear Gate's south door, the one at
            the top of that room's gravel ramp (rear_gate.h's DOOR paragraph:
            it was drawn and walled off and led nowhere until now). Same
            transition as Delivery Area <-> Kitchen Dining: DOOR_PANEL_OUTER.

     EAST   the wd_dr leaf in the x=0 wall, z[-108,108], so centre z=0.
            Collision wall 0 runs across it with nx=-4096: approached from -X,
            which for a YZ sign is mirror=1. It leads to Reception's UPPER-floor
            north-west door — the one reception.c calls NDOOR. Single wooden
            door, so DOOR_PANEL_WOOD.

            IT IS LOCKED FROM THE RECEPTION SIDE, and this is the side that
            unlocks it — the 2F Hall's east door mechanic exactly (see
            FLAG_HALL_2F_DOOR). The sign here reads "Press O to unlock"
            until the first press, which plays SFX_UNLOCK and does NOT
            transition; after that it reads "to enter" and behaves normally.
            Reception's NDOOR reads the same flag and shows the red "Locked
            from the other side" until it is set.

   NO ROOM MUSIC. cdaudio_stop() on arrival, the Rear Gate's and the Garden
   Stairs' arrangement rather than Reception's — stopped rather than merely not
   started, so a title-screen load or a debug level-select jump (neither of
   which passes through a door transition's own stop) also arrives in silence.

   FOG AND CULL ARE THE CONSERVATORY'S: 350/1500 with the same dark (20,15,10)
   interior wash, which is also what Reception and the Master Bedroom run, so
   the step through either door does not change the weather.

   SEVEN mesh textures and it OWNS NONE — every one belongs to another module,
   so this room registers nothing with texmgr and does no CD access at all
   (garden_courtyard.c's arrangement). Four are resident from startup and the
   other three come in through NARROW single-texture uploads on the owning
   modules. See the slot table in west_corridor.c. */
void west_corridor_load_assets(void);     /* startup: capture tpage/clut only */
void west_corridor_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void west_corridor_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void west_corridor_init(void);            /* set collision/floor zones + spawn */
void west_corridor_draw(RenderContext *ctx);

/* The two doors. "north" is the double door to the Rear Gate's ramp, "east" the
   single door back into Reception's upper floor. */
void west_corridor_doors_arm(void);        /* seed both doors' Circle edge state */
int  west_corridor_ndoor_triggered(void);  /* north double door -> Rear Gate    */
int  west_corridor_edoor_triggered(void);  /* east single door  -> Reception 2F */

/* Spawn the player just inside a door after arriving through it. */
void west_corridor_spawn_north(void);
void west_corridor_spawn_east(void);

/* The EAST door's lock, shared with Reception's NDOOR, is FLAG_WEST_CORR_DOOR
   in src/player.h — clear until the player unlocks it from this side. A
   GameFlag rather than a plain global so it rides in SaveData.flags and comes
   back unlocked after a save/load; FLAG_HALL_2F_DOOR, the mechanic this copies,
   is stored the same way. Read it with game_flag(), not through this header. */

#endif
