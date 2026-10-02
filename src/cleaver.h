#ifndef CLEAVER_H
#define CLEAVER_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — each instance is tagged with its area */

/* -----------------------------------------------------------------------
 * Cleaver — a guillotine blade hung in a slot in the vault that SLAMS DOWN on
 * a player walking under it. Chapter 3's third piece of moving furniture, built
 * on the bars' terms (src/bars.h): its own instance array, a small SMD read
 * once in main() and held for the run, collision out of its own mesh, and a
 * lift that the drop animates.
 *
 * PLACED in the Cleaver Corridor, three of them across the corridor's width
 * (src/cleaver_corridor.c), and in Cleaver L, four across its two arms
 * (src/cleaver_l.c). Nothing in this module knows that. Only one of the two
 * rooms is ever loaded, and each clears and re-places its own, so they share
 * the MAX_CLEAVERS pool rather than adding to it.
 *
 * TWO YAWS. As modelled, the blade lies across an EAST-WEST run (30 thick in
 * X, 550 wide in Z). Placed `turned`, it is yawed a quarter and lies across a
 * NORTH-SOUTH one; the AABB, the trigger, the knockback and the draw all turn
 * with it. Nothing finer is needed by a room built on a 90-degree grid.
 *
 * >>> THE MESH IS THE COLLISION DATA. <<< cleavers_load_assets() measures the
 * SMD's bounding box and that box is the whole of the prop's solid volume: 30
 * thick, 550 wide, 500 tall as authored. A re-export that changes the model
 * changes the collision with it.
 *
 * >>> AND ITS AUTHORED POSITION IS THE PLACEMENT. <<< Cleaver.smx was modelled
 * IN PLACE in the Cleaver Corridor's scene: x[1185,1215] y[-1150,-650]
 * z[-275,275]. cleavers_load_assets() re-centres it in RAM — plan centre to the
 * origin, the CUTTING EDGE (the largest y, since -Y is up) to y=0 — and keeps
 * what it subtracted: cleaver_authored_x/z() is where the artist put it and
 * cleaver_authored_lift() is how high its edge hung above the corridor floor
 * (650). A re-export that moves the blade moves the placement with it.
 *
 * THE CYCLE. A blade is placed ARMED: hanging at its lift, still. When the
 * player comes within CL_TRIGGER_REACH of it along its run (and is inside
 * its span across it, so a blade at the corner of an L ignores the other arm)
 * it drops — under
 * CL_GRAVITY, which is two and a half times the bars' so it SLAMS rather than
 * falls, and bounces once, hard and small (CL_RESTITUTION), with SFX_SLAM on
 * the impact. From then on it is LIVE and never goes back to ARMED:
 *
 *     DOWN     CL_DOWN_FRAMES (2 s) on the floor
 *     RISING   CL_RISE_FRAMES back up to its lift
 *     RAISED   CL_UP_FRAMES   (2 s) hanging
 *     FALLING  ...and slams again, whether or not anyone is under it
 *
 * The figures, from the Cleaver Corridor's 750 lift (the authored 650 plus
 * the room's CC_CLEAVER_RAISE): the edge passes head height ~16 frames after
 * the drop starts and hits the floor at ~19. With the 400 trigger, a player
 * WALKING in (12 a frame) covers ~195 in that time and is still ~205 short of
 * the blade when it arrives — outside its 165 reach, so the first slam is a
 * scare. One SPRINTING in (20) covers ~325 and is caught. The live cycle leaves
 * ~2.5 s open per blade, and crossing its footprint (330 with the standoff)
 * takes a walker ~28 frames.
 *
 * THE HIT. Once per drop: if the edge is at or below the player's head while
 * they overlap the blade's footprint (CL_HIT_REACH either side of it), they
 * take CL_DAMAGE, SFX_HURT, and a knockback straight along the run away from
 * the blade (X, or Z turned), so it throws them back the way they are on
 * rather than into a wall. collide then holds them off the fallen blade.
 *
 * COLLISION FOLLOWS THE LIFT, as the bars' does: raised, its solid span is over
 * the player's head and it blocks nothing; down, it spans the corridor.
 *
 * THE SLOT. The vault it hangs from has no hole in it, so a raised blade's upper
 * two-thirds are ABOVE the ceiling plane — and a prop drawn at true depth over
 * a room mesh biased deeper would paint them onto the vault. cleavers_draw()
 * skips every primitive lying wholly above the instance's `ceiling_y`, so the
 * blade reads as coming out of a slot and appears band by band as it drops.
 *
 * STATE IS NOT SAVED, on purpose: the room clears and re-places its blades
 * ARMED on every entry, which is the brief's "they reset if the player leaves
 * the room".
 *
 * TEXTURE: The Pit's rusty ironwork (x704 y0), which the blade was modelled in.
 * This module registers nothing; the room calls the_pit_upload_rusty(), and the
 * tpage/clut are the compile-time TIM_*_RUSTY constants. The Blender material
 * is `rusty_128`, so textures/rusty_128.tim is a byte-identical exporter alias
 * of rusty.tim for smxlink's sake (EXPORTER_ALIASES in tools/vram_map.py) and
 * is not on the disc.
 *
 * ITS UVs REACH 128, so the caller's 128 texture window must be in force: draw
 * AFTER draw_crawlers()/draw_lumberers(), which restore it — the bars' rule.
 * ----------------------------------------------------------------------- */

