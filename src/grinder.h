#ifndef GRINDER_H
#define GRINDER_H

#include <stdint.h>
#include "render.h"

#define MAX_GRINDERS        4
#define GRINDER_PUSH_MARGIN 30   /* extra gap between player and grinder edge */
#define GRINDER_HALF_H     300   /* vertical collision reach around the grinder's
                                    floor reference (g->y). The model is 450 tall,
                                    but this must stay under a floor's height so
                                    the player can't collide with a grinder on the
                                    level below/above them — same rule as the
                                    dresser's DRESSER_HALF_H. */
#define GRINDER_SOLID_H    450   /* real model height above its floor base, for the
                                    height-aware gun LOS (Grinder.smx spans y=0
                                    base to y=-450 top). */

/* cut_x sentinel: draw the whole model, nothing set into a wall. */
#define GRINDER_NO_CUT  0x7FFFFFFF

/* A reusable, static, indestructible prop, built on the dresser's pattern: the
   player collides with it and it has no state of its own. It is textured with
   TWO textures:
     - plinth  : the room's already-resident plinth texture (reused, passed into
                 grinders_draw as tpage/clut, exactly like the dresser's wd_flr).
                 In the Rear Gate that is PLNTHRG, the room's own retarget.
     - grinder : the prop's OWN art. Unlike the dresser's, it is NOT streamed —
                 it owns a scrap of VRAM nothing else touches, so a single
                 startup LoadImage is all it ever needs. See grinder.c.

   Load geometry + upload the texture once at startup with grinder_load_assets().
   There is no per-entry upload to call: nothing overwrites its slot. Place
   instances per area with grinder_add() from that area's init (after
   grinders_clear()), render from that area's draw with grinders_draw(), and
   collide from that area's apply_collision_* with grinders_collide().

   >>> THE PUZZLE HOOK. <<< x/y/z are public and are read LIVE by both the draw
   and the collide, so a puzzle that slides a grinder only has to write them —
   the same arrangement the piano room's sinking bookcase uses for its offset,
   and the reason this prop is a module rather than part of the room mesh.
   Nothing moves one today. NOTE THAT cut_x BELOW IS A WORLD PLANE, NOT AN
   OFFSET INTO THE MODEL: it stays with the wall while the grinder slides, so
   more of the body simply comes into view as it emerges, and none of the
   sliding code has to touch it.

   y is the room "standing on the floor" reference (-149 for a floor at world
   y=0, i.e. every garden room); the draw adds GROUND_FLOOR_Y so the model's feet
   (authored at model y~0) rest on the floor, matching the other props. */
typedef struct {
    int32_t x, y, z;
    int32_t rot_y;       /* 0..4096 = full turn. 0 points the grinder-textured
                            face at -X, which is how the model is authored. */
    int32_t area;        /* the GameState this instance belongs to — see below */
    int32_t cut_x;       /* world X of the wall face it is set into, or
                            GRINDER_NO_CUT — see the block below */
    int32_t active;
    int32_t half_w;      /* model-space XZ half-extents (footprint) */
    int32_t half_d;
} Grinder;

extern Grinder grinders[MAX_GRINDERS];
extern int     grinder_count;

void grinder_load_assets(void);      /* startup: load SMD + LoadImage the TIM (CD reads) */
void grinders_clear(void);           /* remove all instances (call in an area's init) */
/* `area` is the GameState the instance lives in, and EVERY COLLISION ENTRY POINT
   BELOW GATES ON IT. This array is global — it is not part of world.c's per-room
   RoomState swap — so without that gate a grinder's fixed world coordinates
   would go on blocking the player invisibly in every other room whose bounds
   happen to contain them, which is how the kitchen's dining tables once stalled
   the zombies in the 2F hall. It is passed in rather than read from
   current_area because a room's init runs BEFORE main.c commits the new area.

   >>> cut_x IS THE WALL THE GRINDER IS SET INTO, AND IT IS NOT COSMETIC. <<<
   Pass the world X of that wall's visible face; everything behind the plane is
   clipped away in grinders_draw, so the drawn shape ends exactly at the wall.
   GRINDER_NO_CUT draws the whole model, for a free-standing one.

   IT HAS TO BE CLIPPED, NOT MERELY SORTED BEHIND THE WALL. The model is an
   open-backed cap and skirt: nine plate quads across its face, and nine skirt
   quads that each run its FULL depth. Half-buried, every skirt STRADDLES the
   wall plane — and the OT is a painter's sort with no depth buffer, so a
   straddling quad is drawn either wholly in front or wholly behind, decided by
   an average depth that converges on the wall's own as the camera backs off
   along the wall. That is why the grinders read as buried up close and as
   standing proud of the hedge from down the corridor, hollow back and all. The
   sinking bookcase met the same coin flip and settled it the same way (see
   piano_props.c). Clipping takes the sort out of it at every distance.

   The plane is a constant-world-X one, so rot_y must be 0 or 2048 — the grinder
   must face along X — for the cut to land where you meant. A wall running along
   Z would need the Z sibling of the icos term in grinders_draw. */
int  grinder_add(int32_t x, int32_t y, int32_t z, int32_t rot_y, int32_t area,
                 int32_t cut_x);
/* plinth_tpage/clut = the room's resident plinth texture (the slot the grinder
   reuses for every face that is not the grinder plate). The grinder's own
   texture is module-owned and needs nothing from the caller. */
void grinders_draw(RenderContext *ctx, uint16_t plinth_tpage, uint16_t plinth_clut);
void grinders_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
/* Hitscan solid test: 1 if (x,y,z) is inside a grinder's real solid volume
   (rotated footprint + height, no push margin). For gun line-of-sight. */
int  grinders_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack);

/* Is ANY instance of this family solid in the CURRENT AREA right now? Mirrors
   the non-coordinate gates of grinders_point_solid above/below, and nothing
   else. collision_segment_blocked uses it to skip its whole segment-sampling
   pass in rooms that hold no props at all — see the note there. */
int  grinders_any_solid(void);

#endif
