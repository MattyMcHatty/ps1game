#ifndef ASAG_H
#define ASAG_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   ASAG — the body. The game's SECOND boss.
   =========================================================================
   >>> THIS FILE IS THE MODEL AND THE ANIMATION SYSTEM. IT IS NOT THE FIGHT. <<<
   Nothing in here decides anything: no health, no phases, no attacks, no
   targeting, no damage. The model loads onto its BIND POSE and stays there
   until something calls asag_play(). What drives it TODAY is src/asag_boss.c,
   which is a DEMO and not an encounter — it plays the six clips end to end on a
   loop so the animation can be looked at. When the real fight is written, that
   file is what gets replaced; this one should not need to change.

   ---- ONE PART, WHERE THE FIRST ASAG HAD EIGHT -----------------------------
   The first version of this boss was eight separate meshes on eight armatures —
   a head, four leaves, two tentacles and a sheet of boils — with fourteen clips
   between them and a clock per part, so an arm could strike while the head
   spoke. All of that is gone. Asag Version Two is ONE mesh of 80 vertices
   driven by ONE armature, with six clips that are ALTERNATIVES rather than
   layers: exactly one of them plays at a time.

   That is why this file has no AsagPart enum any more. If a later version
   splits the body up again, the thing to restore is the per-part ARRAY — the
   clip table already carries everything else.

   ---- POSITIONS ARE BAKED. THERE IS NO MODEL MATRIX. -----------------------
   src/rabisu.c places its boss from a world position it holds itself, and its
   draw therefore composes a model matrix onto the view every frame and has to
   restore the view afterwards (tools/ADDING_A_3D_ENEMY.txt STEP 5).

   Asag does not move. It is rooted in the rock behind the arena, and the mesh
   was exported from Blender with `matrix_world` applied — so the .smd's
   vertices are ALREADY in the arena's own coordinate space, exactly like the
   room mesh's. The draw therefore loads NO matrix of its own and restores
   nothing: it runs under the view asag_arena_draw() has already set, and the
   body lands where the artist put it. The bind pose spans

       x[-168, 146]   y[-960, -528]   z[2262, 3931]

   which is the MODEL DATA and not a constant anywhere in this source. >>> SO
   THERE IS NOTHING HERE TO "PLACE". <<< If the boss is in the wrong spot it is
   in the wrong spot in the .blend, and the fix is a re-export
   (tools/export_asag.py), not a number in this file. It also means a director
   cannot move the body by setting a position — movement is the clip's job.

   Note what those bounds say about the SHAPE of the encounter: the body's
   lowest point is 528 units ABOVE the y=0 floor (y is negative-up), and it
   reaches 1000 units further back in z than the arena's own back wall. Asag
   hangs over the arena out of the rock behind it. It does not stand on the
   floor, and the player cannot walk into it — which is also why the collision
   generated from this same mesh never fires at body height. See the head of
   tools/gen_asag_arena_collision.py.

   ---- MEMORY: ROOM-SCOPED, AND THE FIGHT'S WHOLE BUDGET --------------------
   asags_load_model() is called from main.c's STATE_LOADING when the destination
   is STATE_ASAG_ARENA, and asags_free_model() on every OTHER transition — the
   exact shape rabisus_load_model() has, for the reasons in
   tools/ADDING_THE_ASAG_FIGHT.txt PART 6. Never a startup load: a boss that
   lives in one room must not be resident in the other twenty-six.

       1 mesh, 80 verts                  4,468 bytes ->   6,144 sector-rounded
       6 clips, 196 frames, PACKED      94,152 bytes -> 102,400
                                                      ---------
                                                        108,544 at the door

   >>> AND THE DOOR HAS 121-147 KB, NOT 171. <<< That is a real measurement and
   it was paid for. tools/heap_budget.py long quoted "~171 KB free at the arena
   door", a figure inferred once during the FIRST Asag and never re-checked.
   This boss pinned it: the seven reads go in a fixed order, and on hardware the
   run SUCCEEDED through a cumulative 112,640 bytes and was REFUSED at 139,264 —
   so the truth is somewhere in [121 KB, 147 KB).

   >>> THE REFUSAL WAS SILENT, AND THAT IS THE LESSON. <<< read_file() below
   declines an allocation that would reach the stack, which is exactly right and
   far better than the alternative (the first Asag died inside CdReadSync with
   $sp INSIDE the buffer being read). But a declined clip does not crash and
   does not log. It is simply ABSENT: the body falls back to its bind pose, and
   because a clip with no frames never advances it never reports
   asag_clip_done() either. The faint went missing this way and looked for all
   the world like an animation with no motion in it. src/asag_boss.c now steps
   over any clip asag_clip_loaded() disowns, so the next one is obvious.

   WHAT BOUGHT THE ROOM: the clips are PACKED (PVA2, three int16 per vertex)
   rather than PVA1's four, whose fourth halfword is always zero. That pad was
   a quarter of the payload — 30,720 bytes across six clips, more than dropping
   the whole boss from 8 fps to 6 would have saved, and it costs nothing
   visually. See load_clip() and body_verts() in the .c.

   The clips are also baked at a THIRD of the authored 24 fps
   (tools/export_asag.py, STEP 3) and play at 8. Re-baking one by hand WITHOUT
   --step 3, or through the Blender UI (which still writes unpacked PVA1), puts
   this back over the line.

   The transition also loads the model AFTER the room's texture stream rather
   than before it, so the stream's 20 KB scratch is freed before the big read
   starts and the peak is the larger of the two rather than their sum.
   ========================================================================= */

