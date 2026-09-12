#include <stdint.h>
#include "asag.h"
#include "asag_boss.h"

/* ASAG'S DIRECTOR — a DEMO. See asag_boss.h: this plays the six clips end to
   end on a loop so the animation can be looked at, and it is meant to be
   replaced wholesale by the real encounter. */

/* The cycle. ASAG_CLIP_* order, which is also tools/export_asag.py's bake
   order. EMERGE used to lead it and no longer exists: the travel it did by hand
   is now the position track in src/asag.c, which every attack carries for
   itself, so the loop starts on the IDLE instead.

   IDLE FIRST IS ALSO WHAT MAKES THE POSITION TRACK READ CORRECTLY. The idle
   INHERITS whatever offset it starts on rather than driving one (asag.h), and
   every attack ends its back-ramp at Home - so the cycle begins at Home,
   breathes there, and each attack lunges out of it and returns.

   Every one is played NON-LOOPING, which is what makes the chain work at all:
   a looping clip never reports asag_clip_done() and the cycle would stop on it
   forever. */
static const AsagClip demo_seq[] = {
    ASAG_CLIP_IDLE,
    ASAG_CLIP_LASER,
    ASAG_CLIP_SLAM,
    ASAG_CLIP_VOMIT,
    ASAG_CLIP_FAINT,
};
#define DEMO_SEQ_COUNT ((int)(sizeof(demo_seq) / sizeof(demo_seq[0])))

/* >>> AN ABSENT CLIP IS SKIPPED, NOT WAITED ON, AND THAT IS A DIAGNOSTIC AS
   MUCH AS A FIX. <<< src/asag.c's read_file() REFUSES a read that would reach
   the stack, and load_clip() refuses a .pva whose vertex count disagrees with
   the mesh. Both are SILENT: the clip ends up with clip_count 0, the body falls
   back to its bind pose, and because a clip with no frames never advances it
   never reports asag_clip_done() either.

   A cycle that just played such a clip would sit on the bind pose until the
   watchdog below fired - and a boss standing still on its rest pose for ten
   seconds looks EXACTLY like a clip that played and had no motion in it. That
   ambiguity cost a debugging session: the faint appeared to "play but not
   move", and telling "the clip is missing" from "the clip is dull" needed a
   stopwatch.

   So next_loaded() steps over anything asag_clip_loaded() disowns. The failure
   is then unmistakable in the room itself - the clip is simply ABSENT from the
   cycle - and it can no longer be mistaken for an animation problem.

   WHICH CLIP GOES MISSING IS NOT RANDOM: asags_load_model() reads them in
   AsagClip order and the heap is what runs out, so it is always the LAST ones
   in that order. If a clip vanishes from the cycle, the fix is the budget (PART
   6 of tools/ADDING_THE_ASAG_FIGHT.txt), not the art. */
static int next_loaded(int from) {
    for (int n = 1; n <= DEMO_SEQ_COUNT; n++) {
        int i = (from + n) % DEMO_SEQ_COUNT;
        if (asag_clip_loaded(demo_seq[i])) return i;
    }
    return -1;                /* nothing loaded at all - see start() */
}

/* >>> THE WATCHDOG IS STILL NOT PADDING. <<< next_loaded() covers a clip that
   is absent; this covers one that is PRESENT and still fails to finish, which
   is the case no table can predict. Ten seconds is comfortably longer than the
   longest clip (the 50-frame slam at 8 fps is 6.25s) and short enough to read
   as a pause rather than a hang. */
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
    if (i < 0) { asag_stop(); return; }   /* no clip loaded: hold the bind pose */
    demo_index = i;
    demo_age   = 0;
    asag_play(demo_seq[i], 0);   /* never looping - see demo_seq above */
}

void asag_boss_update(void) {
    /* Nothing to drive until the model is in. asag_play() on an absent model is
       harmless, but starting the cycle before the read has happened would burn
       the emerge against a body that is not there yet. */
    if (!asag_model_loaded()) return;

    /* Start on the first clip that is actually in memory, not blindly on
       demo_seq[0] - the emerge could be the missing one. */
    if (asag_playing() == ASAG_CLIP_NONE) {
        start(asag_clip_loaded(demo_seq[0]) ? 0 : next_loaded(0));
        return;
    }

    demo_age++;

    /* >>> THIS RUNS BEFORE asag_update(), AND THE ORDER IS WORTH ONE FRAME. <<<
       asag_update() is what sets the "holding the last frame" flag, so testing
       it here means the flag was raised on the PREVIOUS frame and the clip's
       final pose has already been drawn once. Call this AFTER asag_update()
       instead and the switch happens in the same frame the flag goes up, so
       every clip's last frame is skipped and each one ends a beat early. */
    if (asag_clip_done() || demo_age >= DEMO_WATCHDOG_FRAMES)
        start(next_loaded(demo_index));
}
