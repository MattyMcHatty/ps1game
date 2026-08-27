#include <stdint.h>
#include <stddef.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "camera.h"
#include "collision.h"
#include "player.h"    /* current_weapon, SCREEN_* (via render.h) */
#include "graveolver.h"  /* graveolver_is_reloading */
#include "weapon.h"      /* weapon_switching */
#include "sound.h"       /* footstep SFX */
#include "title.h"
#include "debug_opts.h"

int32_t cam_x   = 0;
int32_t cam_y   = 0;
int32_t cam_vy  = 0;
int32_t cam_z   = 0;
int32_t cam_rot = 0;
int32_t cam_pitch = 0;
int32_t cam_kb_vx = 0;
int32_t cam_kb_vz = 0;

/* Player anchor (see camera.h): held position while a puzzle owns the camera. */
static int32_t anchor_x = 0, anchor_y = 0, anchor_z = 0;
static int     anchored = 0;
/* Bumped on every anchor AND every release — see player_anchor_epoch(). */
static int     anchor_epoch = 0;

void camera_anchor_player(int32_t x, int32_t y, int32_t z) {
    /* A puzzle taking the camera wants cam_rot to be the player's real facing,
       and it owns cam_pitch from here — so a look offset applied on the frame
       the puzzle started (Circle both looks and interacts) goes now, not on the
       next update_camera, which a camera-locked puzzle may not even reach. */
    camera_look_cancel();
    anchor_x = x; anchor_y = y; anchor_z = z;
    anchored = 1;
    anchor_epoch++;
}
void camera_release_player(void) { anchored = 0; anchor_epoch++; }

int player_anchor_epoch(void) { return anchor_epoch; }

int32_t player_x(void) { return anchored ? anchor_x : cam_x; }
int32_t player_y(void) { return anchored ? anchor_y : cam_y; }
int32_t player_z(void) { return anchored ? anchor_z : cam_z; }

/* Push the player away from (from_x, from_z) at `speed` units on the first
   frame; update_camera decays it. */
void player_knockback(int32_t from_x, int32_t from_z, int32_t speed) {
    int32_t dx = cam_x - from_x;
    int32_t dz = cam_z - from_z;
    int32_t d  = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (d <= 0) { cam_kb_vx = speed; cam_kb_vz = 0; return; }
    cam_kb_vx = (dx * speed) / d;
    cam_kb_vz = (dz * speed) / d;
}
int sprint_stamina  = SPRINT_STAMINA_MAX;
int sprint_cooldown = 0;

/* Aiming: hold L2 with a RANGED weapon to reposition the crosshair. While
   aiming the player is rooted (walk + turn disabled) and the d-pad drives the
   crosshair; releasing L2 recentres it and frees movement. aim_x/aim_y are the
   crosshair's live screen position, read by the weapon's hitscan and reticule.

   RANGED means the Grave-olver OR the Helluminator — see has_ranged below. They
   share aim_x/aim_y and so share the rest position, which is what makes the
   lantern's crosshair start exactly where the gun's does. */
#define AIM_REST_X     (SCREEN_XRES / 2)
#define AIM_REST_Y     (SCREEN_YRES / 2 + 20)   /* rests a touch below centre */
#define AIM_MOVE_SPEED 3     /* crosshair pixels per frame                     */
#define AIM_MARGIN     10    /* keep the crosshair this far from screen edges  */
/* Lowest the crosshair goes: the reticule's bottom edge ends exactly on the top
   edge of the HUD panel, so it never disappears behind it. The panel is 56px
   tall and bottom-aligned (hud.c's HUD_Y).

   >>> THE REACH IS PER-WEAPON. <<< The Grave-olver's cross reaches 14px below
   aim_y; the Helluminator's ring is three times the size and reaches 42
   (HELL_AIM_RADIUS). One shared 14 would let the lantern's ring sink 28px into
   the panel, so the floor is computed from whatever is in hand. */
#define AIM_HUD_TOP (SCREEN_YRES - 56)
#define AIM_RETICULE_REACH 14
#define AIM_RETICULE_REACH_HELL 42
static int32_t aim_max_y(void) {
    int reach = (current_weapon == WEAPON_HELLUMINATOR) ? AIM_RETICULE_REACH_HELL
                                                        : AIM_RETICULE_REACH;
    return AIM_HUD_TOP - reach;
}
int     aiming = 0;
int32_t aim_x  = AIM_REST_X;
int32_t aim_y  = AIM_REST_Y;

