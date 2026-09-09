#ifndef ASAG_H
#define ASAG_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   ASAG — the body. The game's SECOND boss, and its first MULTI-PART one.
   =========================================================================
   >>> THIS FILE IS THE MODEL AND THE ANIMATION SYSTEM. IT IS NOT THE FIGHT. <<<
   Nothing in here decides anything: no health, no phases, no attacks, no
   targeting, no damage. Every part loads onto its BIND POSE and STAYS there
   until something calls asag_play(). That is deliberate — the director module
   (src/asag_boss.c, which does not exist yet) is what will drive it, and
   tools/ADDING_A_BOSS_ENCOUNTER.txt is its runbook.

   What this file gives that director is: eight drawable parts standing in the
   right places, fourteen clips it can start and stop by name, and a clock per
   part so two parts can be mid-different-clips at once.

   ---- WHY EIGHT PARTS AND NOT ONE MODEL -----------------------------------
   Asag is authored in Blender as separate objects on separate armatures — a
   head in the alcove, four leaves around it, an arm down each side wall, and
   six boils on the back wall. They animate INDEPENDENTLY (the head can speak
   while an arm strikes), and a baked-vertex clip covers a whole mesh, so one
   joined model would mean one clip per COMBINATION. Eight meshes with eight
   clocks is the cheap version of that.

   The one exception is the BOILS, which are joined into a single mesh: they
   never animate, they are always drawn together, and six separate files would
   have cost six ISO9660 records in a \TEX\ directory that has crashed this game
   before by growing (see src/chain_room.c). Their prim order is Boil One..Six,
   five quads each, so a single boil is still addressable inside the one mesh.

   ---- POSITIONS ARE BAKED. THERE IS NO PER-PART MATRIX. --------------------
   >>> THIS IS THE THING THAT MAKES THIS MODULE SO MUCH SIMPLER THAN
   src/rabisu.c, AND IT IS WORTH KNOWING WHY IT IS ALLOWED. <<<

   The Rabisu FLIES AROUND. Its anchor moves, it turns to face the player, and
   its draw therefore composes a model matrix onto the view every frame and has
   to restore the view afterwards (tools/ADDING_A_3D_ENEMY.txt STEP 5).

   Asag does not move. It is rooted in the arena's back wall, and every one of
   its parts was exported from Blender with `matrix_world` applied — so the
   .smd's vertices are ALREADY in the arena's own coordinate space, exactly like
   the room mesh's. The draw therefore loads NO matrix of its own and restores
   nothing: it runs under the view asag_arena_draw() has already set, and the
   parts land where the artist put them.

   That also means there is nothing here to "place". If a part is in the wrong
   spot, it is in the wrong spot in the .blend, and the fix is a re-export
   (tools/export_asag.py), not a constant in this file.

   >>> AND IT MEANS THE DIRECTOR CANNOT MOVE A PART BY SETTING A POSITION. <<<
   Movement is the clip's job. If the fight ever needs a part to travel
   somewhere the animation does not take it, that part needs a real model matrix
   and this comment stops being true.

   ---- MEMORY: ROOM-SCOPED, AND THE FIGHT'S WHOLE BUDGET -------------------
   asags_load_model() is called from main.c's STATE_LOADING when the destination
   is STATE_ASAG_ARENA, and asags_free_model() on every OTHER transition — the
   exact shape rabisus_load_model() has, for the reasons in
   tools/ADDING_THE_ASAG_FIGHT.txt PART 6. Never a startup load: a boss that
   lives in one room must not be resident in the other twenty-six.

       8 part meshes                    21,940 bytes
       14 clips                         95,048 bytes
                                      -----------
                                       116,988 bytes, ~119 KB sector-rounded

   >>> AGAINST 171 KB, NOT 371 KB, AND THE DIFFERENCE COST THIS BOSS A CRASH.
   <<< tools/heap_budget.py used to report 371 KB free at rest because it treated
   the whole span from _end to the top of RAM as heap. The top 145 KB of that is
   main()'s RenderContext, which is a LOCAL — two 64 KB packet buffers and two
   8 KB ordering tables sitting on the stack for the life of the run. Real free
   at rest is ~208 KB; real free at the moment this room loads, measured, is
   ~171 KB.

   The first build of this boss was sized against the phantom figure: 229 KB of
   meshes and clips at half rate. It crashed inside CdReadSync every single time,
   with the stack pointer INSIDE the buffer being read. The clips are now baked
   at a QUARTER of the authored 24 fps (tools/export_asag.py, STEP 4) and play at
   6, so ASAG_ANIM_TICKS is 10. Re-baking a clip by hand WITHOUT --step 4 will
   not fit, and "will not fit" means the CD DMA writing through the stack, not a
   failed malloc — read_file() in the .c now refuses such a read outright.

   The transition also loads the model AFTER the room's texture stream rather
   than before it, so the stream's 20 KB scratch is freed before the big read
   starts and the peak is the larger of the two rather than their sum.
   ========================================================================= */

/* The drawable parts. The order is the load order and the DRAW order, and the
   draw order is deliberate: the boils are flat against the back wall, the head
   and leaves stand in front of them, and the arms are furthest forward. Every
   primitive is sorted into the OT by its own depth so this does not decide what
   covers what — but it does decide which part wins a tie. */
