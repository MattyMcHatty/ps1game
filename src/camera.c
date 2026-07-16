#include <stdint.h>
#include <stddef.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "camera.h"
#include "collision.h"
#include "title.h"

int32_t cam_x   = 0;
int32_t cam_y   = 0;
int32_t cam_vy  = 0;
int32_t cam_z   = 0;
int32_t cam_rot = 0;
int sprint_stamina  = SPRINT_STAMINA_MAX;
int sprint_cooldown = 0;

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

    {
        static int select_prev = 0;
        int select_held = (btn & PAD_SELECT) ? 1 : 0;
        if (select_held && !select_prev) debug_mode ^= 1;
        select_prev = select_held;
    }
}

