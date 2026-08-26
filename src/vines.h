#ifndef VINES_H
#define VINES_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — each curtain is tagged with the area it lives in */

/* Curtains of overgrowth that fill a doorway. Structurally this is the FAT DOOR
   (src/fatdoor.c): one global array tagged by area, instanced SMD drawn with the
   room's own texture window, an axis-aligned box for collision, and a hitscan
   solid test so the gun respects it. Read that file first — the draw loop here
   is the same loop.

   THE ONE THING THAT IS NEW IS `destructible`. A fat door is always breakable;
   a vine curtain may be either, per instance:

     destructible = 1   five crucifaxe hits, or ONE Flame Round, and it clears.
     destructible = 0   nothing removes it. It is a wall the player can see
                        through the far side of, and the way past is elsewhere.

   Nothing else in the module branches on it: an indestructible curtain still
   collides, still blocks a shot, and still draws exactly the same. It simply
   never loses health, so `health` is meaningless when the flag is 0 and is left
   at VINE_MAX_HEALTH so a save round-trip cannot resurrect or clear one.

   >>> STANDARD ROUNDS DO NOTHING. <<< The damage is asked for by type, not by
   weapon: DMG_FLAME clears a destructible curtain in one round and a Standard
   Round bounces off. See vines_shoot() and src/graveolver.c.

   >>> AND ENEMIES CANNOT TOUCH THEM. <<< There is deliberately no
   vines_damage_at() to mirror fatdoors_damage_at(): a zombie battering its way
   through the greenery is not the intent, and the zombie/spider/mushroom movers
   push out of a curtain (vines_collide) rather than chew through it. */

#define MAX_VINES          8   /* the array is part of the save blob, so raising
                                  this grows every save file */
#define VINE_MAX_HEALTH    5   /* crucifaxe hits to clear a destructible curtain */
#define VINE_FLAME_DAMAGE  5   /* ...or one Flame Round, which does all of it   */
#define VINE_SMASH_RANGE 300   /* crucifaxe reach past the box, as the fat door */
#define VINE_PUSH_MARGIN  30   /* camera standoff; smaller than the fat door's 55
                                  because the curtain is 100 deep, not 26, so the
                                  near plane is already well clear of its face  */

/* Model bounds, straight out of assets/props/Vines.smx: x[-50,50], z[-300,300],
   y[-900,0]. The ORIGIN IS AT THE FOOT, not at the centre the way the fat door's
   is — so a curtain's `y` is the floor it stands on and it hangs UP to
   y - VINE_HEIGHT. Getting that backwards buries it. */
#define VINE_HEIGHT      900
#define VINE_HALF_X       50
#define VINE_HALF_Z      300

typedef enum {
    VINE_INTACT,
    VINE_CLEARED,
} VineState;

typedef struct {
    int32_t   x, y, z;            /* y = the FLOOR it stands on (see VINE_HEIGHT) */
    int32_t   rot_y;
    int32_t   half_x, half_z;     /* world-axis collision half-widths             */
    int32_t   health;             /* hits remaining; inert while !destructible    */
    int32_t   destructible;       /* 0 = nothing clears it                        */
    VineState state;
    int32_t   active;
    GameState area;               /* draw/collide/damage all skip other areas     */
} Vine;

extern Vine vines[MAX_VINES];
extern int  vine_count;

void vines_load_assets(void);   /* load VINES.SMD — STARTUP only (no CD later) */
void vines_init(void);          /* place every curtain in the game             */
void vines_reset(void);
void vines_draw(RenderContext *ctx);
void vines_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* Hitscan solid test, and the area-only gate that lets collision.c skip the
   whole sampling pass in rooms that hold none. Both mirror the fat door's. */
int  vines_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack);
int  vines_any_solid(void);

/* Crucifaxe: damage every intact curtain in front of the player, one point
   each. Returns 1 if any was hit (the caller guards re-entry per swing). An
   indestructible curtain still reports a hit — the axe connects and thuds —
   it just never loses health. */
int  vines_try_smash(void);

/* The gun's half of it. `i` indexes vines[]; vines_body fills the centre and
   half-extents graveolver.c's enemy_in_circle wants, so a curtain competes for
   the crosshair on depth like any enemy. vines_shoot applies ONE round of the
   given damage type: DMG_FLAME clears a destructible curtain outright, anything
   else does nothing but spark. */
void vines_body(int i, int32_t *out_cy, int32_t *out_half_h, int32_t *out_half_w);
void vines_shoot(int i, int damage_type);

#endif
