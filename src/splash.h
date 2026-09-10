#ifndef SPLASH_H
#define SPLASH_H

#include "render.h"

/* ---- THE BOOT SPLASH -------------------------------------------------------
   The Electric Reload logo, before the red LOADING screen and before the title.
   Four phases, all wall-clock timed off the vblank counter rather than off a
   frame count (which matters — see below):

     ROLL   the logo rolls in from off the left of a black screen and lands
            dead centre, turning three times on the way in
     FLASH  the screen flashes yellow and settles to the logo's own yellow
     HOLD   everything sits still
     FADE   the whole screen fades to black, and the LOADING screen takes over

   >>> THE STARTUP ASSET BLOCK RUNS UNDERNEATH IT. <<< main()'s block is the
   longest freeze in the game, and this animation is what it now hides behind:

     splash_prelude()  runs ROLL and FLASH at a true 60fps with nothing else
                       going on. These two are the only phases with anything
                       MOVING in them, so they are the two that cannot share
                       the CPU with a blocking CD read.
     splash_pump()     one frame, called from main()'s loading_screen_pump()
                       after every step of the startup block. HOLD and FADE
                       draw a static (or near-static) screen, so a load step
                       between frames costs nothing the player can see.
     splash_finish()   the block is done: release the HOLD and play the rest out
                       at 60fps.

   Because the phases are timed off VSync(-1) and not off how many times
   splash_pump() happened to be called, the fade begins at the right MOMENT
   however slow or fast the disc is; a long step drops frames rather than
   stretching the animation. HOLD also has a ceiling (HOLD_MAX): on a slow drive
   the splash gives up waiting, fades out on schedule and hands the rest of the
   load to the red LOADING screen, which is what that screen is for.

   If ERLOGO.TIM cannot be read the whole thing is skipped — splash_prelude()
   falls back to priming both framebuffers with the loading screen exactly as
   main() used to, and every other call here becomes a no-op. The splash is a
   flourish, not a dependency.
   ------------------------------------------------------------------------- */

/* Read ERLOGO.TIM into VRAM. Call after CdInit() and before splash_prelude();
   it is the first thing main() reads, ahead of the startup block, for the same
   reason loading_screen_load_axe() is. */
void splash_load_assets(void);

/* ROLL + FLASH, at 60fps, flipping as it goes. Returns with the logo centred on
   a yellow screen and both framebuffers primed for the startup block. */
void splash_prelude(RenderContext *ctx);

/* 1 while the splash still owns the screen. main()'s loading_screen_pump()
   tests this: the splash outranks the red LOADING screen for as long as it is
   up, even though loading_screen_up is already set behind it. */
int  splash_active(void);

/* Advance to the current wall-clock frame, draw it and flip. No-op once done. */
void splash_pump(RenderContext *ctx);

/* The startup block is finished: let HOLD end and play the fade out at 60fps.
   Returns once the screen is black. No-op if the splash already timed out and
   handed over to the loading screen. */
void splash_finish(RenderContext *ctx);

#endif
