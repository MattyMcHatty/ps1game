#ifndef RECEPTION_H
#define RECEPTION_H

#include "render.h"

/* Reception is a placeholder room (untextured, flat-shaded) entered through the
   kitchen's "to reception" door. It will be replaced once the art is done. */
void reception_load_assets(void);     /* startup: register streamed textures */
void reception_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void reception_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void reception_init(void);          /* set collision/floor zones + spawn */
void reception_draw(RenderContext *ctx);
void reception_door_arm(void);      /* seed Circle edge state on entering reception */
int  reception_door_triggered(void);/* 1 when Circle pressed within range of the door */
void wdoor_arm(void);               /* same pair for the west single door */
int  wdoor_triggered(void);         /* 1 when Circle pressed within its range */
void cdoor_arm(void);               /* same pair for the conservatory door */
int  cdoor_triggered(void);         /* 1 when Circle pressed within its range */
void hdoor_arm(void);               /* same pair for the 2nd-floor door to the 2F hall */
int  hdoor_triggered(void);         /* 1 when Circle pressed near the upper-floor south door */
void edoor_arm(void);               /* same pair for the 2nd-floor east double door */
int  edoor_triggered(void);         /* 1 when Circle pressed near the upper-floor east door */
void ndoor_arm(void);               /* same pair for the 2nd-floor NORTH-WEST door */
int  ndoor_triggered(void);         /* 1 when Circle pressed near it (-> West Corridor) */

/* Re-seed the Circle edge state of ALL SIX doors and the save point at once.
   reception_init() arms them one by one on entry; this is for anything that
   takes input away mid-visit and has to hand it back — the entry quake
   (src/reception_hadad.c) is the only such thing today. An edge that has not
   been polled for five seconds is stale either way, so re-arming the lot is
   both cheaper to read and impossible to get half right. */
void reception_doors_arm(void);

/* Arrive back on the upper floor at the north-west door, coming out of the
   West Corridor's east door. */
void reception_spawn_northwest(void);

/* ---- SEALED RECEPTION (FLAG_HADAD_TWO) -------------------------------------
   Once the four key stones are back out of the Attic Exit's door the house has
   closed around the player, and this room becomes the stage for the Reception
   Hadad encounter. Three things change, and reception_sealed() is the single
   test all three read:

     1. FOUR OF THE SIX DOORS ARE RUBBLE. The kitchen door (RDOOR), the
        conservatory door (CDOOR), the 2F Hall door (HDOOR) and the East Hall
        door (EDOOR) keep their green "Press O to enter" signs — the player is
        meant to try them — but Circle only posts "There is rubble behind the
        door!" and there is no transition. Handled in main.c beside the door
        triggers, exactly as the East Hall's rubble is (east_hall_quake.h), so
        each *_triggered() keeps meaning "the player pressed O at this door".
        The piano-room door (WDOOR) and the West Corridor door (NDOOR) still
        work: they are the way in and the way on.
     2. NO SAVE POINT. reception_apply_flags() clears it.
     3. NO MUSIC ON ENTRY. main.c stops CD-DA instead of starting
        CDAUDIO_RECEPTION_TRACK.

   reception_apply_flags() is called from main.c's post-entry re-derive block —
   AFTER world_enter and savegame_apply_pending, the same slot and the same
   reason east_hall_apply_flags has: reception_init() runs while game_flags
   still holds the pre-load values, so anything derived from a saved flag has to
   be re-derived there or a "Load Game" straight into this room reads the
   previous playthrough's flags. */
int  reception_sealed(void);        /* 1 once FLAG_HADAD_TWO is set */
void reception_apply_flags(void);   /* re-derive the above (save point) */

#endif