#define MAX_CLEAVERS          4    /* Cleaver L's four; the corridor uses three */

/* The drop, in 1/256ths of a world unit per frame (and per frame squared). */
#define CL_GRAVITY         1024    /* 4 units/frame^2 — the bars' is ~1.56      */
#define CL_RESTITUTION       50    /* of 256: one small, hard bounce           */
#define CL_SETTLE_SPEED     768    /* 3 units/frame: slower than this, it stops */

#define CL_DOWN_FRAMES      120    /* 2 s on the floor before it rises          */
#define CL_RISE_FRAMES       60    /* 1 s winching back up                      */
#define CL_UP_FRAMES        120    /* 2 s hanging before it slams again         */

/* How close in X, from the blade's centre plane, the player has to come to set
   an ARMED blade off. The blade is 30 thick, so this is 385 short of its face.
   (It was 200, "almost underneath", which caught every walker; see THE CYCLE's
   figures for what 400 does.) */
#define CL_TRIGGER_REACH    400

/* How far off the blade's faces the shared collision routine holds the player.
   NOT the 75 other props take: the blade is only 30 thick, and 75 let the eye
   (which IS the player's position) get within 90 of its centre plane — close
   enough to seem to stand inside it. 150 holds the eye 165 off. */
#define CL_COLLIDE_RADIUS   150

/* Past the blade's faces, how far a player's centre still counts as under it.
   The collision standoff itself, so anyone the fallen blade would shove is
   someone it hit. */
#define CL_HIT_REACH        CL_COLLIDE_RADIUS
#define CL_DAMAGE            50
#define CL_KNOCKBACK        110    /* ~440 units of push with the 3/4 decay     */

typedef enum {
    CL_ARMED = 0,      /* hanging at its lift, waiting for the player          */
    CL_FALLING,        /* in the air, falling or on its bounce                 */
    CL_DOWN,           /* on the floor, counting down to the rise              */
    CL_RISING,         /* winching back up                                     */
    CL_RAISED          /* live and hanging, counting down to the next slam     */
} CleaverState;

/* Startup: read CLEAVER.SMD (held for the run), measure and re-centre it. */
void cleavers_load_assets(void);

/* Where Cleaver.smx put the blade: its plan centre, and how high its edge hung
   above the floor of the room it was modelled in. Valid after load_assets. */
int32_t cleaver_authored_x(void);
int32_t cleaver_authored_z(void);
int32_t cleaver_authored_lift(void);

void cleavers_clear(void);

/* Place one ARMED. (x, z) is the blade's plan CENTRE; `y` is the floor
   reference, world y = y + GROUND_FLOOR_Y (the crib's and the bars'
   convention); `lift` is how far above that floor its edge hangs; `ceiling_y`
   is the WORLD y of the vault it hangs from (see THE SLOT above); `turned` 1
   yaws it a quarter to lie across a north-south run (see TWO YAWS). Returns the
   instance index, or -1 if MAX_CLEAVERS are placed. */
int  cleaver_place(GameState area, int32_t x, int32_t y, int32_t z,
                   int32_t lift, int32_t ceiling_y, int turned);

/* One frame of every blade in current_area: the trigger, the cycle, the hit.
   Call AFTER the room's collision, so the hit test sees the player where this
   frame's collision left them. */
void cleavers_update(void);

/* Player push-out: the bars' shallowest-axis scheme against the baked AABB,
   gated vertically on the CURRENT lift. Area-gated, so the shared collision
   routine calls it unconditionally. */
void cleavers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

void cleavers_draw(RenderContext *ctx);

#endif
