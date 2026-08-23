#include <stdint.h>
#include "camera.h"            /* player_x/y/z — never cam_*, see below */
#include "player.h"            /* game_flag, FLAG_HADAD_TWO, FLAG_RECEPTION_HADAD */
#include "quake.h"
#include "hadad.h"             /* hadad_reception_instance / _begin */
#include "reception.h"         /* reception_sealed, reception_doors_arm */
#include "reception_hadad.h"

/* The Reception's Hadad encounter. Read reception_hadad.h first: the beat
   sheet, the "once and for good" rule and the reason the trigger has a
   backstop are all there. */

/* ---- The trigger -----------------------------------------------------------
   "When the player crosses onto the area in front of where the ramp meets the
   second level." The stair head is x[300,700] at z=500 (reception.c's floor
   zones), so its centre line is x=500 and the floor in front of it is the strip
   of the second level's north band immediately above z=500.

     TRIG      (500, 700) — 200 north of the head, on its centre line. TRUE
               RADIAL at 500, tested against squared distance so no sqrt is
               needed, which reaches z=1200 up the band and x=0/x=1000 across
               it. It cannot fire on the way IN: the East Hall's double door
               stands the player at (1290, 1071), which is 873 away.

               It also cannot be walked PAST on the way down: the only way onto
               the stair is through x[300,700] at z=500, and every point of that
               is within 300 of this one.

     BACKSTOP  x <= -200. The one route this radius does not cover is somebody
               who arrives, ignores the stair entirely and hugs the north wall
               all the way west to the West Corridor door — at z=1305 (the north
               wall's own push-out) the radius is 605 short. This line catches
               them a little further along the same floor. It is WEST of the
               stair head, so a player who takes the ordinary route has already
               tripped the radius long before they reach it.

     UPPER_Y   both tests are gated on the player being on the SECOND LEVEL.
               That floor stands the eye at -789; the mid-landing at -339 and
               the ground floor at -189 are both well below -500. Without it the
               ground-floor room tucked under the balcony (the dresser's, north
               of z=525) sits directly beneath the radius and would fire it from
               underneath. */
#define HRH_TRIG_X            500
#define HRH_TRIG_Z            700
#define HRH_TRIG_RADIUS       500
#define HRH_BACKSTOP_X      (-200)
#define HRH_UPPER_Y         (-500)

typedef enum {
    HRH_IDLE = 0,  /* not armed, or long over — nothing here runs */
    HRH_ARMED,     /* the entry said yes; the first update takes the camera */
    HRH_SHAKING,   /* src/quake.c owns the camera and all input */
    HRH_WAIT,      /* free play; watching for the player to cross */
    HRH_DONE,      /* he is in the room and walking; nothing left to direct */
} HrhState;

static HrhState state = HRH_IDLE;

/* >>> LATCHED AT THE ARM, NOT RE-DERIVED. <<< ADDING_A_BOSS_ENCOUNTER.txt
   STEP 4: a script that re-asks "which Hadad is this" every frame tears itself
   down on the frames the answer is legitimately NULL. The slot is a fixed
   array and only a room change can empty it — which resets this director
   anyway — but the handover is the one call that must not miss, so the answer
   is taken once, on the frame world_enter has finished placing him. */
static Hadad *rc_hadad = NULL;

static void hrh_park(void) {
    state    = HRH_IDLE;
    rc_hadad = NULL;
}

void reception_hadad_reset(void) {
    hrh_park();
    quake_reset();
}

void reception_hadad_enter(void) {
    /* PARK ONLY. reception_init() runs before world_enter() has placed anybody
       and while game_flags may still hold the pre-load values, so nothing here
       may decide anything — that is reception_hadad_arm_on_entry()'s job, from
       main.c's post-entry re-derive block. */
    hrh_park();
    quake_reset();
}

void reception_hadad_arm_on_entry(void) {
    if (!reception_sealed())                  { reception_hadad_reset(); return; }
    if (game_flag(FLAG_RECEPTION_HADAD))      { reception_hadad_reset(); return; }

    /* Set NOW, not when the beat ends — see the warning in the header. From
       this instant the encounter has happened, in every future, including the
       ones where the minute that follows never finishes. */
    game_flag_set(FLAG_RECEPTION_HADAD);

    quake_reset();
    rc_hadad = hadad_reception_instance();
    /* ARMED, not started: the camera is taken on the first UPDATE instead of
       here, because the shake is about the arrival spot and main.c's loading
       branch is still placing the player when this runs. Deciding the base a
       frame later costs nothing and cannot be wrong. */
    state = HRH_ARMED;
}

int reception_hadad_active(void) {
    return state == HRH_ARMED || state == HRH_SHAKING;
}

/* Has the player crossed onto the floor in front of the stair head? Squared
   distance against squared radius: the room is 3000 on a side, so the largest
   dx*dx + dz*dz this can see is about 1.8e7 and int32 has room to spare.

   player_x/y(), never cam_*: the quake this encounter opens with anchors the
   player and shakes the camera off the spot (camera.h), and although that beat
   is over by the time this is polled, an encounter that reads the camera to
   find the player is the single most likely bug in the whole feature
   (ADDING_A_BOSS_ENCOUNTER.txt STEP 6). */
static int hrh_crossed(void) {
    int32_t px = player_x();
    int32_t dx, dz;

    if (player_y() >= HRH_UPPER_Y) return 0;   /* not on the second level */
    if (px <= HRH_BACKSTOP_X)      return 1;   /* the backstop */

    dx = px - HRH_TRIG_X;
    dz = player_z() - HRH_TRIG_Z;
    return dx * dx + dz * dz <= HRH_TRIG_RADIUS * HRH_TRIG_RADIUS;
}

void reception_hadad_update(void) {
    if (state == HRH_IDLE || state == HRH_DONE) return;

    if (state == HRH_ARMED) {
        quake_start();
        state = HRH_SHAKING;
        return;
    }

    if (state == HRH_SHAKING) {
        /* quake_update gives the camera and the player back on the frame it
           returns 1; all this owes is the room's own Circle edges, which have
           not been polled for five seconds and are stale either way. Without
           the re-arm a player who came through the East Hall's door on a held
           Circle walks straight into the rubble line on the frame control
           returns. */
        if (quake_update()) {
            reception_doors_arm();
            state = HRH_WAIT;
        }
        return;
    }

    /* HRH_WAIT — free play, watching one line. The handover is a single call:
       from here the enemy has him, and there is nothing to stop, seal or
       restore afterwards. */
    if (!hrh_crossed()) return;
    hadad_reception_begin(rc_hadad);   /* NULL-safe: a debug jump can empty the
                                          slot, and the beat is over either way */
    rc_hadad = NULL;
    state    = HRH_DONE;
}
