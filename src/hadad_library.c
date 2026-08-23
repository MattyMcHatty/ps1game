#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "door.h"              /* door_draw_string_3d, DOOR_PIXEL_SIZE */
#include "btn_glyph.h"         /* BTN_CIRCLE */
#include "hadad.h"
#include "hadad_library.h"
#include "library_destroyed.h" /* library_destroyed_doors_arm on the handover */

/* The Library Destroyed encounter's director. Read hadad_library.h first: the
   beat sheet and the reason this is not in hadad.c are both there. */

/* ---- The gap ---------------------------------------------------------------
   The collapsed shelving fills x[-1303,-350] z[-2080,-759] as one solid block
   (library_destroyed.h), and collision wall 4 is its west face at x=-1303 with
   NO break in it anywhere along z. So the crawl is scripted end to end: there is
   no hole in the collision for the player to walk through and there is not meant
   to be one. The camera passes through solid geometry for six seconds, which is
   only safe because the cutscene branch in main.c skips apply_collision and
   apply_height entirely for the duration.

     GAP    the mouth, on the block's west face, level with the far side.
     EXIT   the room's south-east corner, in the east aisle — the user's point.
            Checked against the aisle's own push-out: collision wall 12 holds a
            195-radius player to x >= -155 and wall 0 to x <= 155, so -42 is 113
            clear of the nearer of the two and control does not come back on a
            boundary (ADDING_A_BOSS_ENCOUNTER.txt mistake 10).

   The two share a Z, so the crawl is due east and the yaw never changes through
   it — which is what lets the middle leg be a straight lerp with nothing to
   solve per frame. */
#define HL_GAP_X          (-1303)
#define HL_GAP_Z          (-1833)
#define HL_EXIT_X            (-42)
#define HL_EXIT_Z          (-1833)

/* Eye heights. This room's single floor plane is y=0 and apply_height stands the
   player at floor - GROUND_FLOOR_Y - 40 = -189, which is the height control has
   to come back at or the first frame of free play is a drop. The crawl height is
   the "duck slightly" — 90 above the floor against a 189 stand, so a little over
   half, which is low enough to read as going under something and high enough
   that the shelving does not fill the whole frame. */
#define HL_STAND_Y         (-189)
#define HL_CRAWL_Y          (-90)

/* Yaw. 1024 is +X (east, along the gap); 0 is +Z (north, up the east aisle
   toward the double door). The stand-up turns from one to the other, so control
   comes back pointing at the only exit rather than at the wall the player has
   just crawled out of. */
#define HL_CRAWL_ROT        1024
#define HL_EXIT_ROT            0

/* Six seconds, as asked for, in frames at 60 fps. The middle number is the one
   that matters: 1261 units of gap over 300 frames is 4.2 a frame, a third of a
   walk, which is the pace the whole beat is for. Retime the crawl and the speed
   moves with it — there is no separate rate constant to forget. */
#define HL_T_DUCK             30   /* 0.5 s: duck and slide to the mouth */
#define HL_T_CRAWL           300   /* 5 s under the block, at a constant rate */
#define HL_T_RISE             30   /* 0.5 s: stand up and turn north */

/* The prompt. The player reads it from -X (they are in the west aisle, held to
   x <= -1498 by the block's own face), so the sign stands 11 proud of the face
   on that side and takes mirror=1 in the YZ plane — the west door's sign
   inverted. door_draw_string_3d adds 200 to the reading axis before centring,
   hence the -200 on the Z.

   The text TOP at y=-120 puts the glyphs (7 rows of DOOR_PIXEL_SIZE = 28 units)
   at about knee height, on the gap rather than floating over it.

   Radii copied from this room's two doors so the prompt behaves like every other
   prompt in the game: visible from 1500, fading in over the last 500, live
   within 500 Manhattan of an interaction point 65 west of the face. */
#define HL_SIGN_X         (HL_GAP_X - 11)
#define HL_SIGN_Y           (-120)
#define HL_INTERACT_X     (HL_GAP_X - 65)
#define HL_INTERACT_Z      HL_GAP_Z
#define HL_TEXT_RADIUS      1500
#define HL_FADE_NEAR        1000
#define HL_TRIGGER_RADIUS    500

