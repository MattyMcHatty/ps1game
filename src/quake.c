#include <stdint.h>
#include "camera.h"
#include "player.h"          /* show_pickup_msg_raw */
#include "sound.h"           /* SFX_RUMBLE + its four alias voices */
#include "quake.h"

/* The house shaking. Read quake.h first — what this owes its caller, and why it
   may only run in a HOUSE room, are both there. */

/* ---- Timing and amplitude --------------------------------------------------
   Six rumbles across the shake, one every 30 frames. The clip is 127 frames
   long, so from the third onward there are five sounding at once — which is the
   point, and which is why SFX_RUMBLE has four alias voices (sound.h). Five
   voices covers six plays: #6 starts at frame 150 and #1 finished at 127, so the
   round-robin never lands on a voice still in use.

   The shake is flat-topped with a taper on the last half second, so it fades out
   rather than snapping to a stop on the frame control comes back. */
#define QK_HOLD           120   /* 2 s of stillness first          */
#define QK_SHAKE          180   /* 3 s of shaking after it         */
#define QK_TAPER           30   /* ...the last 0.5 s of which eases off */
#define QK_AMP_XZ          28   /* jitter, world units, +/- per axis */
#define QK_AMP_Y           20
#define QK_AMP_ROT         12   /* ~1 deg of yaw wobble, in 4096ths  */
#define QK_RUMBLE_COUNT     6
#define QK_RUMBLE_GAP      30   /* QK_SHAKE / QK_RUMBLE_COUNT        */

/* The base to shake around, taken at quake_start and restored every frame — the
   camera is driven FROM it rather than accumulated, so the jitter cannot walk
   the view off the spot it started on. */
static int32_t base_x, base_y, base_z, base_rot;

static int     running       = 0;
static int32_t quake_t       = 0;   /* frames in, hold included */
static int     rumbles_fired = 0;   /* how many of the six have gone off */

/* Self-contained LCG, as the Anzu, lightswitch and Rabisu modules each keep —
   there is no global PRNG in this project. It only jitters the quake. */
static uint32_t qk_rng = 0x2545F491u;
static uint32_t qk_rand(void) {
    qk_rng = qk_rng * 1664525u + 1013904223u;
    return qk_rng >> 16;
}

/* A signed jitter in [-amp, amp]. */
static int32_t qk_jitter(int32_t amp) {
    return (int32_t)(qk_rand() % (uint32_t)(2 * amp + 1)) - amp;
}

/* The rumble and its four alias voices, in the order the quake cycles them.
   See the SFX_RUMBLE_2 block in sound.h before changing the length of this. */
static const SfxID QUAKE_RUMBLE[] = {
    SFX_RUMBLE, SFX_RUMBLE_2, SFX_RUMBLE_3, SFX_RUMBLE_4, SFX_RUMBLE_5
};
#define QUAKE_RUMBLE_VOICES ((int)(sizeof QUAKE_RUMBLE / sizeof QUAKE_RUMBLE[0]))

void quake_reset(void) {
    int i;
    running       = 0;
    quake_t       = 0;
    rumbles_fired = 0;
    for (i = 0; i < QUAKE_RUMBLE_VOICES; i++) sound_stop(QUAKE_RUMBLE[i]);
}

int quake_running(void) { return running; }

void quake_start(void) {
    /* Strip any look offset BEFORE the snapshot, or the quake would shake around
       — and restore — a swung-off-centre heading. */
    camera_look_cancel();
    base_x = cam_x; base_y = cam_y; base_z = cam_z; base_rot = cam_rot;
    /* The camera is about to stop being where the player is, so pin the real
       spot for anything hunting them (camera.h). */
    camera_anchor_player(base_x, base_y, base_z);
    /* update_camera does not run for the whole quake, so a Circle pressed just
       before it would still be "in flight" on the frame control returns. */
    interact_spend_press();

    quake_t       = 0;
    rumbles_fired = 0;
    running       = 1;
}

int quake_update(void) {
    if (!running) return 0;

    quake_t++;

    cam_x = base_x; cam_y = base_y; cam_z = base_z;
    cam_rot = base_rot; cam_vy = 0; cam_pitch = 0;

    if (quake_t <= QK_HOLD) return 0;   /* the pause before it starts */

    int32_t t = quake_t - QK_HOLD;
    if (t > QK_SHAKE) {
        /* Done. The view is already back exactly where it started; hand the
           player back and let the caller re-arm its own input edge. The rumbles
           still sounding are left to ring out under free play. */
        cam_pitch = 0;
        running   = 0;
        camera_release_player();
        return 1;
    }

    /* On the frame the shaking starts, not on the trigger two seconds earlier:
       the line is what the player FEELS, and there is nothing to feel during the
       hold. The log box survives the quake — main.c keeps hud_draw_log_only up
       for the branches that host one — so it is readable while the room moves,
       and it stays up afterwards like every other posted line. */
    if (t == 1) show_pickup_msg_raw("You feel the ground shake violently");

    /* One rumble every QK_RUMBLE_GAP frames, round-robin across the five
       voices — see the QUAKE_RUMBLE note above. */
    if (rumbles_fired < QK_RUMBLE_COUNT && (t - 1) % QK_RUMBLE_GAP == 0) {
        sound_play(QUAKE_RUMBLE[rumbles_fired % QUAKE_RUMBLE_VOICES]);
        rumbles_fired++;
    }

    /* Flat amplitude, tapering over the last QK_TAPER frames so it dies away
       instead of stopping dead on the frame control comes back. */
    int32_t amp = 256;
    int32_t left = QK_SHAKE - t;
    if (left < QK_TAPER) amp = (left * 256) / QK_TAPER;

    cam_x   += (qk_jitter(QK_AMP_XZ) * amp) / 256;
    cam_z   += (qk_jitter(QK_AMP_XZ) * amp) / 256;
    cam_y   += (qk_jitter(QK_AMP_Y)  * amp) / 256;
    cam_rot  = (cam_rot + (qk_jitter(QK_AMP_ROT) * amp) / 256) & 4095;
    return 0;
}
