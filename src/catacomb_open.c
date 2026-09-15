#include <stdint.h>
#include "camera.h"
#include "player.h"
#include "catacomb_doors.h"
#include "catacomb_open.h"

/* The catacomb doors opening. See catacomb_open.h for the shot and for why this
   module and not src/asag_boss.c owns FLAG_ASAG_DEAD. */

/* ---- THE VANTAGE ----------------------------------------------------------
   One fixed shot, south of the mouth on the room's centre line, at the height
   of the doors' own tops and tilted down onto them. Every number below was
   solved against gte_SetGeomScreen(256) on a 320x240 screen, where a world span
   S at distance D covers S * 256 / D pixels:

     THE RANGE comes off the HEIGHT, not the width. The leaves are 1000 wide and
     910 tall (x[-500,500] y[-910,0] z[3787,3847], see catacomb_doors.h), and
     240 of screen is the scarcer axis: at CO_CAM_Z the doorway stands 1300
     away, which puts 910 tall across 179 px of the 240 and 1000 wide across
     197 px of the 320. Pulling in to frame on the width instead would have run
     the leaves off the top and bottom of the picture.

     THE HEIGHT is the door tops, -900 against their -910. The brief asks for a
     camera OVERLOOKING them, and standing level with the lintel and tipping
     down is what reads as that; standing at the player's own eye height (-186)
     and tipping up would have been a shot of a wall.

     THE PITCH is that tilt and it is a constant because nothing in the shot
     moves: the eye is fixed and so is what it is looking at. Aiming at the
     leaves' vertical centre, y=-455, is dy=445 over dz=1300 — 18.9 degrees,
     which in the 4096-unit turn this engine measures angles in is 215. (The
     binary-search aim solver in src/asag_boss.c exists because that scene's
     camera and target both move. Here it would answer the same number 720
     times in a row.)

   CO_CAM_ROT is 0, which is facing +Z: the facade is at the NORTH end of the
   room and the room's own south-gate spawn uses the same 0 to look up the
   avenue at it (src/outside_catacombs.c). */
#define CO_CAM_X          0
#define CO_CAM_Y      (-900)
#define CO_CAM_Z       2487    /* 1300 south of the leaves at z=3787 */
#define CO_CAM_ROT        0    /* facing +Z, at the facade           */
#define CO_CAM_PITCH    215    /* 18.9 deg down, onto y=-455         */

/* ---- THE CLOCK ------------------------------------------------------------
   >>> THE SLIDE IS THE WHOLE SCENE AND THE OTHER THREE BEATS ARE ITS FRAME.
   <<< Eight seconds to move five hundred units is 62 units a second, which at
   this range is about twelve pixels a second — slow enough that a viewer cannot
   catch either leaf in the act of starting, which is the point of it.

   The one-second pause in front of it is the brief's, and it is doing real
   work: the cut into this room lands on a shot the player has never seen, and a
   beat of stillness is what lets them read it before anything moves.

   The glow comes AFTER the slide rather than under it, again as briefed. Two
   seconds, and it ramps rather than switching on for the reason every light in
   this game ramps. The hold after it is this file's own: cutting on the frame
   the ramp tops out would throw away the thing the previous eleven seconds were
   for. */
#define CO_T_PAUSE   60    /* 1.0 s held, doors shut                 */
#define CO_T_SLIDE  480    /* 8.0 s: CD_SLIDE_FULL apiece            */
#define CO_T_GLOW   120    /* 2.0 s: black to white                  */
#define CO_T_HOLD    90    /* 1.5 s on the lit doorway, then the cut */

typedef enum {
    CO_IDLE = 0,   /* not running                                     */
    CO_PAUSE,      /* the shot, held, doors shut                      */
    CO_SLIDE,      /* the leaves coming apart                         */
    CO_GLOW,       /* the backing polys lighting                      */
    CO_HOLD,       /* held on the lit doorway                         */
    CO_DONE        /* over; finished() was true for one frame         */
} CoState;

static CoState state = CO_IDLE;
static int32_t phase_t;
static int     finished;      /* 1 for exactly one frame, at the cut  */

static void enter_phase(CoState s) { state = s; phase_t = 0; }

