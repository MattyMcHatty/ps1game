#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

extern int32_t cam_x;
extern int32_t cam_z;
extern int32_t cam_rot;

void update_camera(void);
void apply_collision(void);
void apply_vampire_collision(void);

#endif