typedef enum {
    HL_IDLE,    /* free play. The prompt is live iff Hadad is in the room */
    HL_DUCK,
    HL_CRAWL,
    HL_RISE,
    HL_DONE,    /* free play again, and the gap is never offered a second time */
} HlState;

static HlState state = HL_IDLE;
static int32_t anim_t;
static int32_t src_x, src_y, src_z, src_rot, src_pitch, rot_delta;
static int     interact_prev = 1;   /* Circle edge-detect, seeded "held" */

/* >>> LATCHED, NOT RE-DERIVED. <<< ADDING_A_BOSS_ENCOUNTER.txt STEP 4: a script
   that re-asks "which Hadad is this" every frame tears itself down on the frames
   the answer is legitimately NULL. Nothing can empty the slot during the six
   seconds this is held — update_hadads does not even run — but the handover at
   the end is the one call that must not miss, so it is taken from here. */
static Hadad *crawl_boss = NULL;

static void hl_park(void) {
    state         = HL_IDLE;
    anim_t        = 0;
    interact_prev = 1;
    crawl_boss    = NULL;
}

void hadad_library_enter(void) { hl_park(); }
void hadad_library_reset(void) { hl_park(); }

int hadad_library_cutscene(void) {
    return state == HL_DUCK || state == HL_CRAWL || state == HL_RISE;
}

/* The door is dead from the APPEARANCE, not from the arrival — see
   hadad_library_present(). Asked of the enemy rather than latched here so that
   killing him (a hundred crucifaxe swings, which is allowed) hands the door
   back, and so that a debug jump into the room with nobody in it leaves both
   doors working. */
int hadad_library_seals_sdoor(void) { return hadad_library_present(); }

/* Shortest signed turn from `from` to `to`, in 4096ths. Lerping the raw angle
   instead would swing a 1024 -> 0 turn the long way round. */
