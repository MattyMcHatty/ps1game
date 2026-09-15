#ifndef HATCH_ARRIVAL_H
#define HATCH_ARRIVAL_H

/* =========================================================================
   THE DROP OFF THE WELL — the last beat of Asag's ending.
   =========================================================================
   The player went DOWN a shaft to fight Asag (src/hatch_puzzle.h) and they come
   back up somewhere else: out of the brick well in The Hatch's north chamber,
   the one the room's header is at pains to say is NOT the pit. The camera is
   already standing on top of it, in front of the four chain_128 polys that hold
   the lid up, facing south down the room. It jumps off, lands on the lawn, and
   the player has their legs back.

     0.0 s   held on the well top, looking south and a little down
     0.4 s   THE ARC. Forward off the well and onto the chamber floor, over a
             parabola — 490 units south and 185 units down, with a small rise in
             the middle of it. Seven tenths of a second.
     1.1 s   the landing: SFX_HURT, the pitch levels, the garden track starts,
             and the camera is released.

   >>> THE HURT SOUND IS A SOUND AND NOT DAMAGE, which is the call Asag's own
   arrival made (src/asag_boss.c's ABE_DROP) and for the same reason: taking
   health off a player for a move they did not choose is a different design
   decision from the one that was asked for. SFX_HURT is RESIDENT (src/sound.h),
   so it plays whichever bank this room is holding. <<<

   >>> AND THE MUSIC STARTS HERE, NOT ON ARRIVAL. <<< Everything from the
   killing blow to this frame is silent — src/asag_boss.c cuts the boss track on
   the last hit, the catacombs' door scene plays over nothing, and this room's
   usual cdaudio_play is suppressed on this one route (see main.c). The garden
   coming back in as the player's feet hit the grass is what says the encounter
   is over.

   ---- WHERE IT SITS IN THE CHAIN -------------------------------------------
     src/asag_boss.c      the death. Ends in a transition rather than a release.
     src/catacomb_open.h  the doors, in the Outside Catacombs. Ends the same way.
     THIS                 the landing, and the end of the whole sequence.
   Each one reports "finished" for a frame and main.c takes the transition; none
   of them knows what the next room is.
   ========================================================================= */

/* ROOM ENTRY from the Outside Catacombs, and from nowhere else: main.c arms
   this in the STATE_LOADING branch when the room being entered is The Hatch and
   the room being left is the Outside Catacombs, which is not a journey any
   other route makes. Takes the camera and anchors the player at the LANDING
   point rather than on the well, because that is where they are about to be and
   nothing should be able to reach them on the way. */
void hatch_arrival_start(void);

/* Park it. From the_hatch_init(), i.e. on EVERY arrival, so an ordinary walk in
   through the west gate cannot inherit a half-played scene. Called BEFORE
   hatch_arrival_start() on the one path that arms it. */
void hatch_arrival_reset(void);

/* 1 while the scene owns the camera — the hold and the arc. False on the frame
   after the landing, which is free play. main.c needs it in the cutscene
   early-return at the top of update_current_area(), in the `cutscene` list that
   suppresses the Start menu and the weapon overlay, and in the HUD exception. */
int  hatch_arrival_active(void);

/* One game frame. Safe when it is not running. */
void hatch_arrival_update(void);

#endif