/* Look mode (see camera.h): hold Circle to swing the view off the player's
   facing without moving. look_yaw/look_pitch are the offsets currently folded
   into cam_rot/cam_pitch — update_camera subtracts look_yaw again at the top of
   every frame so turning, walking, enemies and saves all keep seeing the true
   facing in cam_rot. */
#define LOOK_SPEED   14   /* offset units per frame while held (~1.2 deg) */
#define LOOK_RETURN  28   /* recentre speed once Circle is released       */
int look_mode = 0;
static int32_t look_yaw   = 0;
static int32_t look_pitch = 0;

/* Circle is shared: a TAP interacts (doors, prompts, save points, puzzle
   entries), a HOLD looks around. So interactions resolve on release — every
   world interaction calls interact_tapped() instead of reading the pad, and
   gets a single frame of 1 when a press ends inside LOOK_HOLD_FRAMES. Once a
   press crosses that it has become a look and can never fire an interaction,
   which is what stops a look near a door from walking you through it. */
/* The tap window has to cover a RELAXED press, not a flick: a deliberate press
   of a pad button commonly runs 0.2-0.3 s, and anything shorter than that made
   interacting feel like it needed a sharp jab. 20 frames (~0.33 s) takes every
   normal press as a tap; a look is then an unmistakable hold. */
#define LOOK_HOLD_FRAMES 20   /* ~0.33 s down and the press is a look, not a tap */
static int circle_frames = 0; /* frames Circle has been down, capped at the
                                 threshold; -1 = press spent (see below) */
static int tap_frame     = 0; /* 1 for the single frame a tap resolves */

int interact_tapped(void) { return tap_frame; }

/* Mark the Circle press currently in flight as spent, so it can neither become
   a look nor pop out as a tap when it is finally released. The mark clears on
   that release, so only the one press is affected.

   Split out of camera_look_cancel because the two are wanted separately: a
   module that read Circle off the pad and acted on the PRESS needs to stop that
   press reaching the interact path on RELEASE, without also unwinding a look
   angle and flattening cam_pitch — which for a module that has restored its own
   camera would visibly skew the view. trick_drawers is the caller. */
void interact_spend_press(void) {
    circle_frames = -1;
    tap_frame     = 0;
}

/* Is (wx, wz) in front of the player? The delta is rotated into player space —
   forward is (isin, icos) of the facing, so `fwd` is the component along it and
   `side` the component across — and the cone test is then just a comparison of
   the two, with no square root or arctangent involved.
   look_yaw is unfolded first so this reads the player's real facing even on a
   frame where the view is still easing back from a look. */
int interact_facing(int32_t wx, int32_t wz) {
    int32_t rot = (cam_rot - look_yaw) & 4095;
    int32_t dx  = wx - cam_x;
    int32_t dz  = wz - cam_z;

    int32_t manhattan = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (manhattan < INTERACT_FACE_MIN) return 1;   /* standing on it */

    int32_t s = isin(rot), c = icos(rot);
    int32_t fwd  = (dx * s + dz * c) >> 12;
    int32_t side = (dx * c - dz * s) >> 12;
    if (fwd <= 0) return 0;                        /* behind the player */
    if (side < 0) side = -side;
    return side * INTERACT_FOV_DEN <= fwd * INTERACT_FOV_NUM;
}

/* Drop any applied offset and put cam_rot back to the player's facing. Called on
   the paths that leave update_camera early (menus, no pad), by anything that
   takes the camera, and from main's loop when the game leaves free roam — so
   nothing else ever snapshots or saves a view that is still swung off-centre.
   Leaves cam_pitch alone while a puzzle owns the camera: that pitch is the
   puzzle's, not ours. */
void camera_look_cancel(void) {
    cam_rot   = (cam_rot - look_yaw) & 4095;
    look_yaw  = 0;
    if (!anchored) cam_pitch = 0;
    look_pitch = 0;
    look_mode  = 0;
    /* Spend whatever Circle press is in flight: a press that was still down when
       the camera was taken must not resume looking, nor pop out as a tap when
       it is finally released. The next release clears the mark. */
    interact_spend_press();
}

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Load the GTE with the current camera view matrix (rotation about Y by
   -cam_rot, then translate by -cam). Any world-space GTE projection (gte_rtps)
   done afterwards uses the camera view — used by the debug overlays, which run
   after the weapon/HUD draws have left their own matrices in the GTE. */
