#ifndef INTRO_H
#define INTRO_H

#include "render.h"

/* Opening sequence: runs between "NEW GAME" on the title screen and the first
   frame of the Delivery Area. See src/intro.c for the timeline. */

void intro_load_assets(void);        /* STARTUP ONLY — LoadImage of MANSION.TIM */
void intro_start(void);              /* called the moment New Game is confirmed */
void intro_update(void);
void intro_draw(RenderContext *ctx);
int  intro_finished(void);           /* 1 once, when the sequence is over       */

#endif
