#ifndef SPIRIT_GLOW_H
#define SPIRIT_GLOW_H

#include <stdint.h>
#include "render.h"

/* -----------------------------------------------------------------------
 * THE MINI-BOSSES' SPIRITS - one glow, three colours
 *
 * Every mini-boss death ends with its spirit as a ball of light climbing out
 * of sight (src/hadad_grinder.c, src/adapa_death.c, src/gula_death.c), and
 * Destiny's Door (src/heros_plinth.c) brings the same three spirits back to
 * fly into the jewels on its frame. The colour is the boss's and the jewel's:
 *
 *     ADAPA  BLUE     the frame's RIGHT jewel
 *     HADAD  GREEN    the TOP jewel
 *     GULA   YELLOW   the LEFT jewel
 *
 * >>> A SPIRIT'S COLOUR LIVES HERE AND NOWHERE ELSE. <<< Recolour a boss here
 * and his death and his jewel follow; a copy in a death file would let the
 * two disagree.
 * ----------------------------------------------------------------------- */
extern const uint8_t SPIRIT_RGB_ADAPA[3];
extern const uint8_t SPIRIT_RGB_HADAD[3];
extern const uint8_t SPIRIT_RGB_GULA[3];

/* The death scenes' ball: three concentric additive squares round the
   projection of (x, y, z), SPIRIT_GLOW_WORLD world units at the outermost,
   sorted at sz>>2 so it draws over the room. hadad_grinder.c's hg_glow,
   which the other two copied number for number. `bright` 0..256. Call with
   the view matrix up. */
#define SPIRIT_GLOW_WORLD  150
void spirit_glow_draw(RenderContext *ctx, int32_t x, int32_t y, int32_t z,
                      int32_t bright, const uint8_t rgb[3]);

/* The same three rings at any outer size - Destiny's Door's jewels. */
void spirit_glow_draw_sized(RenderContext *ctx, int32_t x, int32_t y,
                            int32_t z, int32_t bright, const uint8_t rgb[3],
                            int32_t world_half);

/* The death scenes' pulse: ~224..288 of 256 off a frame clock. */
int32_t spirit_glow_pulse(int32_t clock);

#endif
