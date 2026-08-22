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
                                 one leaf image used twice.                       */
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