void catacomb_open_reset(void) {
    state    = CO_IDLE;
    phase_t  = 0;
    finished = 0;
}

void catacomb_open_start(void) {
    catacomb_open_reset();

    /* THE CAMERA, AND THE PLAYER, WHICH ARE TWO THINGS. The room's init has
       already run outside_catacombs_spawn_south(), so cam_* is standing just
       inside the south gate — anchor the BODY there before moving the eye, or
       anything that reads "where the player is" would read the vantage instead.
       Nothing in this room is currently seeded with an enemy in it, so nothing
       is looking; the anchor is here because the next thing that gets seeded
       will be, and because releasing an anchor that was never taken is the
       asymmetry that bites later. */
    camera_anchor_player(cam_x, cam_y, cam_z);

    cam_x     = CO_CAM_X;
    cam_y     = CO_CAM_Y;
    cam_z     = CO_CAM_Z;
    cam_vy    = 0;
    cam_rot   = CO_CAM_ROT;
    cam_pitch = CO_CAM_PITCH;

    /* Shut, explicitly, rather than trusting catacomb_doors_init() to have left
       them there: this scene is the only thing in the game that runs with the
       flag clear and the doors about to move, and it should state the pose it
       starts from. */
    catacomb_doors_set_slide(0);

    enter_phase(CO_PAUSE);
}

int catacomb_open_active(void) {
    /* CO_DONE falls through, as ABE_DONE does in src/asag_boss.c: the frame
       after the cut the player is in another room entirely, and a cutscene flag
       still up would suppress their menu and their HUD there. */
    return state != CO_IDLE && state != CO_DONE;
}

int catacomb_open_finished(void) { return finished; }

void catacomb_open_update(void) {
    finished = 0;
    if (state == CO_IDLE || state == CO_DONE) return;

    phase_t++;

    switch (state) {

    case CO_PAUSE:
        if (phase_t >= CO_T_PAUSE) enter_phase(CO_SLIDE);
        break;

    /* LINEAR, and deliberately not eased. Every other move in this game eases
       because it is a body or a camera, and both accelerate. These are two
       slabs of stone being pushed by something that does not tire: a constant
       rate is what that looks like, and over eight seconds an ease-in would
       have spent the first two of them apparently doing nothing. */
    case CO_SLIDE: {
        int32_t s = (CD_SLIDE_FULL * phase_t) / CO_T_SLIDE;
        catacomb_doors_set_slide(s);
        if (phase_t >= CO_T_SLIDE) {
            catacomb_doors_set_slide(CD_SLIDE_FULL);
            enter_phase(CO_GLOW);
        }
        break;
    }

    case CO_GLOW:
        /* The ramp itself is read out of catacomb_open_glow() rather than
           written to anything — see there. */
        if (phase_t >= CO_T_GLOW) enter_phase(CO_HOLD);
        break;

    case CO_HOLD:
        if (phase_t >= CO_T_HOLD) {
            /* >>> THE FLAG, HERE, ON THE LAST FRAME. <<< It is what makes the
               doors stay apart and the doorway stay lit for the rest of the
               game, and src/player.h has the argument for why it cannot be set
               any earlier than this. */
            game_flag_set(FLAG_ASAG_DEAD);
            /* The camera goes back to the body. Nobody will look through it
               again in this room — main.c takes the transition off `finished`
               on this same frame — but a scene that anchors and does not
               release leaves the next room's player pinned to a spot in a room
               they have left. */
            camera_release_player();
            cam_pitch = 0;
            cam_vy    = 0;
            finished  = 1;
            state     = CO_DONE;
        }
        break;

    default:
        break;
    }
}

/* 0..256. The scene's ramp while the scene is running, FLAG_ASAG_DEAD every
   other time — which covers the hold, CO_DONE, and every later visit to this
   room including one that never played the scene at all. */
int32_t catacomb_open_glow(void) {
    if (state == CO_GLOW) {
        int32_t g = (phase_t * 256) / CO_T_GLOW;
        return g > 256 ? 256 : g;
    }
    if (state == CO_HOLD) return 256;
    if (state == CO_PAUSE || state == CO_SLIDE) return 0;
    return game_flag(FLAG_ASAG_DEAD) ? 256 : 0;
}
