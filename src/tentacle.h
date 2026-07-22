#ifndef TENTACLE_H
#define TENTACLE_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — each tentacle is tagged with its area */

/* A stationary tentacle enemy. It cannot move and is never knocked back. It
   sits idle until the player comes within range, then oscillates between its
   idle and active sprites; if the player gets too close it lashes out, dealing
   demon-dog damage and knocking the player back. 3 HP, killable with the
   crucifaxe. Like the fat doors, tentacles are a single global array tagged by
   area (draw/update/hit skip tentacles whose area != game_state) and persist
   through world.c. */

#define MAX_TENTACLES          4
#define TENTACLE_MAX_HEALTH    3

typedef struct {
    int32_t   x, y, z;        /* y = floor "standing" reference (as other props) */
    int32_t   health;
    int32_t   active;         /* slot in use */
    int       anim;           /* oscillation counter (active state) */
    int       lively;         /* 1 = player in range this frame (oscillating)    */
    int       damage_timer;   /* cooldown between hits on the player             */
    int       hit_timer;      /* health-bar flash after a crucifaxe hit          */
    GameState area;
} Tentacle;

extern Tentacle tentacles[MAX_TENTACLES];
extern int      tentacle_count;

void tentacles_load_assets(void);   /* startup: load both sprite TIMs (resident) */
void tentacles_init(void);          /* place the conservatory tentacles          */
void tentacles_reset(void);         /* new game: restore the initial layout      */
void update_tentacles(void);        /* proximity oscillate + damage the player   */
void draw_tentacles(RenderContext *ctx);
/* Set the area's texture window so sprites can bracket it (they sit at u-off
   128). Pass NULL for areas with no window. */
void tentacles_set_texwindow(const RECT *tw);
/* Crucifaxe strike: damage the first in-range tentacle in front of the player.
   Returns 1 if one was hit (no knockback — tentacles never move). */
int  tentacles_try_hit(void);

/* Grave-olver hitscan support: aim target (sprite-centre Y + half-height) and a
   shot that deals 1 HP. */
void tentacle_body(const Tentacle *t, int32_t *cyc, int32_t *hh);
void tentacle_shoot(Tentacle *t);

#endif
