#include <stdint.h>
#include <stddef.h>
#include <psxgte.h>
#include <psxpad.h>
#include "camera.h"

int32_t cam_x   = 0;
int32_t cam_z   = -500;
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
}

void apply_collision(void) {
    int32_t margin = 400;

    if (cam_x < -1800 + margin) cam_x = -1800 + margin;
    if (cam_x >  1800 - margin) cam_x =  1800 - margin;
    if (cam_z < -1800 + margin) cam_z = -1800 + margin;
    if (cam_z >  1800 - margin) cam_z =  1800 - margin;
}