static int32_t turn_delta(int32_t from, int32_t to) {
    int32_t d = ((to - from) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    return d;
}

/* 0..256 ease-out, the piano puzzle's, shared by the duck and the stand. The
   crawl between them deliberately does NOT use it. */
static int32_t hl_ease(int32_t t, int32_t total) {
    int32_t p = t * 256 / total; if (p > 256) p = 256;
    int32_t inv = 256 - p;
    return 256 - (inv * inv / 256);
}

static void hl_start(void) {
    /* Take the camera and PIN THE PLAYER where they stood. Hadad is walking the
       west aisle at them while this runs; if he read cam_* he would follow the
       camera into the crawl and come out the far side with it. He does not — he
       reads player_x/y/z() — and this is the call that makes that mean
       something (camera.h). */
    camera_look_cancel();
    camera_anchor_player(cam_x, cam_y, cam_z);

    src_x = cam_x; src_y = cam_y; src_z = cam_z;
    src_rot = cam_rot; src_pitch = cam_pitch;
    rot_delta = turn_delta(src_rot, HL_CRAWL_ROT);

    crawl_boss = hadad_library_instance();
    anim_t = 0;
    state  = HL_DUCK;
}

/* Control comes back. The release is the teleport: player_x/y/z() go back to
   reading cam_*, and cam_* is standing in the south-east corner. */
static void hl_finish(void) {
    cam_x   = HL_EXIT_X;
    cam_y   = HL_STAND_Y;
    cam_z   = HL_EXIT_Z;
    cam_rot = HL_EXIT_ROT;
    cam_vy  = 0;
    cam_pitch = 0;              /* free-look never clears this for you */
    camera_release_player();

    /* A Circle still down at the handover would otherwise resolve as an
       interact on release and walk the player straight back out of the west
       door. Spend the press, then re-seed both doors' own edge state — theirs
       has not been polled for six seconds and is stale either way. */
    interact_spend_press();
    library_destroyed_doors_arm();
    interact_prev = 1;

    /* Put him on the door the player was running for and start his second walk.
       NULL only if a debug jump emptied the slot mid-crawl; the camera has
       already been given back by then, which is the part that matters. */
    hadad_library_begin_return(crawl_boss);
    crawl_boss = NULL;
    state      = HL_DONE;
}

void hadad_library_update(void) {
    if (state == HL_DONE) return;

    if (state == HL_IDLE) {
        /* ARMED LAZILY, off the enemy's own state: library_destroyed_init()
           runs before world_enter() places anybody, so anything decided at init
           time is decided a frame too early. */
        if (!hadad_library_present()) { interact_prev = interact_tapped(); return; }

        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        if (!just) return;

        int32_t dx = cam_x - HL_INTERACT_X;
        int32_t dz = cam_z - HL_INTERACT_Z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz < HL_TRIGGER_RADIUS && interact_facing(HL_INTERACT_X, HL_INTERACT_Z))
            hl_start();
        return;
    }

    anim_t++;
    cam_vy = 0;   /* nothing applies gravity in the cutscene branch, but the
                     value carried in from free play would be waiting on the
                     other side of the release */

    /* DUCK — ease across to the mouth of the gap and down to crawl height,
       turning to face east as it goes. One move rather than a crouch followed by
       a slide: the player is a step or two off the gap when they press, and two
       beats for that would be two beats of nothing happening. */
    if (state == HL_DUCK) {
        int32_t e = hl_ease(anim_t, HL_T_DUCK);
        cam_x     = src_x + ((HL_GAP_X   - src_x) * e) / 256;
        cam_y     = src_y + ((HL_CRAWL_Y - src_y) * e) / 256;
        cam_z     = src_z + ((HL_GAP_Z   - src_z) * e) / 256;
        cam_rot   = (src_rot + (rot_delta * e) / 256) & 4095;
        cam_pitch = src_pitch - (src_pitch * e) / 256;
        if (anim_t >= HL_T_DUCK) {
            cam_x = HL_GAP_X; cam_y = HL_CRAWL_Y; cam_z = HL_GAP_Z;
            cam_rot = HL_CRAWL_ROT; cam_pitch = 0;
            anim_t = 0;
            state  = HL_CRAWL;
        }
        return;
    }

    /* CRAWL — LINEAR, and only X moves. A constant rate under there is the whole
       read; the ease-out the other two legs use would look like being dragged
       out on a rope. The yaw is already right and never changes, which is why
       the gap was laid out along a single Z. */
    if (state == HL_CRAWL) {
        int32_t p = anim_t * 256 / HL_T_CRAWL; if (p > 256) p = 256;
        cam_x   = HL_GAP_X + ((HL_EXIT_X - HL_GAP_X) * p) / 256;
        cam_y   = HL_CRAWL_Y;
        cam_z   = HL_GAP_Z;
        cam_rot = HL_CRAWL_ROT;
        if (anim_t >= HL_T_CRAWL) {
            cam_x = HL_EXIT_X;
            src_rot   = HL_CRAWL_ROT;
            rot_delta = turn_delta(src_rot, HL_EXIT_ROT);
            anim_t = 0;
            state  = HL_RISE;
        }
        return;
    }

    /* RISE — stand back up on the spot and turn north, so free play resumes at
       the right height and pointing at the double door. */
    {
        int32_t e = hl_ease(anim_t, HL_T_RISE);
        cam_x   = HL_EXIT_X;
        cam_z   = HL_EXIT_Z;
        cam_y   = HL_CRAWL_Y + ((HL_STAND_Y - HL_CRAWL_Y) * e) / 256;
        cam_rot = (src_rot + (rot_delta * e) / 256) & 4095;
        if (anim_t >= HL_T_RISE) hl_finish();
    }
}

void hadad_library_draw(RenderContext *ctx) {
    /* Offered only while he is actually in the room and only until it is used.
       Before he appears the single door's own sign is the one on show, and the
       two must never be up together — the whole point of the swap is that the
       player is told, without a line of dialogue, that the door is no longer the
       way out. */
    if (state != HL_IDLE || !hadad_library_present()) return;

    int32_t dx = cam_x - HL_INTERACT_X;
    int32_t dz = cam_z - HL_INTERACT_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= HL_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > HL_FADE_NEAR) {
        int range = HL_TEXT_RADIUS - HL_FADE_NEAR;
        int prog  = xz - HL_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to escape",
                        HL_SIGN_X, HL_SIGN_Y, HL_GAP_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}
