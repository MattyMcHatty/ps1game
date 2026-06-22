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

void door_anim_load_assets(void);   /* load the panel TIMs into VRAM (startup) */
void door_anim_start(int variant);  /* begin the animation; plays the sound */
void door_anim_update(void);        /* advance one frame */
void door_anim_draw(RenderContext *ctx);
int  door_anim_finished(void);      /* 1 once the transition has fully faded out */

#endif
