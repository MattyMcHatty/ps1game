#include <stdint.h>
#include "asag.h"
#include "asag_boss.h"

/* ASAG'S DIRECTOR — a DEMO. See asag_boss.h: this plays the six clips end to
   end on a loop so the animation can be looked at, and it is meant to be
   replaced wholesale by the real encounter. */

/* The cycle. ASAG_CLIP_* order, which is also tools/export_asag.py's bake order
   — except that EMERGE leads, because it is the only one of the six that reads
   as a beginning: it brings the body up out of the rock, and the other five all
   start from where it leaves off. Watching the loop from a cold start therefore
   looks like an arrival rather than like joining something already in progress.

   Every one is played NON-LOOPING, which is what makes the chain work at all:
   a looping clip never reports asag_clip_done() and the cycle would stop on it
   forever. */
static const AsagClip demo_seq[] = {
    ASAG_CLIP_EMERGE,
    ASAG_CLIP_IDLE,
    ASAG_CLIP_LASER,
    ASAG_CLIP_SLAM,
    ASAG_CLIP_VOMIT,
    ASAG_CLIP_FAINT,
};
#define DEMO_SEQ_COUNT ((int)(sizeof(demo_seq) / sizeof(demo_seq[0])))

/* >>> THE WATCHDOG IS NOT PADDING. <<< src/asag.c's read_file() REFUSES a read
   that would reach the stack, and load_clip() refuses a .pva whose vertex count
   disagrees with the mesh. Either way the clip ends up with clip_count 0, and a
   clip with no frames never advances, so it never reports done and this cycle
   would stop dead on it — which looks exactly like the boss being frozen and
   says nothing about why.

   Ten seconds is comfortably longer than the longest clip (the 50-frame slam at
   8 fps is 6.25s) and short enough that a missing clip reads as a pause rather
   than a hang. If the demo visibly stalls for ten seconds on one clip, that
   clip did not load: check it is in disc.xml's TEXASAG dir and re-run
   tools/heap_budget.py. */
#define DEMO_WATCHDOG_FRAMES  600

static int demo_index;
static int demo_age;          /* game frames since the current clip started */

void asag_boss_reset(void) {
    demo_index = 0;
    demo_age   = 0;
    asag_set_visible(1);
    asag_stop();              /* the bind pose; update() starts the cycle */
}

static void start(int i) {
    demo_index = i;
    demo_age   = 0;
    asag_play(demo_seq[i], 0);   /* never looping — see demo_seq above */
}

void asag_boss_update(void) {
    /* Nothing to drive until the model is in. asag_play() on an absent model is
       harmless, but starting the cycle before the read has happened would burn
       the emerge against a body that is not there yet. */
    if (!asag_model_loaded()) return;

    if (asag_playing() == ASAG_CLIP_NONE) { start(0); return; }

    demo_age++;

    /* >>> THIS RUNS BEFORE asag_update(), AND THE ORDER IS WORTH ONE FRAME. <<<
       asag_update() is what sets the "holding the last frame" flag, so testing
       it here means the flag was raised on the PREVIOUS frame and the clip's
       final pose has already been drawn once. Call this AFTER asag_update()
       instead and the switch happens in the same frame the flag goes up, so
       every clip's last frame is skipped and each one ends a beat early. */
    if (asag_clip_done() || demo_age >= DEMO_WATCHDOG_FRAMES)
        start((demo_index + 1) % DEMO_SEQ_COUNT);
}
