#ifndef BTN_GLYPH_H
#define BTN_GLYPH_H

#include <stdint.h>
#include "render.h"

/* PlayStation face-button glyphs (Circle/Cross/Square/Triangle), each in its
   own colour, embeddable anywhere text refers to a button.

   A button is a CONTROL CHARACTER inside an ordinary string: build prompts by
   pasting the BTN_* literals, e.g.  "Press " BTN_CIRCLE " to enter".
   - World-space signs: door_draw_string_3d / _billboard recognise the codes
     and draw the button shape in its own colour (fade still applies).
   - Screen-space menus: btn_prompt_draw() renders a mixed string, using the
     debug font for text and coloured pixel tiles for the button codes. */

#define BTN_CIRCLE_CH   '\x01'   /* red        */
#define BTN_CROSS_CH    '\x02'   /* light blue */
#define BTN_SQUARE_CH   '\x03'   /* pink       */
#define BTN_TRIANGLE_CH '\x04'   /* green      */

/* String literals for pasting into prompts. */
#define BTN_CIRCLE   "\x01"
#define BTN_CROSS    "\x02"
#define BTN_SQUARE   "\x03"
#define BTN_TRIANGLE "\x04"

/* If c is a button code, returns its 5x7 bitmap (LSB-first rows, same format
   as the door font) and writes its colour to r/g/b; NULL for normal chars. */
const uint8_t *btn_glyph_lookup(char c, uint8_t *r, uint8_t *g, uint8_t *b);

/* Screen-space prompt: draws `s` at (x,y) — text via FntSort (8px advance),
   button codes as coloured 5x7 pixel tiles — sorted at ot[ot_idx]. */
void btn_prompt_draw(RenderContext *ctx, int x, int y, const char *s, int ot_idx);

#endif
