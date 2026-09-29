#ifndef BARS_H
#define BARS_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — each instance is tagged with its area */

/* -----------------------------------------------------------------------
 * Bars — a portcullis of iron bars that DROPS. Chapter 3's second piece of
 * moving furniture, after the crib, and built on the crib's terms: its own
 * instance array, a deferred texture registration, a small SMD read once in
 * main() and held for the run, and a narrow bars_upload_texture() for the room
 * that draws it.
 *
 * FIRST PLACED in The Pit, across the mouth of the north alcove, where it falls
 * at the end of the descent and seals the player in with two Lumberers
 * (src/the_pit.c, THE AMBUSH). Nothing in this module knows that: a room places
 * an instance RAISED, calls bars_drop() when it wants it down, and polls
 * bars_settled() to find out when it has stopped moving.
 *
 * >>> THE MESH IS THE COLLISION DATA, AS THE CRIB'S IS. <<< bars_load_assets()
 * measures the SMD's bounding box and that box is the whole of the prop's solid
 * volume: 860 wide, 860 tall, 50 deep as authored. A re-export that changes
 * the model changes the collision with it and there is no second number to
 * keep in step.
 *
 * >>> AND THE MESH IS RE-CENTRED IN RAM AT LOAD, WHICH THE CRIB'S IS NOT. <<<
 * Bars.smx was authored IN PLACE in The Pit's Blender scene, so its vertices
 * are world coordinates — x[-130,730] y[-860,0] z[3245,3295] — rather than an
 * origin-centred model. bars_load_assets() subtracts the plan centre and the
 * base from every vertex once, so model space is "centred in plan, standing on
 * y=0" like every other prop, and an instance's (x, z) is where its CENTRE
 * goes. That is what lets the same model stand in a room it was not authored
 * in. The Pit's placement is the authored position read back off the SMX; see
 * the note beside it.
 *
 * THE DROP. An instance has a LIFT: how far its base is above the floor it was
 * placed on (world units, positive = up). bars_drop() lets it go; from then it
 * falls under BARS_GRAVITY, hits the floor, and bounces back up at
 * BARS_RESTITUTION of the speed it hit at, until a bounce would be slower than
 * BARS_SETTLE_SPEED — then it is DOWN for good. The first impact plays
 * SFX_SLAM (RESIDENT, so it plays in any bank). The figures, from a 450 lift:
 *
 *     fall      24 frames, hitting at ~37 units a frame
 *     bounce 1  ~56 units high, 17 frames
 *     bounce 2  ~7 units high,   6 frames
 *     settled   ~0.8 s after the drop
 *
 * Only the first bounce reads; the second is the rattle. Tune the restitution
 * before the gravity: the gravity sets how HEAVY the drop looks, the
 * restitution how much it JUMPS, and the brief was "a little".
 *
 * COLLISION FOLLOWS THE LIFT. The vertical gate in bars_collide() uses the
 * instance's CURRENT base, so raised above head height the bars block nothing
 * (the player walks under them) and dropped they block the full width.
 *
 * STATE IS NOT SAVED and does not need to be yet: bars_clear() runs on every
 * entry to a room that places them and the room re-places them raised. The
 * Pit can only be entered from the top of its shaft, where the drop is always
 * still to come. A room that can be re-entered with the bars already down has
 * to remember that OUTSIDE the instance (a GameFlag or a WorldDelta bit), the
 * way the crib's solved set does.
 *
 * TEXTURE: bars.tim, 4bpp 128x128 at x512 y256 — the LEFT HALF of the
 * wd_dr_crk page — with wd_dr_crk's own CLUT line at (0,497). Stretched from a
 * 64x64 source, and that was measured (tools/TEXTURING_NOTES.txt PART 7): 287
 * world units per UV tile, so one copy of the art per tile gives bars ~57 apart
 * and ~18 thick. See the note on bars_load_assets() for the page's restore.
 *
 * >>> ITS UVs PASS 127, SO THE CALLER'S 128 TEXTURE WINDOW IS LOAD-BEARING.
 * <<< The front faces run u 0..192 and the tall bottom band v 116..255; they
 * tile correctly only because the window wraps them mod 128. The Pit sets that
 * window for its own mesh and the enemy sprites restore it, so bars_draw() must
 * be called AFTER draw_lumberers()/draw_crawlers(). Unlike the crib's, this
 * prop is NOT happy either way.
 * ----------------------------------------------------------------------- */

#define MAX_BARS              2

/* The drop, in 1/256ths of a world unit per frame (and per frame squared). */
#define BARS_GRAVITY        400    /* ~1.56 units/frame^2                     */
#define BARS_RESTITUTION     90    /* of 256: each bounce keeps ~35% speed    */
#define BARS_SETTLE_SPEED   768    /* 3 units/frame: slower than this, it stops */

/* THE RISE (bars_raise) is timed to the machinery, the piano-room bookcase's
   rule: mchne.vag is 2.80 s, it plays TWICE back to back, and the travel lasts
   exactly two clip lengths, the second play landing on the frame the first
   runs out. Re-encode that VAG at another length and BARS_MCHNE_FRAMES has to
   move with it (src/piano_props.c's PPROP_SINK_SFX is the same number). */
#define BARS_MCHNE_FRAMES   168    /* mchne.vag: 2.80 s at 60 fps             */
#define BARS_RISE_FRAMES    (BARS_MCHNE_FRAMES * 2)

typedef enum {
    BARS_RAISED = 0,   /* hanging at its lift, not moving                      */
    BARS_FALLING,      /* dropped: in the air, falling or on a bounce          */
    BARS_DOWN,         /* on the floor                                         */
    BARS_RISING        /* winching back up (bars_raise), then RAISED again     */
} BarsState;

/* Startup: read BARS.SMD (held for the run), measure and re-centre it, and
   register BARS.TIM (deferred — header only until the Catacombs bank is
   selected). */
void bars_load_assets(void);

/* Room entry: one LoadImage out of the RAM copy area_bank_sync() has read.
   Called from the_pit_upload_textures(). */
void bars_upload_texture(void);

void bars_clear(void);

/* Place one. (x, z) is the model's plan CENTRE; `y` is the floor reference,
   world y = y + GROUND_FLOOR_Y, the crib's convention; `lift` is how far above
   that floor its base hangs (0 = standing on it). Returns the instance index,
   or -1 if MAX_BARS are placed. */
int  bars_place(GameState area, int32_t x, int32_t y, int32_t z,
                int32_t rot_y, int32_t lift);

/* Let a RAISED instance go. No-op in any other state. */
void bars_drop(int idx);

/* 1 once the instance is BARS_DOWN. */
int  bars_settled(int idx);

/* Winch a DOWN instance back up to `lift`, linearly over BARS_RISE_FRAMES,
   with SFX_MCHNE (catacombs bank) twice back to back. No-op in any other
   state. Collision follows the lift as it goes, so the way opens the moment
   the base clears the player's head, a little over half way up from 0 to
   450. */
void bars_raise(int idx, int32_t lift);

/* 1 while the instance is BARS_RAISED (hanging still). */
int  bars_raised(int idx);

/* One frame of every falling or rising instance in current_area. */
void bars_update(void);

/* Player push-out: the dresser's shallowest-axis scheme against the baked AABB,
   gated vertically on the CURRENT lift. Area-gated, so the shared collision
   routine calls it unconditionally. */
void bars_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

void bars_draw(RenderContext *ctx);

#endif
