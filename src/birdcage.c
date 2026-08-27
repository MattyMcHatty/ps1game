#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"      /* game_flag, show_pickup_msg_raw */
#include "btn_glyph.h"   /* BTN_CIRCLE */
#include "door.h"        /* door_draw_string_3d_yaw */
#include "birdcage.h"

/* The cage's coordinates (BIRDCAGE_X/_Z/_FLOOR_Y/_MID_Y) are in birdcage.h,
   public because world.c places the key inside the cage off the same numbers. */

/* ---- The prompt -------------------------------------------------------------
   AT EYE LEVEL, not up under the cage. BC_TEXT_Y is the glyph TOP and matches
   MO_TEXT_Y, which is what every gate sign in this room stands at over the same
   y=0 ground — so the prompt reads straight ahead like the rest of them and the
   cage hangs above it. Hanging it just under the cage instead put it high in
   the frame and made the player look up to read a line about something they
   cannot reach.

   >>> IT IS AT 45 DEGREES TO THE CAGE, WHICH IS WHY IT USES THE YAW CALL. <<<
   The cage is square to the world grid, so TEXT_PLANE_XY / _YZ could only face
   the sign along an axis. BC_TEXT_YAW is 3584 of 4096 (315 degrees) measured the
   way cam_rot is, which puts the reading direction along (+X,+Z) and the sign's
   FACE looking back south-east — out into the pocket the player crosses to get
   here (it is bounded by collision walls 68 at x=3500 and 55 at z=1100, so the
   cage sits in its north-west corner and every approach is from the south-east).
   Turn the sign by a quarter and it goes edge-on to that approach. */
#define BC_TEXT_Y      (-186)   /* = MO_TEXT_Y, the room's sign height */
#define BC_TEXT_YAW      3584
#define BC_TEXT_RADIUS   1200
#define BC_FADE_NEAR      850
#define BC_TRIGGER_RADIUS 500   /* the gates' radius. The player can stand
                                   directly under the cage — nothing is solid
                                   there — so this is reach to spare, not a
                                   stretch */

/* ---- The three lines --------------------------------------------------------
   Indexed by BirdcageState, so a fourth state is a row here and not a branch.
   Each wraps into the HUD log's 15-column, 4-row box without losing a line
   (see hud.c's hud_log_wrap); the longest is 58 of the 63 characters
   show_pickup_msg_raw will copy. */
static const char * const BC_LINE[3] = {
    "There's a key in that birdcage. I can't reach it.",
    "The bird cage is open but the key has fell into the drain!",
    "The key washed away!",
};

/* Circle edge-detect, seeded by birdcage_init(). Starts "held" so a press
   carried in through a gate transition doesn't post a line on arrival — the
   same contract maze_one_gate_arm() has. */
static int circle_prev = 1;

BirdcageState birdcage_state(void) {
    /* _WASHED first: it is only ever set on top of _OPEN, and reading it first
       means a save that somehow carried it alone still lands on the last line
       rather than falling through to the first. */
    if (game_flag(FLAG_BIRDCAGE_WASHED)) return BIRDCAGE_WASHED;
    if (game_flag(FLAG_BIRDCAGE_OPEN))   return BIRDCAGE_DROPPED;
    return BIRDCAGE_LOCKED;
}

void birdcage_open(void) { game_flag_set(FLAG_BIRDCAGE_OPEN); }

void birdcage_wash(void) {
    /* Set both, so the state can never skip a bit even if the drain somehow
       runs before the cage is opened. */
    game_flag_set(FLAG_BIRDCAGE_OPEN);
    game_flag_set(FLAG_BIRDCAGE_WASHED);
}

void birdcage_init(void) { circle_prev = interact_tapped(); }

/* Manhattan reach plus a facing test, as every other Circle prompt in the
   garden uses: a cage behind you is not one you are examining. */
static int cage_in_reach(void) {
    int32_t dx = cam_x - BIRDCAGE_X;
    int32_t dz = cam_z - BIRDCAGE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < BC_TRIGGER_RADIUS && interact_facing(BIRDCAGE_X, BIRDCAGE_Z);
}

int birdcage_update(void) {
    int held = interact_tapped();
    int just = held && !circle_prev;
    circle_prev = held;

    if (!just || !cage_in_reach()) return 0;

    show_pickup_msg_raw(BC_LINE[birdcage_state()]);
    return 1;
}

void birdcage_text(RenderContext *ctx) {
    int32_t dx = cam_x - BIRDCAGE_X;
    int32_t dz = cam_z - BIRDCAGE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= BC_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > BC_FADE_NEAR) {
        int range = BC_TEXT_RADIUS - BC_FADE_NEAR;
        int prog  = xz - BC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* The r/g/b is the garden's sign green; the Circle control code inside the
       string supplies its own red and the fade still applies to both. The
       string is FIXED in all three states — what the press says changes, what
       the sign offers does not. */
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to examine",
                            BIRDCAGE_X, BC_TEXT_Y, BIRDCAGE_Z,
                            50, 255, 50, fade, BC_TEXT_YAW, DOOR_PIXEL_SIZE);
}
