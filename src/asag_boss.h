#ifndef ASAG_BOSS_H
#define ASAG_BOSS_H

/* =========================================================================
   ASAG'S DIRECTOR — and right now it is a DEMO, not an encounter.
   =========================================================================
   >>> THIS FILE IS A PLACEHOLDER AND IS MEANT TO BE THROWN AWAY. <<< It exists
   so the six clips can be LOOKED AT: it plays them one after another on an
   endless loop and does nothing else. There is no health, no phase machine, no
   attack that can hurt anybody, no camera work, no seal, no death sequence, no
   sound and no music.

   WHAT A REAL ENCOUNTER GOES THROUGH IS tools/ADDING_A_BOSS_ENCOUNTER.txt, and
   the hooks it wants are already marked in place in src/main.c and
   src/asag_arena.c — the cutscene early-return at the top of
   update_current_area(), the free-play update, the seal predicate in
   asag_arena_exit_sealed(), and the four draw calls at the foot of
   asag_arena_draw(). Writing the fight means replacing the body of this file,
   not rewiring any of those.

   THE INTERFACE THE BODY OFFERS is the whole of src/asag.h: asag_play(),
   asag_stop(), asag_clip_done() and asag_set_visible(). This module uses three
   of the four and nothing else, which is the shape a real director should keep.
   ========================================================================= */

/* Start the cycle from the beginning. Called from asag_arena_init(), i.e. on
   every arrival, so a debug level-select jump sees the same thing a drop down
   the shaft does. */
void asag_boss_reset(void);

/* One game frame. Chains the next clip when the current one finishes. Call from
   the arena's free-play branch in main.c, BEFORE asag_update() — see the .c for
   why the order matters by exactly one frame. Safe with no model loaded. */
void asag_boss_update(void);

#endif /* ASAG_BOSS_H */
