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
    ASAG_CLIP_LASER,         /* 43f   the beam attack                         */
    ASAG_CLIP_SLAM,          /* 50f   the slam attack                         */
    ASAG_CLIP_VOMIT,         /* 40f   the spew attack                         */
    ASAG_CLIP_FAINT,         /* 40f   collapses                               */

    ASAG_CLIP_COUNT
} AsagClip;

/* There WAS an ASAG_CLIP_EMERGE, 13 frames, and it is gone at the user's
   request: the travel it did by hand is now done by the position track below,
   which every attack carries for itself. Its .pva is deleted, its disc.xml row
   is gone and tools/export_asag.py no longer bakes Head_Emerge_Baked — the
   action is still in the .blend if it is ever wanted back. */

/* =========================================================================
   POSITION: HOME AND EMERGED
   =========================================================================
   >>> THE CLIPS DO NOT TRAVEL. THE BODY IS SLID UNDERNEATH THEM. <<<
   Every .pva is authored around ONE resting place — HOME, the fully retracted
   position, which is simply the baked coordinates with nothing added. The
   attacks are supposed to lunge into the arena and withdraw again, and that
   lunge is NOT in the animation data: it is a straight translation along Z that
   this module ramps in and out underneath whatever pose the clip is holding.

   That is the cheap way round and it is also the flexible one. Re-timing the
   lunge is a number in clip_move[] in the .c; re-timing it in Blender would be
   a re-bake of every clip, and a clip that travels can only ever travel the one
   distance it was baked with.

   ---- WHERE "EMERGED" COMES FROM. IT IS DERIVED, NOT CHOSEN. ---------------
   The brief was "his tail-end polys half-in, half-out of the wall". Both halves
   of that are measurable:

     THE TAIL-END POLYS are the rear-most cluster of the mesh — the four
     primitives over vertices 0..7, which span z[3607, 3931] in the bind pose.
     Their midpoint is z = 3769.

     THE WALL is the arena's back face at z = 2800. The mesh has an ALCOVE cut
     into it there (mouth at z=2800, back plate at z=2900, opening
     x[-215.5, 215.5] y[-1015.5, -584.5]) which is Asag's throat; 2800 is the
     surface the player actually sees.

   So the offset that lands the tail's midpoint on the wall is 2800 - 3769:

       HOME       z[2262, 3931]      tail z[3607, 3931]
       EMERGED    z[1293, 2962]      tail z[2638, 2962]   straddling 2800

   which leaves 76 of the 80 vertices in front of the wall (38 at Home) and
   stops 993 units short of the player's landing spot at z=300.

   >>> IT IS Z ONLY. <<< The brief said one axis and the geometry agrees: the
   body is already centred on the alcove in X (x[-168, 146] inside an opening of
   x[-215.5, 215.5]), so any X or Y component would push it into the rock.

   NOTE THE SIGN. Emerging means DECREASING z, because the arena runs from the
   player's landing at z=300 up to the back wall at z=2800. Comments elsewhere
   in this room call low z "north"; the brief called this direction "south". The
   geometry is what is implemented and it is not ambiguous — there is exactly
   one direction that puts the tail through the wall.
   ========================================================================= */
#define ASAG_EMERGE_DZ  (-969)

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

/* Start `clip` PART WAY IN — `start_ticks` GAME FRAMES from its beginning, so
   "two seconds into the faint" is 120 and needs no conversion here.

   >>> TICKS AND NOT ANIMATION FRAMES, BECAUSE A CLIP IS TWO TRACKS. <<< The
   baked pose steps at ASAG_ANIM_FPS and the position ramp runs at 60 (see
   ASAG_EMERGE_DZ above). Seeking one without the other would put the body in a
   mid-clip POSTURE at its resting POSITION, which on a reveal is exactly the
   frame the player is looking at. Ticks are the unit both tracks share.

   A seek past the clip's end clamps and comes up HELD, i.e. in the state a
   finished one-shot is in, so asag_clip_done() is true immediately rather than
   never. asag_play(clip, loop) is this with start_ticks 0. */
void asag_play_at(AsagClip clip, int loop, int32_t start_ticks);

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

