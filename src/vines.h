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
    int32_t   lift;               /* how far the RAISE has wound it up, 0..VINE_HEIGHT.
                                     DRAW ONLY: a rising curtain is still solid,
                                     and only goes passable when the raise ends
                                     and sets state = VINE_CLEARED. Transient —
                                     the save carries the cleared state, not the
                                     travel (see vines_raise_start). */
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
/* Advance every curtain that is dropping in (see vines_drop_start). Cheap and
   unconditional; call it once a frame from the area update. */
void vines_drop_update(void);
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

/* ---- Winding a curtain up into the ceiling ---------------------------------
   The other way a curtain leaves the world, and the way an INDESTRUCTIBLE one
   has to: a puzzle answers, and the greenery winds up out of the doorway. It
   ends in exactly the same place a burnt one does — state = VINE_CLEARED,
   health = 0 — so the WorldDelta records it with the bit it already had and no
   save-format change was needed.

   >>> IT IS SOLID FOR THE WHOLE TRAVEL. <<< `lift` moves the DRAWN model only;
   vines_collide, vines_point_solid and vines_any_solid are untouched until the
   final frame flips the state. That is deliberate and it is the cheap answer to
   a real question — what a half-raised curtain does to a player standing under
   it — rather than an oversight: at a couple of seconds nobody is waiting on it,
   and a shrinking collision box would have had to answer for the gun and the
   zombie movers too.

   ONE AT A TIME. There is one raise in flight across the whole game, because
   there is one puzzle that starts one; a second vines_raise_start while another
   is travelling replaces it, which would strand the first half-way. Nothing does
   that today and the assert is the comment. */
void vines_raise_start(int i, int32_t frames);
/* Advance the raise. Returns 1 on the frame it finishes AND on every frame
   after — i.e. "there is nothing travelling" — so a caller can poll it the way
   chainlink_doors_raise_update is polled. */
int  vines_raise_update(void);

/* ---- Dropping a curtain IN ------------------------------------------------
   The mirror of the raise, and the way a curtain ARRIVES: the Greenhouse's
   flood sends five down out of the roof at once (src/greenhouse_flood.c).

   Three differences from the raise, all of them forced by "five at once":

     IT IS PER-CURTAIN, not one-in-flight. The raise keeps a single module
     index because one puzzle starts one; a drop keeps a timer per slot.

     IT ACTIVATES THE SLOT. A curtain that has not dropped yet is `active = 0`,
     which is what keeps it out of the draw, the collision, the hitscan and
     vine_in_area alike — there is no separate "hidden" bit to forget to test.
     vines_drop_start is what turns it on.

     IT IS SOLID FROM THE FIRST FRAME, for the same reason the raise stays
     solid to the last: `lift` moves the drawn model only. A curtain still half
     in the roof already blocks the aisle under it. Nobody is standing there —
     the drop happens under a camera cut — and a growing collision box would
     have had to answer for the gun and the enemy movers too.

   Passing 0 frames places it down and finished, which is what a room entry
   does when the flood has already happened. */
void vines_drop_start(int i, int32_t frames);

/* Index of the (first) intact curtain in `area`, or -1. The puzzle that opens
   one has to name it somehow, and naming it by room keeps the placement table
   in vines_init the single source of truth for where curtains are. */
int  vine_in_area(GameState area);

/* The (first) intact INDESTRUCTIBLE curtain in `area`, or -1. A puzzle that
   winds a curtain up must ask for it this way and not with vine_in_area: the
   Greenhouse holds six, and once the annexe curtain is cleared vine_in_area
   starts answering with one of the flood's five instead — which would have
   greenhouse_puzzle_init quietly delete an aisle curtain on every later entry.
   `destructible = 0` IS the distinguishing fact rather than a tag invented for
   the purpose: a curtain nothing else can remove is exactly the kind a puzzle
   has to open. */
int  vine_locked_in_area(GameState area);

#endif
