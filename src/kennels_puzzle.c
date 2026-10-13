#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include "render.h"
#include "camera.h"
#include "collision.h"        /* GROUND_FLOOR_Y */
#include "sound.h"            /* SFX_UNLOCK: the clunk of a lever being thrown */
#include "btn_glyph.h"        /* BTN_CIRCLE */
#include "door.h"             /* door_draw_string_3d: the "Press O" signs */
#include "title.h"            /* STATE_KENNELS */
#include "lever.h"
#include "bars.h"
#include "kennels_puzzle.h"

/* The Kennels' lever puzzle - see kennels_puzzle.h for the rules. */

#define KP_COUNT 4            /* four levers, four cells */

/* ---- The levers --------------------------------------------------------------
   On the EAST WALL, x=1800 (the proxy's, and the drawn stone's), which runs
   z[600,3000] between the south chamfer and the north-east bay. Set out by The
   Head's arm switches, the brief's guide:

     HEIGHT  the arms' palms rest at y~-176 (src/arm_switch.c), so the shaft's
             centre is at y=-180 on this room's y=0 floor.
     SPACING the arms stand 273 and 310 apart, alternately; 290 here, evenly,
             centred on the wall's mid-point z=1800.

   The model is the Attic Exit's (src/lever.h): local +Z is the blue cap that
   meets the wall, so rot_y 1024 turns it to face +X, and the centre stands
   75 off the wall. The shaft reaches out to x=1650; the 195 wall standoff
   holds the player at x=1605 and levers_collide at 1545 head-on. */
#define KP_WALL_X       1800
#define KP_LEVER_X      (KP_WALL_X - 75)
#define KP_LEVER_Y      (-180 - GROUND_FLOOR_Y)   /* lever_place's standing ref */
#define KP_LEVER_ROT    1024

/* Lever 1 is the NORTHERNMOST. */
static const int32_t KP_LEVER_Z[KP_COUNT] = { 2235, 1945, 1655, 1365 };

/* ---- The cells -----------------------------------------------------------------
   Read off assets/catacombs/Kennels.smx. Every gap is 600 tall, under a lintel
   at y=-600. Each gate stands INSIDE its cell, just behind the lintel, so
   opening it rises WHOLE by its own height up behind the stone rather than in
   front of it, and it is RECESSED (src/bars.h) so the lintel hides it as it
   goes and it is not drawn once it is up.

     B, C  the two kennels' doorways, x[-1200,-600] and x[600,1200] in the
           kennel front z[3600,3700]: 600 wide, square to the hall. The gate
           stands 5 behind the front's inner face (z=3700), from the kennel's
           outer wall to 50 past the doorway's inner jamb, so the cell cannot
           be seen past its edge from an angle.
     A, D  the corner bays' mouths, the diagonals (-1800,3000)-(-1200,3600) and
           (1800,3000)-(1200,3600): 849 wide, turned 45 degrees. The jambs are
           115 deep on the west and 86 on the east, and run on as the bay's
           side walls, so the gate stands 5 in past the jamb, wall to wall. */
#define KP_GATE_H        600

static const struct {
    int32_t x, z, rot, width;
} KP_GATE[KP_COUNT] = {
    /* A */ { -1602, 3402, 3584, 849 },
    /* B */ {  -875, 3730,    0, 650 },
    /* C */ {   875, 3730,    0, 650 },
    /* D */ {  1582, 3382,  512, 849 },
};

/* ---- The wiring ------------------------------------------------------------------
   Exactly these levers down, and every other one up. */
#define KP_L(n)          (1 << ((n) - 1))

static const uint8_t KP_COMBO[KP_COUNT] = {
    /* A */ KP_L(3),
    /* B */ KP_L(1) | KP_L(4),
    /* C */ KP_L(2) | KP_L(4),
    /* D */ KP_L(3) | KP_L(4),
};

/* ---- Timing: the Attic Exit's throw ------------------------------------------- */
#define KP_THROW_FRAMES   30    /* half a second */
#define KP_THROW_PITCH   512    /* 45deg of 4096: down sits that far below level,
                                   and UP the same angle above it */

/* ---- Interaction -----------------------------------------------------------------
   A box measured to the lever, as the Attic Exit's is. DEPTH (|dx|) has to
   clear the lever's own collision: head-on the player is parked 180 out, so
   260 leaves slack. WIDTH (|dz|) is half the spacing, so the boxes tile the
   wall without overlapping and one press can only mean one lever. */
#define KP_TRIGGER_DEPTH  260
#define KP_TRIGGER_WIDTH  145

/* ---- The signs ---------------------------------------------------------------------
   One over each lever, The Head's arms' terms: the YZ plane read from -X
   (mirror=1, and the -200 door_draw_string_3d wants on the reading axis, Z).
   80 off the wall, over the shaft, its bottom edge 60 above the lever. */
#define KP_SIGN_X         (KP_WALL_X - 80)
#define KP_SIGN_Y         (-180 - 60 - 7 * DOOR_PIXEL_SIZE)
#define KP_TEXT_RADIUS    800
#define KP_FADE_NEAR      500

/* ---- State --------------------------------------------------------------------- */
static uint8_t kp_levers = 0;           /* SAVED. bit i: lever i+1 is down */
static uint8_t kp_anim[KP_COUNT];       /* frames into the throw, 0..KP_THROW_FRAMES */
static int     kp_gate[KP_COUNT] = { -1, -1, -1, -1 };   /* bars_place()'s index */
static int     interact_prev = 1;       /* Circle edge-detect */

