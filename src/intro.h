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

/* ---- The 5x7 fade-able font ------------------------------------------------
   Shared with src/trial_end.c, which is the same job (a screenful of text that
   fades on over a flat background) and must not be a second copy of the glyph
   table. Uppercase, ", . @ ' !" and space; anything else draws as a blank cell.
   `level` is 0-255 and `ot` is the OT bucket the runs are sorted into — the
   caller owns it, because the two screens draw over different things. Lines are
   centred horizontally; intro_text_width is exposed for checking a reworded line
   still fits inside 320px. */
int  intro_text_width(const char *s);
void intro_text_draw(RenderContext *ctx, const char *s, int y, int level, int ot);

#endif
