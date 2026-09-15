#include <stdint.h>
#include "camera.h"
#include "collision.h"   /* GROUND_FLOOR_Y: the eye height over a y=0 floor */
#include "sound.h"
#include "cdaudio.h"
#include "hatch_arrival.h"

/* The drop off the well in The Hatch's north chamber. See hatch_arrival.h for
   the beats and for why the music starts on the landing frame. */

/* ---- WHERE IT STARTS -------------------------------------------------------
   ON THE WELL, IN FRONT OF THE CHAINS. The Hatch's four chain_128 polys are a
   box at x[3202,3270] z[2963,3037] y[-570,-178] — the links holding the lid
   over the brick well at x[3000,3600] z[2700,3300] (src/the_hatch.h). The
   camera stands on their centre line, at their own mid-height, and just clear
   of their SOUTH face at z=2963: the chains are directly behind the back of the
   head, which is what "in front of the chain texture, facing south" describes.

   HA_START_Y is the chains' midpoint, (-570 + -178) / 2. It puts the eye 374
   above the lawn and 174 above the well's own 200-tall brick box, so the drop
   that follows is a step off a wall rather than a fall down anything — the
   fall in this room is the other hole, and this is the way back from it. */
#define HA_START_X    3236    /* (3202 + 3270) / 2 */
#define HA_START_Y   (-374)   /* (-570 + -178) / 2 */
#define HA_START_Z    2940    /* 23 clear of the chains' south face at 2963 */
#define HA_ROT        2048    /* facing -Z: due south, down the room.
                                 0 is +Z and 1024 is +X in this engine — see
                                 the_hatch_spawn_west(), which uses 1024. */

/* ---- WHERE IT LANDS --------------------------------------------------------
   ON THE CHAMBER FLOOR, SOUTH OF THE WELL AND CLEAR OF ITS FENCE. Collision
   walls 3, 5, 6 and 14 stand round the brick box facing outward, so the closest
   the player can stand to its south face at z=2700 is one wall radius off it:
   2700 - 195 = 2505. Landing at 2450 leaves 55 units of margin, which is what
   stops apply_height/apply_collision_reception shoving the player on the first
   frame they own — the arrival-spawn rule every gate in this game follows.

   HA_END_Y is the standing eye over a y=0 floor: the floor, less
   GROUND_FLOOR_Y, less the 40-unit standoff apply_height applies. The Hatch's
   own TH_EYE_Y is this expression and the chamber is the same flat plane as the
   rest of the room — all eight collision floor planes are at y=0. */
#define HA_END_X      3236    /* straight forward: no sideways drift */
#define HA_END_Z      2450
#define HA_END_Y      (0 - GROUND_FLOOR_Y - 40)

/* ---- THE ARC ---------------------------------------------------------------
   >>> THE RISE IS WHAT MAKES IT A JUMP RATHER THAN A SLIDE. <<< Interpolating
   both axes straight gives a diagonal, which reads as the camera being carried
   rather than as a body pushing off something. HA_ARC_RISE is subtracted on a
   4t(1-t) hump — zero at both ends, HA_ARC_RISE at the middle, and subtracted
   because -y is up in this engine.

   120 against a 185-unit descent is a shallow hop, which is the "small arc" the
   shot is meant to be: the eye climbs about a third of the way back up the
   chains before it starts coming down. Deeper and the camera leaves frame over
   the top of the hedge line at -500.

   HA_T_ARC is seven tenths of a second, which is roughly what this engine's
   other fall of this size takes.

   ---- AND IT CLIMBS OUT BEFORE IT JUMPS -------------------------------------
   >>> THE FIRST THING THE EYE DOES IS GO STRAIGHT UP. <<< HA_CLIMB_RISE of
   pure vertical over HA_T_CLIMB, no X, no Z, no yaw: the last of a body hauling
   itself over the well's lip and getting its feet under it. Without it the shot
   opens on a camera already standing on top of the well, which does not say how
   it got there — this is where the player comes UP out of the catacombs, and
   the climb is the whole point of the cut.

   44 units is about four inches at this game's scale (the catacomb doorway is
   905 tall and reads as a seven-foot arch, so a foot is a shade over 100), and
   it is deliberately tiny: a bigger rise stops being the end of a climb and
   becomes a second jump in front of the real one. HA_T_CLIMB is 0.9 s for it,
   which is 49 units a second — slow, heavy, and eased OUT so it settles rather
   than stopping dead.

   HA_TOP_Y is where that leaves the eye, and it is the arc's start: everything
   below interpolates from it and not from HA_START_Y.

   HA_T_HOLD then sits between the climb and the jump. It is the beat the whole
   ending is paced in — two seconds, the same as the arena's outro and the
   catacomb doors' pause either side of their slide — and it is doing the same
   work here that it does there: the cut into this room lands on a shot the
   player has never seen, from a vantage they have never had, and stillness is
   what lets them read where they are before anything moves. */