int  kennels_puzzle_levers(void)         { return kp_levers; }
void kennels_puzzle_set_levers(int bits) { kp_levers = (uint8_t)(bits & 0x0F); }
void kennels_puzzle_reset(void)          { kp_levers = 0; }

static int kp_down(int i) { return (kp_levers >> i) & 1; }

/* -45deg (tip up) at rest, +45deg (tip down) thrown: one swing of 90 through
   level, over the same half second. */
static void kp_push_pitch(int i) {
    lever_set_pitch(i, -KP_THROW_PITCH +
                       (2 * KP_THROW_PITCH * kp_anim[i]) / KP_THROW_FRAMES);
}

/* 1 while any lever is between up and down. */
static int kp_moving(void) {
    int i;
    for (i = 0; i < KP_COUNT; i++)
        if (kp_anim[i] != (kp_down(i) ? KP_THROW_FRAMES : 0)) return 1;
    return 0;
}

void kennels_puzzle_place(void) {
    int i;

    levers_clear();
    for (i = 0; i < KP_COUNT; i++) {
        lever_place(STATE_KENNELS, KP_LEVER_X, KP_LEVER_Y, KP_LEVER_Z[i],
                    KP_LEVER_ROT);
        kp_anim[i] = kp_down(i) ? KP_THROW_FRAMES : 0;
        kp_push_pitch(i);
    }

    /* The gates, at rest: the one cell the levers already open stands wound
       all the way up. */
    bars_clear();
    for (i = 0; i < KP_COUNT; i++) {
        int open = (kp_levers == KP_COMBO[i]);
        kp_gate[i] = bars_place(STATE_KENNELS, KP_GATE[i].x, -GROUND_FLOOR_Y,
                                KP_GATE[i].z, KP_GATE[i].rot,
                                open ? KP_GATE_H : 0);
        bars_set_size(kp_gate[i], KP_GATE[i].width, KP_GATE_H);
        bars_set_recessed(kp_gate[i]);
    }

    /* Don't treat a Circle held through the door transition as an interact. */
    interact_prev = interact_tapped();
}

/* Index of the lever the player is standing at and facing, or -1. */
static int kp_lever_in_reach(void) {
    int i, best = -1;
    int32_t best_d = KP_TRIGGER_DEPTH + KP_TRIGGER_WIDTH;
    for (i = 0; i < KP_COUNT; i++) {
        int32_t ax = cam_x - KP_LEVER_X, az = cam_z - KP_LEVER_Z[i];
        int32_t d;
        if (ax < 0) ax = -ax;
        if (az < 0) az = -az;
        if (ax > KP_TRIGGER_DEPTH || az > KP_TRIGGER_WIDTH) continue;
        d = ax + az;
        if (d >= best_d) continue;
        if (!interact_facing(KP_LEVER_X, KP_LEVER_Z[i])) continue;
        best_d = d; best = i;
    }
    return best;
}

void kennels_puzzle_update(int lock) {
    int i;

    /* THE THROWS: every lever mid-travel moves on a frame. */
    for (i = 0; i < KP_COUNT; i++) {
        uint8_t want = kp_down(i) ? KP_THROW_FRAMES : 0;
        if (kp_anim[i] == want) continue;
        kp_anim[i] = kp_down(i) ? (uint8_t)(kp_anim[i] + 1)
                                : (uint8_t)(kp_anim[i] - 1);
        kp_push_pitch(i);
    }

    /* THE GATES, once every lever has landed. Each is brought toward what the
       levers say; one still on its way up or down is left to finish, and is
       looked at again on the frame it stops. */
    if (!kp_moving()) {
        for (i = 0; i < KP_COUNT; i++) {
            int open = (kp_levers == KP_COMBO[i]);
            if (open && bars_settled(kp_gate[i]))      bars_raise(kp_gate[i], KP_GATE_H);
            else if (!open && bars_raised(kp_gate[i])) bars_drop(kp_gate[i]);
        }
    }
    bars_update();

    /* THE CIRCLE. A lever mid-travel ignores it, so a mashed button cannot
       leave one stuck between up and down. */
    {
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        if (lock || !just) return;

        i = kp_lever_in_reach();
        if (i < 0) return;
        if (kp_anim[i] != (kp_down(i) ? KP_THROW_FRAMES : 0)) return;
        kp_levers ^= (uint8_t)(1 << i);
        sound_play(SFX_UNLOCK);   /* the clunk of the switch, either direction */
    }
}

/* A "Press O" over every lever, each faded by the player's distance to it. */
static void kp_signs(RenderContext *ctx) {
    int i;
    for (i = 0; i < KP_COUNT; i++) {
        int32_t dx = cam_x - KP_LEVER_X, dz = cam_z - KP_LEVER_Z[i];
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int fade = 256;
        if (xz >= KP_TEXT_RADIUS) continue;
        if (xz > KP_FADE_NEAR) {
            int range = KP_TEXT_RADIUS - KP_FADE_NEAR;
            int prog  = xz - KP_FADE_NEAR;
            if (prog > range) prog = range;
            fade = 256 - ((prog * 256) / range);
        }
        door_draw_string_3d(ctx, "Press " BTN_CIRCLE,
                            KP_SIGN_X, KP_SIGN_Y, KP_LEVER_Z[i] - 200,
                            50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                            DOOR_PIXEL_SIZE);
    }
}

void kennels_puzzle_draw(RenderContext *ctx) {
    levers_draw(ctx);   /* flat-shaded; restores the view matrix */
    bars_draw(ctx);     /* ...as does this, and it needs the 128 window */
    kp_signs(ctx);
}
