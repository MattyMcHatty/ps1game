#ifndef WEB_H
#define WEB_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — each web is tagged with its area */

/* -----------------------------------------------------------------------
 * Web — the spider's ranged attack.
 *
 * A small white cube spat straight at the player and thereafter dumb: it is
 * aimed ONCE, at where the player stood when it was fired, and then travels in
 * a straight line. That is what makes it dodgeable — walking toward or away
 * from the spider keeps you on the web's line and it connects, while strafing
 * takes you off that line well before it arrives.
 *
 * On contact it deals WEB_DAMAGE (a third of a spider bite) and applies the
 * player's poison status: half walk speed and no sprint for five seconds (see
 * player_poison in player.h).
 *
 * Webs are transient and NOT saved. Like the particles and bullet-hit sprites
 * they live entirely in RAM, tagged by area so a web in flight cannot follow
 * the player through a door, and cleared on reset.
 * ----------------------------------------------------------------------- */

#define MAX_WEBS         8
#define WEB_SPEED       18    /* world units per frame; ~0.75s from max range  */
#define WEB_DAMAGE       5    /* a third of SPD_DAMAGE_AMOUNT (15)             */
#define WEB_HALF        12    /* cube half-extent in world units               */
#define WEB_HIT_RADIUS  75    /* radial distance to the player that counts     */
#define WEB_LIFE        90    /* frames before it expires (~1600 units flown)  */

typedef struct {
    int32_t   x, y, z;
    int32_t   vx, vy, vz;   /* per-frame velocity, already scaled to WEB_SPEED */
    int       life;         /* frames left; 0 = free slot                      */
    GameState area;
} Web;

extern Web webs[MAX_WEBS];

/* Fire a web from (x,y,z) toward (tx,ty,tz). Silently does nothing if the pool
   is full — a dropped web is far better than one that never expires. */
void web_spawn(int32_t x, int32_t y, int32_t z,
               int32_t tx, int32_t ty, int32_t tz, GameState area);

void webs_update(void);
void webs_draw(RenderContext *ctx);
void webs_reset(void);

#endif