#define HA_T_CLIMB     54    /* 0.9 s of pure vertical  */
#define HA_CLIMB_RISE  44    /* ~4 inches, straight up  */
#define HA_TOP_Y      (HA_START_Y - HA_CLIMB_RISE)
#define HA_T_HOLD     120    /* 2.0 s on the well top   */
#define HA_T_ARC       42    /* 0.7 s of flight         */
#define HA_ARC_RISE   120

/* The pitch to hold on the well: looking south and down at the ground the jump
   is aimed at. It eases to level across the arc, so the camera is horizontal on
   the frame control is handed over — nothing else in this room will ever clear
   cam_pitch, which is the note src/asag_boss.c's handover ends on. */
#define HA_START_PITCH 300   /* ~26 deg down in the 4096-unit turn */

typedef enum {
    HA_IDLE = 0,
    HA_CLIMB,     /* straight up, out of the well      */
    HA_HOLD,      /* on the well top, held             */
    HA_ARC,       /* in the air                        */
    HA_DONE       /* landed; the player has the camera */
} HaState;

static HaState state = HA_IDLE;
static int32_t phase_t;

void hatch_arrival_reset(void) {
    state   = HA_IDLE;
    phase_t = 0;
}

void hatch_arrival_start(void) {
    hatch_arrival_reset();

    /* THE BODY GOES TO THE LANDING, NOT TO THE WELL. Anything that reads "where
       the player is" during the second the camera is in the air should get the
       place they are about to be standing: this room seeds four LIVING STATUES
       and that enemy acts on proximity to the anchored body. Anchoring on the
       well top would have put the player 374 units in the air for the scene and
       then teleported them, which is the one thing player_anchor_epoch() exists
       to let a watcher notice — and there is no reason to make it notice. */
    camera_anchor_player(HA_END_X, HA_END_Y, HA_END_Z);

    cam_x     = HA_START_X;
    cam_y     = HA_START_Y;
    cam_z     = HA_START_Z;
    cam_vy    = 0;
    cam_rot   = HA_ROT;
    cam_pitch = HA_START_PITCH;

    state   = HA_CLIMB;
    phase_t = 0;
}

int hatch_arrival_active(void) {
    return state == HA_CLIMB || state == HA_HOLD || state == HA_ARC;
}

void hatch_arrival_update(void) {
    if (!hatch_arrival_active()) return;

    phase_t++;

    /* THE CLIMB. Eased out — c goes 0..256 as p*(512-p)/256, which is fast at
       the bottom and settles at the top, the shape of the last heave over a
       lip. cam_pitch is left alone: the eye comes up still looking down at the
       ground it is about to jump to. */
    if (state == HA_CLIMB) {
        int32_t p = (phase_t * 256) / HA_T_CLIMB;
        if (p > 256) p = 256;
        int32_t c = (p * (512 - p)) / 256;
        if (c > 256) c = 256;

        cam_y  = HA_START_Y - (HA_CLIMB_RISE * c) / 256;
        cam_vy = 0;

        if (phase_t >= HA_T_CLIMB) {
            cam_y   = HA_TOP_Y;
            state   = HA_HOLD;
            phase_t = 0;
        }
        return;
    }

    if (state == HA_HOLD) {
        if (phase_t >= HA_T_HOLD) { state = HA_ARC; phase_t = 0; }
        return;
    }

    /* p is 0..256 across the flight. Horizontal travel is LINEAR in it — a body
       in the air has nothing pushing it sideways — and the vertical is the
       straight fall plus the hump. */
    int32_t p = (phase_t * 256) / HA_T_ARC;
    if (p > 256) p = 256;

    cam_x = HA_START_X + ((HA_END_X - HA_START_X) * p) / 256;
    cam_z = HA_START_Z + ((HA_END_Z - HA_START_Z) * p) / 256;

    int32_t hump = (4 * HA_ARC_RISE * p * (256 - p)) / (256 * 256);
    /* FROM HA_TOP_Y, not HA_START_Y: the climb has already happened and the
       jump leaves from where it left the eye. */
    cam_y = HA_TOP_Y + ((HA_END_Y - HA_TOP_Y) * p) / 256 - hump;

    cam_pitch = HA_START_PITCH - (HA_START_PITCH * p) / 256;
    cam_vy    = 0;

    if (phase_t >= HA_T_ARC) {
        cam_x     = HA_END_X;
        cam_y     = HA_END_Y;
        cam_z     = HA_END_Z;
        cam_rot   = HA_ROT;
        cam_pitch = 0;
        cam_vy    = 0;

        /* The impact, and it is the same sound Asag's arrival lands on: this is
           the other end of that journey. */
        sound_play(SFX_HURT);

        /* ...and the garden comes back. Suppressed on this route in main.c's
           STATE_LOADING branch precisely so it can start HERE — see the note in
           hatch_arrival.h. */
        cdaudio_play(CDAUDIO_FOUNTAIN_TRACK, 1);

        camera_release_player();
        state = HA_DONE;
    }
}
