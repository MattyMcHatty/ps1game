#ifndef HELL_HATH_NO_FURY_H
#define HELL_HATH_NO_FURY_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   HELL HATH NO FURY - Chapter 4's first playable room
   =========================================================================
   Where the Descent's white lands: Descent to Hell (src/descent_to_hell.h)
   ends in full white, and main.c carries the player in under it - the Seal ->
   Descent hand-off's silent transition, the loading frame painted white - and
   this room fades up out of that white over HHNF_T_FADE frames with the
   player rooted, exactly as the Seal fades up out of its black.

   THE SHAPE. An open octagonal platform of volcanic rock, 4200 square with its
   corners cut at 600, floor at y=0, hanging in the Underworld's sky
   (src/underworld_sky.h), which is centred on it. On the platform:
     - the SEAL, laid into the floor at the centre (seal.tim, 20 above it);
     - an L-shaped cobblestone RUIN in the south-east, x[2800,3400)
       z[800,1400), 600 tall, with an OIL DISPENSER (OD_TANK_HHNF, half full
       to start) set into the inside corner of its L;
     - a SAVE POINT at the northern tip, the middle of the north rim;
     - FOUR DOORS, one in each cut corner: catacomb inner doors south-west and
       north-east, wooden doors south-east and north-west. Distortions of the
       chapters behind the player. NOTHING IS BEHIND THEM YET - they have no
       prompt and no trigger until the rooms they lead to exist.
   No wall is drawn round the edge; the collision proxy's eight edges stop the
   player at the rim. One flat floor zone.

   THE ARRIVAL. One, the Descent's white, into the CENTRE of the room (on the
   seal), facing north - by THE OPENING DROP: under the fade the camera hangs
   3300 straight over the seal looking down, just out of the fog, and comes
   down onto it, slowing to a stop at the player's eye and pitching up to
   face north on the way, then hands over control (hell_hath_no_fury.c). A
   debug jump or a save loaded here lands in the same place with neither the
   fade nor the drop.

   THE MUSIC is the chapter's (CDAUDIO_UNDERWORLD_TRACK, main.c's Chapter 4
   block), from the first frame.

   Chapter rules (the fog, the sky): src/underworld.h. */

void hell_hath_no_fury_load_assets(void);     /* startup: one registration     */
void hell_hath_no_fury_load_geometry(void);   /* ROOM ENTRY: mesh + the sky    */
void hell_hath_no_fury_upload_textures(void); /* room entry: pure LoadImage    */
void hell_hath_no_fury_init(void);            /* collision, floor, spawn       */
void hell_hath_no_fury_draw(RenderContext *ctx);
void hell_hath_no_fury_update(int lock);
void hell_hath_no_fury_reset(void);           /* new game / title              */

/* Arm the fade up out of white and the opening drop. main.c calls it after
   hell_hath_no_fury_init() when the player arrived through the Descent's
   white hand-off, and not for a debug jump or a save load. */
void hell_hath_no_fury_from_white(void);

/* 1 while the opening drop runs: the camera is the room's, the player has no
   control at all, and the HUD and menu stay down (main.c's cutscene branch,
   which ticks hell_hath_no_fury_update in place of update_camera). */
int  hell_hath_no_fury_intro(void);

/* 1 while the room is fading up out of the white: the player can turn but not
   walk (camera.h's player_rooted), and the HUD stays down. */
int  hell_hath_no_fury_fading(void);

/* The oil tank's tap (OD_TANK_HHNF): a fresh Circle in range of the ruin's
   inside corner and facing it pours and posts the log line. Returns 1 on a
   press it took. main.c passes the save point's answer in as `lock`. */
int  hell_hath_no_fury_oil_update(int lock);

#endif