typedef enum {
    ASAG_PART_BOILS = 0,     /* six, joined; five quads each, Boil One..Six    */
    ASAG_PART_BODY,          /* the head                                       */
    ASAG_PART_LEAF_TOP,
    ASAG_PART_LEAF_BOTTOM,
    ASAG_PART_LEAF_LEFT,
    ASAG_PART_LEAF_RIGHT,
    ASAG_PART_TENT_L,
    ASAG_PART_TENT_R,
    ASAG_PART_COUNT
} AsagPart;

/* The clips. Each belongs to exactly one part; starting one replaces whatever
   that part was playing and leaves every other part alone.

   THE FOUR LEAF CLIPS ARE THE SAME ANIMATION ON FOUR DIFFERENT MESHES, not one
   clip shared four ways. A .pva holds world-space positions, and the four
   leaves sit at four different places around the mouth, so they cannot share a
   file — 3,612 bytes each is the price of that and it is already counted. */
typedef enum {
    ASAG_CLIP_NONE = -1,

    ASAG_CLIP_HEAD_IDLE = 0, /*  8f, loops — the resting bob                   */
    ASAG_CLIP_HEAD_SPEAK,    /*  9f          the mouth working, for dialogue    */
    ASAG_CLIP_HEAD_EMERGE,   /* 10f          rises out of the alcove            */
    ASAG_CLIP_HEAD_RETRACT,  /* 10f          drops back into it                 */
    ASAG_CLIP_HEAD_VOMIT,    /* 30f, loops   the long spew attack               */
    ASAG_CLIP_HEAD_LASER,    /* 33f, loops   the long beam attack               */

    ASAG_CLIP_LEAF_TOP,      /*  8f          open/close                        */
    ASAG_CLIP_LEAF_BOTTOM,
    ASAG_CLIP_LEAF_LEFT,
    ASAG_CLIP_LEAF_RIGHT,

    ASAG_CLIP_TENT_L_IDLE,   /* 15f, loops   the arm breathing where it lies    */
    ASAG_CLIP_TENT_L_ATTACK, /* 15f, loops   rears up and strikes               */
    ASAG_CLIP_TENT_R_IDLE,
    ASAG_CLIP_TENT_R_ATTACK,

    ASAG_CLIP_COUNT
} AsagClip;

/* Game frames per animation frame. The clips are baked at a QUARTER of the
   authored 24 fps (see the header), so they play at 6 and 60/6 = 10. This is
   NOT 60/24 — using the authored rate here plays every clip four times too
   fast. Change it only together with STEP in tools/export_asag.py. */
#define ASAG_ANIM_TICKS  10

/* ---- Lifetime. Both are idempotent and both are called unconditionally from
   main.c's STATE_LOADING, keyed on pending_area, exactly like the Rabisu's and
   like sound_bank_select(). The FREE goes BEFORE the sound-bank swap and the
   LOAD after it — see tools/ADDING_THE_ASAG_FIGHT.txt PART 6, where getting
   that order wrong cost a crash at the Garden Courtyard's door. ---------- */
void asags_load_model(void);
void asags_free_model(void);

/* Every part visible, every part on its bind pose, every clock stopped. Called
   from asag_arena_init(), i.e. on EVERY arrival, so a debug level-select jump
   finds the same state a real drop down the shaft does. */
void asag_reset(void);

/* 1 once the meshes are in. The draw is a no-op without them and so is every
   asag_play(), so nothing has to test this — it is here for the director's
   benefit and for debug overlays. */
int  asag_model_loaded(void);

/* Advance every playing clip by one game frame. Call from the arena's free-play
   update branch in main.c. Safe to call with nothing loaded and safe to call
   with nothing playing; a part on its bind pose costs one compare. */
void asag_update(void);

/* Draw all eight parts. Runs under whatever view matrix is loaded and does not
   touch the GTE's matrices, so it must be called from asag_arena_draw() AFTER
   camera_build_view() — which is where the runbook already puts it. */
void asag_draw(RenderContext *ctx);

/* ---- The clip API. THIS IS THE WHOLE OF WHAT THE DIRECTOR DRIVES. -------- */

/* Start `clip` on its own part, from frame 0.
   `loop` : 1 to repeat forever, 0 to hold the LAST frame when it ends.
   Holding the last frame rather than snapping back to the bind pose is what
   lets HEAD_RETRACT leave the head down, and it is why asag_clip_done() exists
   as a separate question from "is it playing".
   Starting a clip on a part already playing that same clip RESTARTS it. */
void asag_play(AsagClip clip, int loop);

/* Put a part back on its .smd bind pose and stop its clock. For every part but
   the head that pose is frame 1 of its idle clip, so this is visually the same
   as holding still (tools/export_asag.py guarantees that alignment). */
void asag_stop(AsagPart part);
void asag_stop_all(void);

/* What a part is playing, or ASAG_CLIP_NONE for the bind pose. */
AsagClip asag_playing(AsagPart part);

/* 1 when a non-looping clip has reached its last frame and is holding it. Always
   0 for a looping clip and for a part on its bind pose — so the director can
   poll this to chain a sequence without a timer of its own. */
int  asag_clip_done(AsagPart part);

/* Hide a part without unloading it. Every part starts VISIBLE; the reveal will
   want the head hidden until it emerges, and popping a boil is a hide too. */
void asag_set_visible(AsagPart part, int visible);
int  asag_visible(AsagPart part);

#endif /* ASAG_H */
