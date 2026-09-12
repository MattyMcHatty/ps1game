#ifndef DOOR_ANIM_H
#define DOOR_ANIM_H

#include "render.h"

/* Resident-Evil-style level-transition screen. When the player activates a door
 * to change levels, the screen cuts to black showing a closed double door; one
 * leaf swings open while the door sound plays, the whole thing fades to black,
 * then the destination level loads (via STATE_LOADING).
 *
 * Usage:
 *   door_anim_load_assets()  - STARTUP only (LoadImage), like fatdoors_load_assets
 *   door_anim_start(variant) - begin the transition (plays SFX_DOOR)
 *   then set game_state = STATE_DOOR_ANIM and, each frame in that state:
 *     door_anim_update(); door_anim_draw(ctx);
 *     if (door_anim_finished()) game_state = STATE_LOADING;
 *
 * `variant` selects which door texture the panel is drawn with: */
#define DOOR_PANEL_OUTER  0   /* dbl_dr_hlf       — standard door (default)     */
#define DOOR_PANEL_INNER  1   /* inr_dbl_dr_half  — interior door (-> reception) */
#define DOOR_PANEL_WOOD   2   /* wd_dr — SINGLE wooden door (reception west wall);
                                 one leaf, the whole door swings open            */
#define DOOR_PANEL_EXIT   3   /* xt_dr_lft_hlf + xt_dr_rt_hlf — the Attic Exit's
                                 unlocked exit door onto the Garden Stairs. Unlike
                                 the two above, BOTH leaves swing away from the
                                 camera, and each has its OWN texture rather than
                                 one leaf image used twice. It is also WIDER than
                                 a house door — sized to the proportions of the
                                 450x467 xt_dr panel the player walks up to in
                                 the Attic Exit. Also plays Garden Stairs ->
                                 Garden Courtyard.                                */
#define DOOR_PANEL_GATE   4   /* grdngtl + grdngtr — the wrought-iron garden gate
                                 between the Garden Courtyard and Fountain Square.
                                 Two textures like the exit door, but only the
                                 LEFT leaf swings; the right stays shut. It is
                                 also the one variant that does NOT play SFX_DOOR:
                                 it plays SFX_GATE, and its swing is cut to that
                                 clip's length so the leaf stops when the hinge
                                 stops creaking. The art is see-through (alpha in
                                 the PNG becomes CLUT 0), so the black behind
                                 shows through the bars.                          */
#define DOOR_PANEL_GREENHOUSE 5 /* greenhouse door — the door between the Stables
                                 and the Greenhouse. A SINGLE leaf, like
                                 DOOR_PANEL_WOOD, on the shared clock and with
                                 SFX_DOOR; it differs in the width, and in
                                 swinging about its LEFT edge where the wooden
                                 door swings about its right (the artwork itself
                                 is NOT mirrored to match). A greenhouse door is
                                 a broader thing than a house door, so the panel
                                 is 96 rather than 80 — the same 20% widening,
                                 and for the same reason, as the garden gate's.

                                 IT IS ALSO THE ONE PANEL THAT IS NOT READ OFF
                                 THE CD HERE. Its texture is room art (greenhouse
                                 door.tim at x320 y256) on a page three mansion
                                 textures also use, so a startup LoadImage would
                                 be undone the first time the player walked into
                                 Reception. It does not need one: both ends of
                                 this transition are in the garden-west VRAM bank
                                 and BOTH upload it on entry, so the pixels are
                                 always in VRAM when the panel is drawn. See
                                 door_anim_load_assets.                           */
#define DOOR_PANEL_FALL       6 /* >>> NOT A DOOR AT ALL. <<< The 1200-unit drop
                                 down the pit in The Hatch's yard into Asag's
                                 arena. There is no door on that transition and
                                 there never was one: it used to borrow
                                 DOOR_PANEL_GATE, so falling down a shaft played
                                 a garden gate creaking open, which is the wrong
                                 picture and the wrong sound.

                                 WHAT IT DRAWS instead is a flat 2x2 square of
                                 four mud quads, far off down the view axis, with
                                 the camera accelerating at them and fading to
                                 black before it arrives: the ground coming up at
                                 a falling player. It carries straight on from the
                                 in-room fall hatch_puzzle.c has already played
                                 (its HP_FALL runs y on t^2 for the same reason
                                 this runs z on t^2).

                                 IT IS SILENT, unlike every other variant. The
                                 drop in the yard is silent too, and the brief
                                 asks for the rush of the plane and nothing else.

                                 ITS TEXTURE IS THE ARENA'S OWN MUD, and it is
                                 not in VRAM when this starts - a transition draws
                                 BEFORE STATE_LOADING, so the destination room's
                                 upload has not run and The Hatch's art is still
                                 up. This is the one variant that reads its
                                 texture off the CD AT START TIME rather than at
                                 startup or not at all; door_anim_start calls
                                 asag_arena_upload_mud() for it. See there, and
                                 see door_anim_draw's fall branch for the
                                 projection.                                      */

void door_anim_load_assets(void);   /* load the panel TIMs into VRAM (startup) */
void door_anim_start(int variant);  /* begin the animation; plays the sound */
void door_anim_update(void);        /* advance one frame */
void door_anim_draw(RenderContext *ctx);
int  door_anim_finished(void);      /* 1 once the transition has fully faded out */
/* 1 from the frame door_anim_start() runs until the transition ends. Its one
   use is the tail of update_current_area: the door-trigger branches are NOT the
   last thing in that function, so the shared entity updates below them still run
   once on the trigger frame — after world_silence_monsters() has already cut
   every sound. Anything that would restart a sound (or worse, a CD-DA track)
   from that tail has to ask this first. See update_hadads(). */
int  door_anim_active(void);

#endif