/* Yaw, then pitch, then translate: R = Rx(pitch) * Ry(-yaw), t = R * (-cam).
   The two rotations are composed explicitly (rather than handed to RotMatrix as
   one SVECTOR) so the order is unambiguous — pitch must be applied in VIEW
   space, after the world has been yawed to face the camera. With cam_pitch = 0
   the pitch matrix is the identity and this is exactly the old yaw-only view. */
void camera_build_view(MATRIX *out) {
    MATRIX  yaw, pitch;
    SVECTOR neg_rot   = {0, (short)-cam_rot, 0, 0};
    SVECTOR pitch_rot = {(short)cam_pitch, 0, 0, 0};
    VECTOR  trans     = {-cam_x, -cam_y, -cam_z};

    RotMatrix(&neg_rot, &yaw);
    if (cam_pitch) {
        RotMatrix(&pitch_rot, &pitch);
        MulMatrix0(&pitch, &yaw, out);
    } else {
        *out = yaw;
    }
    ApplyMatrixLV(out, &trans, &trans);
    out->t[0] = trans.vx; out->t[1] = trans.vy; out->t[2] = trans.vz;
}

void camera_set_view_matrix(void) {
    MATRIX m;
    camera_build_view(&m);
    gte_SetRotMatrix(&m);
    gte_SetTransMatrix(&m);
}

