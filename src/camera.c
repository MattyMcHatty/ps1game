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
#include "title.h"

int32_t cam_x   = 0;
int32_t cam_y   = 0;
int32_t cam_vy  = 0;
int32_t cam_z   = 0;
int32_t cam_rot = 0;
int sprint_stamina  = SPRINT_STAMINA_MAX;
int sprint_cooldown = 0;

/* Aiming: hold L2 with the grave-olver to reposition the crosshair. While
   aiming the player is rooted (walk + turn disabled) and the d-pad drives the
   crosshair; releasing L2 recentres it and frees movement. aim_x/aim_y are the
   crosshair's live screen position, read by the gun's hitscan and reticule. */
#define AIM_REST_X     (SCREEN_XRES / 2)
#define AIM_REST_Y     (SCREEN_YRES / 2 + 20)   /* rests a touch below centre */
#define AIM_MOVE_SPEED 3     /* crosshair pixels per frame                     */
#define AIM_MARGIN     10    /* keep the crosshair this far from screen edges  */
#define AIM_MAX_Y   ((SCREEN_YRES * 4) / 5)  /* lowest crosshair: 20% up from bottom */
int     aiming = 0;
int32_t aim_x  = AIM_REST_X;
int32_t aim_y  = AIM_REST_Y;

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Load the GTE with the current camera view matrix (rotation about Y by
   -cam_rot, then translate by -cam). Any world-space GTE projection (gte_rtps)
   done afterwards uses the camera view — used by the debug overlays, which run
   after the weapon/HUD draws have left their own matrices in the GTE. */
void camera_set_view_matrix(void) {
    MATRIX  m;
    SVECTOR neg_rot = {0, (short)-cam_rot, 0, 0};
    VECTOR  trans   = {-cam_x, -cam_y, -cam_z};
    RotMatrix(&neg_rot, &m);
    ApplyMatrixLV(&m, &trans, &trans);
    m.t[0] = trans.vx; m.t[1] = trans.vy; m.t[2] = trans.vz;
    gte_SetRotMatrix(&m);
    gte_SetTransMatrix(&m);
}

void update_camera(void) {
    if (game_state == STATE_MENU) return;
    if (!pad_buff_len[0]) return;

    PadResponse *pad = (PadResponse *)pad_buff[0];
    uint16_t btn = ~pad->btn;

    /* Aiming mode: the d-pad drives the crosshair and the player is frozen.
       Starting a reload cancels aiming, and a reload can't be aimed through — you
       must release L2 and press it again after it finishes (edge-triggered start,
       so holding L2 across the whole reload does not re-arm the aim). */
    static int aim_prev_held = 0;
    int aim_held  = (btn & PAD_L2) ? 1 : 0;
    int has_gun   = (current_weapon == WEAPON_GRAVEOLVER);
    int reloading = graveolver_is_reloading() || weapon_switching();

    if (aiming) {
        if (reloading || !has_gun || !aim_held) {
            aiming = 0;
            aim_x  = AIM_REST_X;
            aim_y  = AIM_REST_Y;
        }
    } else if (has_gun && !reloading && aim_held && !aim_prev_held) {
        aiming = 1;
    }
    aim_prev_held = aim_held;

    if (aiming) {
        if (btn & PAD_UP)    aim_y -= AIM_MOVE_SPEED;
        if (btn & PAD_DOWN)  aim_y += AIM_MOVE_SPEED;
        if (btn & PAD_LEFT)  aim_x -= AIM_MOVE_SPEED;
        if (btn & PAD_RIGHT) aim_x += AIM_MOVE_SPEED;
        if (aim_x < AIM_MARGIN)                aim_x = AIM_MARGIN;
        if (aim_x > SCREEN_XRES - AIM_MARGIN)  aim_x = SCREEN_XRES - AIM_MARGIN;
        if (aim_y < AIM_MARGIN)                aim_y = AIM_MARGIN;
        if (aim_y > AIM_MAX_Y)                 aim_y = AIM_MAX_Y;
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

    if ((btn & PAD_CROSS) && !sprint_cooldown && sprint_stamina > 0 && sprint_dir_ok) {
        sprinting = 1;
        sprint_stamina--;
        if (sprint_stamina == 0)
            sprint_cooldown = 1;   /* lock sprint — bar exhausted */
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

    int32_t speed = sprinting ? 20 : 12;

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

debug_toggle:
    {
        static int select_prev = 0;
        int select_held = (btn & PAD_SELECT) ? 1 : 0;
        if (select_held && !select_prev) debug_mode ^= 1;
        select_prev = select_held;
    }
}