/* ---- Hold everything where it is ------------------------------------------
   1 = asag_update() does nothing at all. This is the `frozen` field
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 5 lists among the four a director
   writes, and it exists here because the opening scene needs Asag to SIT two
   seconds into his faint while the camera falls down the shaft and lands —
   "currently sitting in the faint position" is a pose being held, not a clip
   being played.

   >>> IT STOPS THE POSITION TRACK TOO, WHICH IS THE POINT WORTH KNOWING. <<<
   asag_update() deliberately runs the travel clock on past the end of a held
   clip, so a back-ramp always reaches Home (see the .c). That is right for a
   clip that has finished and wrong for one a director is holding: without the
   freeze covering both, Asag would go on sliding out of the wall underneath a
   motionless pose.

   Cleared by asag_reset(), i.e. on every arrival, so a hold cannot leak into
   the next visit. */
void asag_set_frozen(int frozen);
int  asag_frozen(void);

/* ---- Solidity -------------------------------------------------------------
   Push the player out of the body, from THIS FRAME'S POSE AND THIS FRAME'S
   POSITION. Called from apply_collision_reception() alongside every other prop
   family, and area-gated inside, so the call is unconditional.

   >>> THE COLLISION USED TO BE 26 BAKED WALLS AND THEY HAD TO GO. <<<
   tools/gen_asag_arena_collision.py used to merge the boss's own geometry into
   src/asag_arena_mesh_collision.c as static walls. That was defensible while
   the body never moved: it sat at y[-960,-528], 528 above the floor and 342
   above the player's eye, so collision.c's vertical gate rejected every one of
   them and they were harmless placeholders for the day the art came down.

   The day came. The position track slides the body up to 969 units into the
   arena and the slam and the faint bring it to floor level, so a table baked
   from the bind pose is now wrong in both axes at once — and CollisionRoom is a
   FIXED table read by every movement query in the frame, so it cannot be
   re-baked per frame. The walls are gone from the generator and this replaces
   them, which is the move src/hatch_doors.c already made for its leaves.

   >>> IT IS AN AABB OVER ONLY THE VERTICES AT THE PLAYER'S HEIGHT, AND THAT IS
   WHAT MAKES ONE BOX ENOUGH. <<< A box round the WHOLE body would be 1669 long
   in Z and would wall off a third of the arena whenever Asag leaned in, most of
   it nowhere near the floor. Selecting first on Y and only then taking the
   footprint means the box is empty for most of the fight — the idle, the laser
   and the vomit never reach body height at all — and when it is not empty it is
   small: the slam's worst frame is 38 vertices inside 314 x 626, the faint's is
   22 inside 251 x 266.

   `radius` is the player's, added to the box Minkowski-style. See the .c for
   why the push is smallest-penetration here where the hatch doors' could not
   be. */
void asag_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* ---- Where the body is, for a camera ---------------------------------------
   The centre of an AABB over EVERY posed vertex, this frame — pose and position
   both, out of the same block the draw uses. A director pans with this.

   >>> IT IS DELIBERATELY NOT asag_collide's BOX. <<< That one selects on Y
   first, because a collider wants only the part of Asag at the player's height.
   A camera wants the whole silhouette: what it is framing is the animal, not
   its footprint.

   And it is worth asking rather than computing: a director could aim at the
   bind centre plus some guess at how far along ASAG_EMERGE_DZ the ramp is, and
   would be wrong about the pose as well (the faint drops the body to the floor,
   the idle bobs) and would be re-deriving a ramp this module owns.

   Returns 0 with `out` untouched when there is nothing posed. A camera given 0
   should hold its last aim, not swing to the origin. */
int asag_body_centre(VECTOR *out);

/* ---- ...and where his FACE is, which is what a camera usually wants --------
   The centroid of the posed vertices at the front-most end of him. Asag lies
   along Z with his head at the LOW end (the bind pose spans z[2262,3931] and
   the tail cluster ASAG_EMERGE_DZ is derived from is the high end), so the
   front in Z is the head.

   >>> PREFER THIS TO asag_body_centre() FOR ANY SHOT OF HIM. <<< He is 1669
   units long; his centre is his flank. The opening scene aims at the face
   throughout for exactly that reason, and the difference is not subtle — at
   Home the face sits 800 units nearer the player than the centre does.

   It is defined by a Z WINDOW rather than a vertex count, which matters
   because the count version is unstable the moment the body deforms. The .c
   has the measurement.

   Returns 0 with `out` untouched when there is nothing posed. */
int asag_face_point(VECTOR *out);

#endif /* ASAG_H */