void update_camera(void) {
    if (game_state == STATE_MENU) { camera_look_cancel(); return; }
    if (!pad_buff_len[0])         { camera_look_cancel(); return; }

    PadResponse *pad = (PadResponse *)pad_buff[0];
    uint16_t btn = ~pad->btn;

    /* Unfold last frame's look offset: everything below (turning, walking, the
       sprint/footstep logic) works on the player's true facing. The offset is
       folded back in at the end of the function. */
    cam_rot = (cam_rot - look_yaw) & 4095;

    /* Player knockback (tentacle hits): displace and decay before input, so it
       stacks with walking and gets wall-resolved by the later apply_collision. */
    if (cam_kb_vx != 0 || cam_kb_vz != 0) {
        cam_x += cam_kb_vx;
        cam_z += cam_kb_vz;
        cam_kb_vx = (cam_kb_vx * 3) / 4;   /* ~0.75 decay per frame */
        cam_kb_vz = (cam_kb_vz * 3) / 4;
        if (cam_kb_vx > -2 && cam_kb_vx < 2) cam_kb_vx = 0;
        if (cam_kb_vz > -2 && cam_kb_vz < 2) cam_kb_vz = 0;
    }

    /* Aiming mode: the d-pad drives the crosshair and the player is frozen.
       Starting a reload cancels aiming, and a reload can't be aimed through — you
       must release L2 and press it again after it finishes (edge-triggered start,
       so holding L2 across the whole reload does not re-arm the aim). */
    static int aim_prev_held = 0;
    int aim_held  = (btn & PAD_L2) ? 1 : 0;
    /* Both ranged weapons aim. The lantern has no reload of its own, so
       graveolver_is_reloading() is only ever true with the gun in hand and the
       one test still covers both. */
    int has_ranged = (current_weapon == WEAPON_GRAVEOLVER) ||
                     (current_weapon == WEAPON_HELLUMINATOR);
    int reloading  = graveolver_is_reloading() || weapon_switching();

    if (aiming) {
        if (reloading || !has_ranged || !aim_held) {
            aiming = 0;
            aim_x  = AIM_REST_X;
            aim_y  = AIM_REST_Y;
        }
    } else if (has_ranged && !reloading && aim_held && !aim_prev_held) {
        aiming = 1;
    }
    aim_prev_held = aim_held;

    /* Circle: hold to look, tap to interact. The press is classified here — under
       LOOK_HOLD_FRAMES it resolves as a tap on release (interact_tapped(), which
       is what doors and prompts read), at or past it the player is looking and
       no interaction can come of it. Walking is still free during those first
       frames, so a tap never stutters the player's movement.
       Look mode itself is unavailable while aiming (L2 owns the d-pad) or while
       a puzzle owns the camera. It roots the player and gives the d-pad to the
       view; on release the view eases back to the player's facing, with movement
       free again during the ease. Nothing else about the frame changes — the
       caller keeps ticking enemies, pickups and props as in roaming mode. */
    int circle = (btn & PAD_CIRCLE) ? 1 : 0;
    tap_frame = 0;
    if (circle) {
        /* -1 (spent) stays put; otherwise count up to — and stop at — the hold
           threshold, which is what both the tap test and look_mode read. */
        if (circle_frames >= 0 && circle_frames < LOOK_HOLD_FRAMES) circle_frames++;
    } else {
        if (circle_frames > 0 && circle_frames < LOOK_HOLD_FRAMES) tap_frame = 1;
        circle_frames = 0;
    }
    look_mode = (circle_frames >= LOOK_HOLD_FRAMES && !aiming && !anchored) ? 1 : 0;
    if (look_mode) {
        if (btn & PAD_LEFT)  look_yaw   -= LOOK_SPEED;
        if (btn & PAD_RIGHT) look_yaw   += LOOK_SPEED;
        if (btn & PAD_UP)    look_pitch -= LOOK_SPEED;   /* +pitch looks DOWN */
        if (btn & PAD_DOWN)  look_pitch += LOOK_SPEED;
        if (look_yaw   >  LOOK_MAX_ANG) look_yaw   =  LOOK_MAX_ANG;
        if (look_yaw   < -LOOK_MAX_ANG) look_yaw   = -LOOK_MAX_ANG;
        if (look_pitch >  LOOK_MAX_ANG) look_pitch =  LOOK_MAX_ANG;
        if (look_pitch < -LOOK_MAX_ANG) look_pitch = -LOOK_MAX_ANG;
    } else {
        if (look_yaw   >  LOOK_RETURN) look_yaw   -= LOOK_RETURN;
        else if (look_yaw   < -LOOK_RETURN) look_yaw   += LOOK_RETURN;
        else look_yaw = 0;
        if (look_pitch >  LOOK_RETURN) look_pitch -= LOOK_RETURN;
        else if (look_pitch < -LOOK_RETURN) look_pitch += LOOK_RETURN;
        else look_pitch = 0;
    }
    if (look_mode) goto debug_toggle;   /* rooted while looking */

    if (aiming) {
        if (btn & PAD_UP)    aim_y -= AIM_MOVE_SPEED;
        if (btn & PAD_DOWN)  aim_y += AIM_MOVE_SPEED;
        if (btn & PAD_LEFT)  aim_x -= AIM_MOVE_SPEED;
        if (btn & PAD_RIGHT) aim_x += AIM_MOVE_SPEED;
        if (aim_x < AIM_MARGIN)                aim_x = AIM_MARGIN;
        if (aim_x > SCREEN_XRES - AIM_MARGIN)  aim_x = SCREEN_XRES - AIM_MARGIN;
        if (aim_y < AIM_MARGIN)                aim_y = AIM_MARGIN;
        { int32_t max_y = aim_max_y();
          if (aim_y > max_y)                   aim_y = max_y; }
        goto debug_toggle;   /* skip all walk/turn/sprint while aiming */
    }

    if (btn & PAD_LEFT)  cam_rot = (cam_rot - 32) & 4095;
    if (btn & PAD_RIGHT) cam_rot = (cam_rot + 32) & 4095;

    /* Sprint state machine ------------------------------------------------
       sprint_cooldown is a lock flag (0/1), not a countdown.
       Stamina refills at half rate (1 per 2 frames) whenever not sprinting;
       the bar is visible throughout. Sprint stays locked until bar is full. */
    static int sprint_regen_tick = 0;
    int sprinting = 0;
    /* Sprint only valid moving forward or turning — not backward or strafing */
    int sprint_dir_ok = (btn & (PAD_UP | PAD_LEFT | PAD_RIGHT)) &&
                        !(btn & PAD_DOWN) &&
                        !(btn & PAD_L1) &&
                        !(btn & PAD_R1);

    /* Poison locks sprint outright, independently of the stamina bar: the bar
       keeps refilling normally underneath so it is ready the moment the status
       wears off. draw_hud paints it red meanwhile. */
    if ((btn & PAD_CROSS) && !sprint_cooldown && !player_poisoned() &&
        sprint_stamina > 0 && sprint_dir_ok) {
        sprinting = 1;
        /* INFINITE STAMINA just skips the drain: the bar stays full, so it never
           hits 0 and the cooldown lock below can never arm. */
        if (!debug_opts[DBG_INFINITE_STAMINA]) {
            sprint_stamina--;
            if (sprint_stamina == 0)
                sprint_cooldown = 1;   /* lock sprint — bar exhausted */
        }
    } else {
        sprint_regen_tick++;
        if (sprint_regen_tick >= 2) {
            sprint_regen_tick = 0;
            if (sprint_stamina < SPRINT_STAMINA_MAX) {
                sprint_stamina++;
                if (sprint_stamina == SPRINT_STAMINA_MAX)
                    sprint_cooldown = 0;  /* unlock sprint — bar full */
            }
        }
    }

    /* Poison halves the walking pace. Sprint is already locked out above, so
       the sprinting branch is unreachable while poisoned. */
    int32_t speed = sprinting ? 20 : 12;
    if (player_poisoned()) speed /= 2;

    if (btn & PAD_UP) {
        cam_x += (isin(cam_rot) * speed) >> 12;
        cam_z += (icos(cam_rot) * speed) >> 12;
    }
    if (btn & PAD_DOWN) {
        cam_x -= (isin(cam_rot) * speed) >> 12;
        cam_z -= (icos(cam_rot) * speed) >> 12;
    }
    if (btn & PAD_L1) {
        cam_x -= (icos(cam_rot) * speed) >> 12;
        cam_z += (isin(cam_rot) * speed) >> 12;
    }
    if (btn & PAD_R1) {
        cam_x += (icos(cam_rot) * speed) >> 12;
        cam_z -= (isin(cam_rot) * speed) >> 12;
    }

    /* Footsteps: accumulate travelled distance while any translation key is
       held, and play a step each stride. Alternating STEP1/STEP2 gives the
       usual left/right gait; the accumulator ties cadence to speed, so a
       sprint's longer strides trigger steps proportionally faster. */
    {
        static int32_t step_dist = 0;
        static int     step_foot = 0;
        const  int32_t STEP_STRIDE = 280;  /* world units per footstep */
        if (btn & (PAD_UP | PAD_DOWN | PAD_L1 | PAD_R1)) {
            step_dist += speed;
            if (step_dist >= STEP_STRIDE) {
                step_dist -= STEP_STRIDE;
                sound_play(step_foot ? SFX_STEP2 : SFX_STEP1);
                step_foot ^= 1;
            }
        } else {
            step_dist = 0;   /* reset so the next move starts on a fresh step */
        }
    }

debug_toggle:
    {
        /* SELECT cycles debug:
             0 = off
             1 = PERF METER ONLY. Three short lines of text and nothing else —
                 this is the level to read VB in.
             2 = the authoring overlay: coordinates, compass, held keys. Useful
                 for placing things, NOT for measuring (see below).
             3 = adds the heavy collision visualisation (purple walls etc).
             4..7 = the PERF METER plus one ISOLATION SWITCH each, for bisecting
                 where a frame's time actually goes without a rebuild per guess:
                   4  room mesh not drawn (the baseline cost of everything else)
                   5  side-plane frustum cull OFF - does it pay for itself?
                   6  view distance 2000
                   7  view distance 1800
                 Only Maze One honours them today (maze_one.c); every other room
                 ignores them and just shows the meter.

           These found the Maze One regression: no-mesh read VB1 and no-sprites
           read VB2, so the room's geometry was the cost and the enemies in it
           were not, and a half view distance was enough to hold 60fps. The fog
           far distance moves WITH the ladder, so what you see at each rung is
           what the room would really look like at that setting rather than a
           hard pop at the cull.

           >>> LEVEL 1 EXISTS BECAUSE THE OVERLAY WAS DISTORTING ITS OWN
           READING. <<< The old level 1 bundled the perf text together with the
           coordinate panel, and that panel is not cheap: a 320x30 backing tile
           plus a seven-segment TILE per digit segment for X, Y, Z and the wall
           count, plus a 40-character compass tape and the key list — on the
           order of 250 extra primitives every frame. Its comment claimed it was
           "cheap, so VB reflects the real game", and that claim was wrong; Maze
           One reads VB2 with it up and VB1 without. An instrument that costs a
           vblank cannot be used to hunt a vblank, so the meter is now its own
           level with nothing else in it. */
        static int select_prev = 0;
        int select_held = (btn & PAD_SELECT) ? 1 : 0;
        if (select_held && !select_prev) debug_mode = (debug_mode + 1) % 8;
        select_prev = select_held;
    }

    /* Fold the look offset back into the camera. cam_rot carries the yaw so
       every room and prop view matrix picks it up; cam_pitch is only ours when
       no puzzle has anchored the player and taken the camera. */
    cam_rot = (cam_rot + look_yaw) & 4095;
    if (!anchored) cam_pitch = look_pitch;
}

