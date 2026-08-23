#include <stdint.h>
#include "player.h"            /* game_flag, FLAG_HADAD_TWO, FLAG_EAST_HALL_RUBBLE */
#include "quake.h"
#include "east_hall.h"         /* east_hall_doors_arm on the way out */
#include "east_hall_quake.h"

/* The East Hall's collapse. Read east_hall_quake.h first: the beat, the "once
   and for good" rule and the reason the flag is set at the arm are all there. */

typedef enum {
    EHQ_IDLE = 0,  /* free play — nothing here runs */
    EHQ_ARMED,     /* the entry said yes; the first update takes the camera */
    EHQ_SHAKING,   /* src/quake.c owns the camera and all input */
} EhqState;

static EhqState state = EHQ_IDLE;

void east_hall_quake_reset(void) {
    state = EHQ_IDLE;
    quake_reset();
}

void east_hall_quake_arm_on_entry(void) {
    /* Nothing to do unless this is the first time out of the wrecked Library.
       The caller has already established WHERE the player came from; this owns
       the rest of the rule. */
    if (!game_flag(FLAG_HADAD_TWO))        { east_hall_quake_reset(); return; }
    if (game_flag(FLAG_EAST_HALL_RUBBLE))  { east_hall_quake_reset(); return; }

    /* Set NOW, not when the shake ends — see the warning in the header. From
       this instant the east door is buried in every future, including the ones
       where the five seconds never finish. */
    game_flag_set(FLAG_EAST_HALL_RUBBLE);

    /* ARMED, not started: the camera is taken on the first UPDATE instead of
       here, because the shake is about the arrival spot and main.c's loading
       branch is still placing the player when this runs. Deciding the base a
       frame later costs nothing and cannot be wrong. */
    quake_reset();
    state = EHQ_ARMED;
}

int east_hall_quake_active(void) {
    return state == EHQ_ARMED || state == EHQ_SHAKING;
}

void east_hall_quake_update(void) {
    if (state == EHQ_ARMED) {
        quake_start();
        state = EHQ_SHAKING;
        return;
    }
    if (state != EHQ_SHAKING) return;

    /* quake_update gives the camera and the player back on the frame it returns
       1; all this owes is the doors' Circle edge, which has not been polled for
       five seconds and is stale either way. Without it a player who came through
       the west door on a held Circle walks straight back into the rubble line. */
    if (quake_update()) {
        east_hall_doors_arm();
        state = EHQ_IDLE;
    }
}
