#ifndef LIGHTSWITCH_PUZZLE_H
#define LIGHTSWITCH_PUZZLE_H

#include <stdint.h>
#include "render.h"

/* ---- The Attic Exit's lightswitch puzzle ------------------------------------
   Four levers, one in each corner fixture of the room, each wired to the ceiling
   light in the SAME corner (see the quadrant table in lightswitch_puzzle.c).
   Throwing a lever swings it down over half a second and its light projects a
   coloured cone onto the purple star painted on the floor in front of the cage.

   The four colours — yellow, blue, magenta, green — are dealt to the four lights
   at random on every entry, so which lever does what has to be read off the
   floor rather than memorised. The cage opens when yellow, blue and magenta are
   all lit and green is dark.

   Unlike the drawer/stove/piano boards this is NOT a camera-locked minigame: the
   levers are thrown in free play. Only the payoff takes the camera, which is
   what lightswitch_puzzle_active() reports.

   This module owns the four lever placements (lever_place is called from
   lightswitch_place), so the lever <-> light pairing has one source of truth. */

/* Place the levers and deal the colours. Call from attic_exit_init AFTER
   levers_clear(). Re-deals on every entry while unsolved; once solved the
   winning set is installed lit and the levers stand thrown. */
void lightswitch_place(void);

/* 1 while the solve cutscene owns the camera — free movement, weapons, the
   Start menu and the room's own door trigger must all be suppressed. */
int  lightswitch_puzzle_active(void);

/* Per-frame: proximity/Circle handling, lever animation, the solve cutscene.
   Call once a frame from the Attic Exit's area update. */
void lightswitch_update(void);

/* Light cones + the per-lever prompts. Call from attic_exit_draw with the
   camera view matrix loaded (after the room mesh and the levers). */
void lightswitch_draw(RenderContext *ctx);

#endif
