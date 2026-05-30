#include <stdint.h>
#include <stddef.h>
#include <psxgte.h>
#include <psxpad.h>
#include "camera.h"
#include "collision.h"

int32_t cam_x   = 0;
int32_t cam_y   = 0;
int32_t cam_vy  = 0;
int32_t cam_z   = 0;
int32_t cam_rot = 0;

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

void update_camera(void) {
    if (!pad_buff_len[0]) return;

    PadResponse *pad = (PadResponse *)pad_buff[0];
    uint16_t btn = ~pad->btn;

    if (btn & PAD_LEFT)  cam_rot = (cam_rot - 32) & 4095;
    if (btn & PAD_RIGHT) cam_rot = (cam_rot + 32) & 4095;

    int32_t speed = 12;
    if (btn & PAD_UP) {
        cam_x += (isin(cam_rot) * speed) >> 12;
        cam_z += (icos(cam_rot) * speed) >> 12;
    }
    if (btn & PAD_DOWN) {
        cam_x -= (isin(cam_rot) * speed) >> 12;
        cam_z -= (icos(cam_rot) * speed) >> 12;
    }

#ifdef DEBUG_COLLISION
    {
        static int select_prev = 0;
        int select_held = (btn & PAD_SELECT) ? 1 : 0;
        if (select_held && !select_prev) debug_mode ^= 1;
        select_prev = select_held;
    }
#endif
}

