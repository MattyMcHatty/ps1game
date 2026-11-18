#ifndef CRUCIFAXE_H
#define CRUCIFAXE_H

#include "render.h"

#define SWING_DURATION  7
/* The return IS the cooldown: the axe is raised slowly over what used to be a
   14-frame return plus a 21-frame rest, so the next press is taken the frame it
   is upright again. Press-to-press is unchanged at 41 frames. The hit window
   and rabisu.c's parry windows read only the first frames of swing_timer, so
   lengthening the tail moves neither. */
#define SWING_RETURN    34
#define SWING_TOTAL     (SWING_DURATION + SWING_RETURN)
#define SWING_RANGE     350
#define KNOCKBACK_SPEED 80

extern int swing_timer;
/* Damage per connected melee swing, in each enemy's swing units: 1 for the
   Crucifaxe, 2 while Sharur's tick runs (src/sharur.c). Read by every hit
   path in crucifaxe.c and by every module's *_try_hit(). */
extern int melee_damage;

void crucifaxe_init(void);
void update_crucifaxe(void);
void draw_crucifaxe(RenderContext *ctx);

/* The swing itself, shared with Sharur (src/sharur.c): start one (latches,
   sound, swing_timer = 1), and run one frame of it - every hit test, then
   swing_timer += step (1 = the Crucifaxe, 2 = Sharur, twice as fast). */
void melee_start_swing(void);
void melee_swing_tick(int step);
/* The held pose off swing_timer, drawn flat-shaded at `gain` (4096 = 1.0x).
   out_vs (may be NULL) receives the view-space pose matrix. */
#include <psxgte.h>
#include <smd/smd.h>
void melee_draw_model(RenderContext *ctx, SMD *smd, int32_t gain,
                      MATRIX *out_vs);

#endif
