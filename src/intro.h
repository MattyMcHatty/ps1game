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
   The opening sequence's own font, and public so any other screenful of text
   that fades on over a flat background can share it rather than carrying a
   second copy of the glyph table. (The trial build's sign-off screen was that
   other caller; it has been removed, so today the intro is the only one.)
   Uppercase, ", . @ ' !" and space; anything else draws as a blank cell.
   `level` is 0-255 and `ot` is the OT bucket the runs are sorted into — the
   caller owns it, since it decides what the text draws over. Lines are
   centred horizontally; intro_text_width is exposed for checking a reworded line
   still fits inside 320px. */
int  intro_text_width(const char *s);
void intro_text_draw(RenderContext *ctx, const char *s, int y, int level, int ot);

#endif