/* The clips. Exactly one plays at a time; starting one replaces whatever was
   playing. The order is the order tools/export_asag.py's CLIPS table bakes them
   in AND the order src/asag_boss.c cycles them in, so a clip added here is
   added in three places that are meant to stay in step.

   The frame counts are POST-BAKE at step 3 — a third of the authored ones. */
typedef enum {
    ASAG_CLIP_NONE = -1,

    ASAG_CLIP_IDLE = 0,      /* 10f   the resting bob                         */
    ASAG_CLIP_EMERGE,        /* 13f   rises out of the rock                   */
    ASAG_CLIP_LASER,         /* 43f   the beam attack                         */
    ASAG_CLIP_SLAM,          /* 50f   the slam attack                         */
    ASAG_CLIP_VOMIT,         /* 40f   the spew attack                         */
    ASAG_CLIP_FAINT,         /* 40f   collapses                               */

    ASAG_CLIP_COUNT
} AsagClip;

/* >>> THE PLAYED RATE, AND WHY IT IS AN fps AND NOT A TICK COUNT. <<< The clips
   are baked at a third of the authored 24 (tools/export_asag.py, STEP), so they
   play at 8 — and 60/8 is 7.5, which is not a whole number of game frames. The
   first Asag got away with `#define ASAG_ANIM_TICKS 10` because its 6 fps
   divides 60; this one cannot. A rounded 7 runs every clip 7% fast and a
   rounded 8 runs it 6% slow, and over a 50-frame slam that is most of a second.

   So asag_update() ACCUMULATES instead: it adds this many units per game frame
   and steps one animation frame whenever the total passes 60. That is exact on
   average, it costs one add and one compare, and it stays exact for whatever
   rate a later bake picks. Change it only together with STEP in
   tools/export_asag.py. */
#define ASAG_ANIM_FPS  8

/* ---- Lifetime. Both are idempotent and both are called unconditionally from
   main.c's STATE_LOADING, keyed on pending_area, exactly like the Rabisu's and
   like sound_bank_select(). The FREE goes BEFORE the sound-bank swap and the
   LOAD after it — see tools/ADDING_THE_ASAG_FIGHT.txt PART 6, where getting
   that order wrong cost a crash at the Garden Courtyard's door. ----------- */
void asags_load_model(void);
void asags_free_model(void);

/* Visible, on the bind pose, clock stopped. Called from asag_arena_init(), i.e.
   on EVERY arrival, so a debug level-select jump finds the same state a real
   drop down the shaft does. */
void asag_reset(void);

/* 1 once the mesh is in. The draw is a no-op without it and so is every
   asag_play(), so nothing has to test this — it is here for a director's
   benefit and for debug overlays. */
int  asag_model_loaded(void);

/* Advance the playing clip by one game frame. Call from the arena's free-play
   update branch in main.c. Safe with nothing loaded and safe with nothing
   playing. */
void asag_update(void);

/* Draw the body. Runs under whatever view matrix is loaded and does not touch
   the GTE's matrices, so it must be called from asag_arena_draw() AFTER
   camera_build_view() — which is where the runbook already puts it. */
void asag_draw(RenderContext *ctx);

/* ---- The clip API. THIS IS THE WHOLE OF WHAT A DIRECTOR DRIVES. ---------- */

/* Start `clip` from frame 0.
   `loop` : 1 to repeat forever, 0 to HOLD the last frame when it ends.
   Holding rather than snapping back to the bind pose is what lets a one-shot
   leave the body where the animator left it, and it is why asag_clip_done()
   exists as a separate question from "is something playing".
   Starting the clip already playing RESTARTS it. */
void asag_play(AsagClip clip, int loop);

/* Back to the .smd bind pose, clock stopped. That pose is frame 1 of the IDLE
   clip, so this is visually the same as holding still on the idle's first frame
   — tools/export_asag.py guarantees that alignment, and the hand export this
   boss arrived as did NOT have it (it was saved with the faint action active
   and sat up to 164 units off). See that script. */
void asag_stop(void);

/* What is playing, or ASAG_CLIP_NONE for the bind pose. */
AsagClip asag_playing(void);

/* 1 if this clip's .pva actually made it into memory. A clip can be ABSENT for
   two reasons and both are silent: read_file() refused the allocation because it
   would have reached the stack (see the .c), or load_clip() rejected a .pva
   whose vertex count disagreed with the mesh. Either way the body falls back to
   its bind pose and simply never animates.

   >>> A DIRECTOR THAT CHAINS CLIPS MUST ASK THIS. <<< An absent clip has no
   frames, so it never advances and never reports asag_clip_done() - a sequence
   that waits on it stalls forever. src/asag_boss.c uses this to step over one
   rather than hang on it, which also makes the failure OBVIOUS (the clip is
   missing from the cycle) instead of looking like a boss that froze. */
int  asag_clip_loaded(AsagClip clip);

/* 1 when a non-looping clip has reached its last frame and is holding it.
   Always 0 for a looping clip and on the bind pose — so a director can poll
   this to chain a sequence without a timer of its own. */
int  asag_clip_done(void);

/* Hide the body without unloading it. It starts VISIBLE; a real reveal will
   want it hidden until it emerges. */
void asag_set_visible(int visible);
int  asag_visible(void);

#endif /* ASAG_H */
