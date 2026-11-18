#ifndef DESTINY_PLINTHS_H
#define DESTINY_PLINTHS_H

#include <stdint.h>
#include "render.h"

/* -----------------------------------------------------------------------
 * THE CHAMBER OF DESTINY's TWO WEST PLINTHS (Nov 2026)
 *
 * On the dais (y=-200), side by side, both x[-5800,-5400] with their fronts
 * facing EAST at x=-5400 (collision walls 14-17 box the pair):
 *
 *     LOWER   top y=-350, z[-600,0]   centre (-5600, -300)   SHARUR
 *     HIGHER  top y=-400, z[0,600]    centre (-5600,  300)   THE TABLET OF
 *                                                            DESTINIES
 *
 * Sharur stands on its plinth as its own 3D model (src/sharur.c), the
 * Tablet as its sprite (item_pickup_draw_fixed, PICKUP_TABLET_OF_DESTINIES).
 * NEITHER IS A PICKUP: walking up to them takes nothing. Each plinth front
 * carries "Press O" while its prize is there. Circle, facing it, from the
 * dais, starts THE CEREMONY:
 *
 *   - the camera glides to a fixed shot square in front of that plinth and
 *     holds (the player is anchored where they stood);
 *   - 1 s after the press the item rises a little (0.67 s), then glows for
 *     2 s (the spirits' additive rings, in Sharur's pale blue-white), then
 *     flies at the camera (0.67 s, accelerating);
 *   - as it arrives: the pickup sound, the log line, the inventory. Control
 *     comes back where the player stood. Sharur is put in the hand, as every
 *     weapon pickup does.
 *
 * Once per prize, saved: FLAG_SHARUR_TAKEN / FLAG_TABLET_OF_DESTINIES_TAKEN.
 * A taken plinth draws nothing and has no sign.
 * ----------------------------------------------------------------------- */

void destiny_plinths_arm(void);            /* room entry: no scene, Circle seeded */
int  destiny_plinths_active(void);         /* the ceremony owns the camera        */
void destiny_plinths_update(int lock);     /* free roam: Circle; else the scene   */
void destiny_plinths_draw(RenderContext *ctx);   /* view matrix up             */

#endif
